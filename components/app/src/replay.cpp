#include "replay.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include "gbx/audio.h"
#include "gbx/platform.h"

namespace gbx::app {
namespace {

constexpr const char* kTag = "replay";
constexpr char kMagic[8] = {'G', 'B', 'X', 'K', 'E', 'Y', 'S', '2'};
// The version before the GBC byte, which plays in a GB.
constexpr char kMagic1[8] = {'G', 'B', 'X', 'K', 'E', 'Y', 'S', '1'};
// Ten minutes of frames: a measurement needs a representative scene, not a
// whole game. The buffers take about 470 KiB of PSRAM in profiling builds.
constexpr uint32_t kFrames = 36000;
// Frames run after a replay's last one before its report, so that the
// worker has delivered the last frames' work.
constexpr uint32_t kReportDelay = 30;

class Writer {
public:
    explicit Writer(std::span<uint8_t> out) : out_(out) {}
    void bytes(const void* data, size_t size) {
        if (used_ + size <= out_.size()) std::memcpy(out_.data() + used_, data, size);
        used_ += size;
    }
    void u8(uint8_t value) { bytes(&value, 1); }
    void u16(uint16_t value) { u8(static_cast<uint8_t>(value)); u8(static_cast<uint8_t>(value >> 8)); }
    void u32(uint32_t value) { u16(static_cast<uint16_t>(value)); u16(static_cast<uint16_t>(value >> 16)); }
    size_t used() const { return used_; }
private:
    std::span<uint8_t> out_;
    size_t used_ = 0;
};

class Reader {
public:
    explicit Reader(std::span<const uint8_t> in) : in_(in) {}
    bool bytes(void* out, size_t size) {
        if (used_ + size > in_.size()) return false;
        std::memcpy(out, in_.data() + used_, size);
        used_ += size;
        return true;
    }
    const uint8_t* take(size_t size) {
        if (used_ + size > in_.size()) return nullptr;
        used_ += size;
        return in_.data() + used_ - size;
    }
    bool u8(uint8_t& value) { return bytes(&value, 1); }
    bool u16(uint16_t& value) {
        uint8_t b[2];
        if (!bytes(b, 2)) return false;
        value = static_cast<uint16_t>(b[0] | b[1] << 8);
        return true;
    }
    bool u32(uint32_t& value) {
        uint16_t low, high;
        if (!u16(low) || !u16(high)) return false;
        value = low | static_cast<uint32_t>(high) << 16;
        return true;
    }
private:
    std::span<const uint8_t> in_;
    size_t used_ = 0;
};

// Values sorted in place: their mean, 95th percentile and maximum.
struct Spread { double mean, p95, max; };
Spread spread(uint32_t* values, size_t n) {
    if (!n) return {0, 0, 0};
    std::sort(values, values + n);
    uint64_t sum = 0;
    for (size_t i = 0; i < n; ++i) sum += values[i];
    return {static_cast<double>(sum) / n, static_cast<double>(values[(n * 95 + 99) / 100 - 1]),
            static_cast<double>(values[n - 1])};
}

// FNV-1a over the machine state a replay leaves behind.
class Hash {
public:
    void bytes(const void* data, size_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i) value = (value ^ p[i]) * 0x100000001b3ull;
    }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) bytes(reinterpret_cast<const uint8_t*>(&v) + i, 1); }
    uint64_t value = 0xcbf29ce484222325ull;
};

uint64_t state_hash(gb::System& system) {
    Hash h;
    const auto& c = system.cpu;
    const uint8_t cpu[]{c.a, c.f, c.b, c.c, c.d, c.e, c.h, c.l, static_cast<uint8_t>(c.sp), static_cast<uint8_t>(c.sp >> 8),
                        static_cast<uint8_t>(c.pc), static_cast<uint8_t>(c.pc >> 8), c.ime, c.ei_pending, c.halt_bug,
                        static_cast<uint8_t>(c.mode)};
    h.bytes(cpu, sizeof cpu);
    auto& m = system.memory;
    h.u64(m.clock.now);
    h.u64(m.frames);
    h.bytes(m.wram.data(), m.wram.size());
    h.bytes(m.vram.data(), m.vram.size());
    h.bytes(m.oam.data(), m.oam.size());
    h.bytes(m.io.data(), m.io.size());
    h.bytes(m.high.data(), m.high.size());
    const auto ram = system.cartridge_ram();
    h.bytes(ram.data(), ram.size());
    if (system.has_rtc()) {
        const auto r = system.rtc();
        const uint8_t clock[]{r.seconds, r.minutes, r.hours, static_cast<uint8_t>(r.days), static_cast<uint8_t>(r.days >> 8),
                              r.halt, r.carry};
        h.bytes(clock, sizeof clock);
        h.u64(r.subsecond);
        h.bytes(r.latched.data(), r.latched.size());
    }
    return h.value;
}

template<class T>
T* allocate(size_t count) {
    return static_cast<T*>(platform::allocate_external(count * sizeof(T)));
}

}  // namespace

Replay::~Replay() {
    release();
    platform::free_external(keys_);
    platform::free_external(run_us_);
    platform::free_external(work_us_);
    platform::free_external(scratch_);
}

void Replay::request_record(const char* file, bool cgb) {
    std::lock_guard lock(mutex_);
    requested_ = Mode::recording;
    request_cgb_ = cgb;
    std::snprintf(request_file_, sizeof request_file_, "%s", file);
}

void Replay::request_play(const char* file) {
    std::lock_guard lock(mutex_);
    requested_ = Mode::playing;
    std::snprintf(request_file_, sizeof request_file_, "%s", file);
}

bool Replay::read_path(const char* file, char* path, size_t size, bool& cgb) {
    // The console task's stack has no room for it, next to a card path.
    static std::array<uint8_t, sizeof kMagic + 11 + platform::files::kMaxPath> start;
    const int64_t n = platform::files::read_start(file, start);
    if (n < 0) return false;
    Reader in({start.data(), std::min<size_t>(static_cast<size_t>(n), start.size())});
    char magic[sizeof kMagic];
    uint32_t frames, mark;
    uint16_t length;
    uint8_t machine = 0;
    if (!in.bytes(magic, sizeof magic) || !in.u32(frames) || !in.u32(mark)) return false;
    const bool first = std::memcmp(magic, kMagic1, sizeof magic) == 0;
    if ((!first && std::memcmp(magic, kMagic, sizeof magic) != 0) || (!first && !in.u8(machine)) || !in.u16(length) ||
        length >= size || !in.bytes(path, length)) {
        return false;
    }
    path[length] = 0;
    cgb = machine == 1;
    return true;
}

void Replay::release() {
    // The worker stops timing frames before the buffers change.
    first_id_.store(0, std::memory_order_release);
    platform::free_external(ram_);
    ram_ = nullptr;
    ram_size_ = 0;
    mode_ = Mode::off;
    report_in_ = 0;
}

bool Replay::take(std::span<const uint8_t> rom, const char* rom_path) {
    release();
    {
        std::lock_guard lock(mutex_);
        mode_ = requested_;
        cgb_ = request_cgb_;
        requested_ = Mode::off;
        std::memcpy(file_, request_file_, sizeof file_);
    }
    mark_requested_.store(false);
    stop_requested_.store(false);
    if (mode_ == Mode::off) return true;
    if (!keys_) keys_ = allocate<uint8_t>(kFrames);
    if (!run_us_) run_us_ = allocate<uint32_t>(kFrames);
    if (!scratch_) scratch_ = allocate<uint32_t>(kFrames);
    if (!work_us_) {
        work_us_ = allocate<std::atomic<uint32_t>>(kFrames);
        for (uint32_t i = 0; work_us_ && i < kFrames; ++i) new (&work_us_[i]) std::atomic<uint32_t>(0);
    }
    if (!keys_ || !run_us_ || !scratch_ || !work_us_) {
        platform::log(platform::LogLevel::warn, kTag, "not enough memory");
        mode_ = Mode::off;
        return false;
    }
    frames_ = mark_ = frame_ = 0;
    if (mode_ == Mode::recording) {
        std::snprintf(rom_path_, sizeof rom_path_, "%s", rom_path);
        std::copy_n(rom.begin() + 0x134, kHeaderBytes, header_.begin());
        return true;
    }
    if (!load_file(rom)) {
        mode_ = Mode::off;
        return false;
    }
    return true;
}

bool Replay::load_file(std::span<const uint8_t> rom) {
    const int64_t size = platform::files::size(file_);
    auto* bytes = size > 0 ? allocate<uint8_t>(static_cast<size_t>(size)) : nullptr;
    const bool read = bytes && platform::files::read(file_, {bytes, static_cast<size_t>(size)});
    Reader in({bytes, read ? static_cast<size_t>(size) : 0});
    char magic[sizeof kMagic];
    uint16_t length = 0;
    uint32_t ram_size = 0;
    uint8_t clock = 0, machine = 0;
    bool valid = in.bytes(magic, sizeof magic) && in.u32(frames_) && in.u32(mark_);
    const bool first = valid && std::memcmp(magic, kMagic1, sizeof magic) == 0;
    valid = valid && (first || (std::memcmp(magic, kMagic, sizeof magic) == 0 && in.u8(machine))) && in.u16(length) &&
            length < sizeof rom_path_ && in.bytes(rom_path_, length) && in.bytes(header_.data(), header_.size()) &&
            in.u32(ram_size);
    cgb_ = machine == 1;
    rom_path_[valid ? length : 0] = 0;
    const uint8_t* ram = valid ? in.take(ram_size) : nullptr;
    valid = ram && in.u8(clock);
    if (valid && clock) {
        uint16_t days = 0;
        uint32_t subsecond = 0;
        uint8_t flags = 0;
        valid = in.u8(rtc_.seconds) && in.u8(rtc_.minutes) && in.u8(rtc_.hours) && in.u16(days) && in.u8(flags) &&
                in.u32(subsecond) && in.bytes(rtc_.latched.data(), rtc_.latched.size());
        rtc_.days = days;
        rtc_.halt = flags & 1;
        rtc_.carry = flags & 2;
        rtc_.subsecond = subsecond;
    }
    has_rtc_ = clock != 0;
    const uint8_t* keys = valid ? in.take(frames_) : nullptr;
    const char* problem = !read ? "cannot read the file" :
        !keys || !frames_ || frames_ > kFrames || mark_ > frames_ ? "not a recording" :
        !std::equal(header_.begin(), header_.end(), rom.begin() + 0x134) ? "recorded with another ROM" : nullptr;
    if (!problem && ram_size) {
        ram_ = allocate<uint8_t>(ram_size);
        if (!ram_) problem = "not enough memory";
        else std::memcpy(ram_, ram, ram_size);
    }
    if (!problem) {
        ram_size_ = ram_size;
        std::memcpy(keys_, keys, frames_);
        // A mark at the end leaves nothing to time: time the whole replay.
        if (mark_ == frames_) mark_ = 0;
    }
    platform::free_external(bytes);
    if (problem) platform::log(platform::LogLevel::warn, kTag, "play %s: %s", file_, problem);
    return !problem;
}

void Replay::started(gb::System& system) {
    if (mode_ == Mode::recording) {
        const auto ram = system.cartridge_ram();
        ram_size_ = ram.size();
        ram_ = ram_size_ ? allocate<uint8_t>(ram_size_) : nullptr;
        if (ram_size_ && !ram_) {
            platform::log(platform::LogLevel::warn, kTag, "record: not enough memory");
            release();
            return;
        }
        std::copy(ram.begin(), ram.end(), ram_);
        has_rtc_ = system.has_rtc();
        if (has_rtc_) rtc_ = system.rtc();
        platform::log(platform::LogLevel::warn, kTag, "record %s: %s", file_, rom_path_);
    } else if (mode_ == Mode::playing) {
        system.load_cartridge_ram({ram_, ram_size_});
        if (has_rtc_ && system.has_rtc()) system.set_rtc(rtc_);
        platform::log(platform::LogLevel::warn, kTag, "play %s: %lu frames, timed from %lu", file_,
                      static_cast<unsigned long>(frames_), static_cast<unsigned long>(mark_));
    }
}

uint8_t Replay::keys(uint8_t live, gb::System& system) {
    if (report_in_ && --report_in_ == 0) finish(system);
    if (mode_ == Mode::recording) {
        if (stop_requested_.exchange(false)) {
            write_file();
            release();
            return live;
        }
        if (mark_requested_.exchange(false)) {
            mark_ = frame_;
            platform::log(platform::LogLevel::warn, kTag, "record: mark at frame %lu", static_cast<unsigned long>(mark_));
        }
        if (frame_ == kFrames) {
            platform::log(platform::LogLevel::warn, kTag, "record: full");
            write_file();
            release();
            return live;
        }
        keys_[frame_++] = live;
        return live;
    }
    if (mode_ != Mode::playing) return live;
    // From the last frame to the report, the game runs without keys.
    if (frame_ == frames_) return 0;
    if (frame_ == mark_) {
        start_us_ = platform::monotonic_us();
        ticks_ = instructions_ = 0;
        start_ = count(system);
        for (uint32_t i = 0; i < frames_ - mark_; ++i) work_us_[i].store(0, std::memory_order_relaxed);
        first_id_.store(system.memory.frames + 1, std::memory_order_release);
    }
    return keys_[frame_++];
}

void Replay::ran(gb::System& system, uint64_t ticks, uint64_t instructions, uint32_t run_us) {
    if (mode_ != Mode::playing || !frame_ || frame_ - 1 < mark_ || report_in_) return;
    run_us_[frame_ - 1 - mark_] = run_us;
    ticks_ += ticks;
    instructions_ += instructions;
    if (frame_ != frames_) return;
    end_us_ = platform::monotonic_us();
    end_ = count(system);
    hash_ = state_hash(system);
    last_id_ = system.memory.frames;
    report_in_ = kReportDelay;
}

Replay::Counts Replay::count(gb::System& system) {
    Counts counts;
    counts.underruns = platform::audio::underruns();
    if (const auto* jit = system.jit()) {
        counts.jit[0] = jit->compiled_blocks(); counts.jit[1] = jit->restored_blocks();
        counts.jit[2] = jit->evictions(); counts.jit[3] = jit->invalidations();
        for (auto fallback : jit->fallbacks()) counts.fallbacks += fallback;
        counts.code = static_cast<uint32_t>(jit->code_bytes());
    }
    counts.offload = gb::offload_counters;
    counts.counters = gb::sm83_jit_counters;
    return counts;
}

void Replay::work(uint64_t id, uint32_t us) {
    const uint64_t first = first_id_.load(std::memory_order_acquire);
    if (first && id >= first && id - first < kFrames) work_us_[id - first].store(us, std::memory_order_relaxed);
}

void Replay::finish(gb::System& system) {
    const uint64_t first = first_id_.exchange(0, std::memory_order_acq_rel);
    const uint32_t timed = frames_ - mark_;
    const auto run = spread(run_us_, timed);
    uint32_t samples = 0;
    for (uint64_t id = first; first && id <= last_id_ && id - first < kFrames; ++id) {
        if (const uint32_t us = work_us_[id - first].load(std::memory_order_relaxed)) scratch_[samples++] = us;
    }
    const auto work = spread(scratch_, samples);
    uint32_t jit[4];
    for (int i = 0; i < 4; ++i) jit[i] = end_.jit[i] - start_.jit[i];
    const double emulated = static_cast<double>(ticks_) / kTicksPerSecond;
    const double wall = (end_us_ - start_us_) / 1e6;
    // Three lines: a log line holds at most 255 characters.
    platform::log(platform::LogLevel::warn, kTag,
                  "play %s %s: frames %lu timed %lu, %.2f s in %.2f s (%.1f %%), underruns %lu",
                  file_, system.backend() == gb::Backend::jit ? "jit" : "int", static_cast<unsigned long>(frames_),
                  static_cast<unsigned long>(timed), emulated, wall, wall > 0 ? emulated * 100 / wall : 0.0,
                  static_cast<unsigned long>(end_.underruns - start_.underruns));
    platform::log(platform::LogLevel::warn, kTag,
                  "play %s: run avg %.3f p95 %.3f max %.3f ms; frame avg %.3f p95 %.3f max %.3f ms of %lu",
                  file_, run.mean / 1000, run.p95 / 1000, run.max / 1000, work.mean / 1000, work.p95 / 1000,
                  work.max / 1000, static_cast<unsigned long>(samples));
    platform::log(platform::LogLevel::warn, kTag,
                  "play %s: jit compiled %lu restored %lu evicted %lu invalidated %lu fallbacks %llu code %lu; "
                  "hash %016llx",
                  file_, static_cast<unsigned long>(jit[0]), static_cast<unsigned long>(jit[1]),
                  static_cast<unsigned long>(jit[2]), static_cast<unsigned long>(jit[3]),
                  static_cast<unsigned long long>(end_.fallbacks - start_.fallbacks),
                  static_cast<unsigned long>(end_.code),
                  static_cast<unsigned long long>(hash_));
    const auto &offload = end_.offload, &before = start_.offload;
    platform::log(platform::LogLevel::warn, kTag, "play %s: worker waits rows %lu %llu us, sound %lu %llu us", file_,
                  static_cast<unsigned long>(offload.waits[0] - before.waits[0]),
                  static_cast<unsigned long long>(offload.wait_us[0] - before.wait_us[0]),
                  static_cast<unsigned long>(offload.waits[1] - before.waits[1]),
                  static_cast<unsigned long long>(offload.wait_us[1] - before.wait_us[1]));
    if (system.jit()) {
        // The JIT's own counters, see Sm83JitCounters.
        const auto &now = end_.counters, &then = start_.counters;
        const auto d = [&](uint32_t gb::Sm83JitCounters::*counter) {
            return static_cast<unsigned long>(now.*counter - then.*counter);
        };
        platform::log(platform::LogLevel::warn, kTag,
                      "play %s: execute %lu enter %lu hit %lu mid %lu interpreted %lu compiles %lu; cf dynamic %lu "
                      "linked %lu none %lu budget %lu", file_, d(&gb::Sm83JitCounters::execute),
                      d(&gb::Sm83JitCounters::enter), d(&gb::Sm83JitCounters::hit), d(&gb::Sm83JitCounters::mid),
                      d(&gb::Sm83JitCounters::interpreted), d(&gb::Sm83JitCounters::compiles),
                      d(&gb::Sm83JitCounters::cf_dynamic), d(&gb::Sm83JitCounters::cf_linked),
                      d(&gb::Sm83JitCounters::cf_none), d(&gb::Sm83JitCounters::budget));
        unsigned long exits[8];
        for (int i = 0; i < 8; ++i) exits[i] = static_cast<unsigned long>(now.exits[i] - then.exits[i]);
        platform::log(platform::LogLevel::warn, kTag,
                      "play %s: exits limit %lu flow %lu suspend %lu fallback %lu event %lu refetch %lu; read %lu "
                      "write %lu group %lu idle %lu deadline %lu", file_, exits[0], exits[1], exits[2], exits[3],
                      exits[4], exits[6], d(&gb::Sm83JitCounters::read), d(&gb::Sm83JitCounters::write),
                      d(&gb::Sm83JitCounters::group), d(&gb::Sm83JitCounters::idle), d(&gb::Sm83JitCounters::deadline));
        platform::log(platform::LogLevel::warn, kTag, "play %s: interpreted %lu of %llu instructions", file_,
                      d(&gb::Sm83JitCounters::interpreted_instructions),
                      static_cast<unsigned long long>(instructions_));
    }
    release();
}

void Replay::write_file() {
    const size_t size = sizeof kMagic + 11 + std::strlen(rom_path_) + kHeaderBytes + 4 + ram_size_ + 1 + 15 + frame_;
    auto* bytes = allocate<uint8_t>(size);
    if (!bytes) {
        platform::log(platform::LogLevel::warn, kTag, "record: not enough memory to write %s", file_);
        return;
    }
    Writer out({bytes, size});
    out.bytes(kMagic, sizeof kMagic);
    out.u32(frame_);
    out.u32(mark_);
    out.u8(cgb_ ? 1 : 0);
    out.u16(static_cast<uint16_t>(std::strlen(rom_path_)));
    out.bytes(rom_path_, std::strlen(rom_path_));
    out.bytes(header_.data(), header_.size());
    out.u32(static_cast<uint32_t>(ram_size_));
    out.bytes(ram_, ram_size_);
    out.u8(has_rtc_);
    if (has_rtc_) {
        out.u8(rtc_.seconds); out.u8(rtc_.minutes); out.u8(rtc_.hours); out.u16(rtc_.days);
        out.u8(static_cast<uint8_t>((rtc_.halt ? 1 : 0) | (rtc_.carry ? 2 : 0)));
        out.u32(rtc_.subsecond);
        out.bytes(rtc_.latched.data(), rtc_.latched.size());
    }
    out.bytes(keys_, frame_);
    const bool written = platform::files::make_parents(file_) == platform::files::Error::none &&
                         platform::files::write(file_, {bytes, out.used()}) == platform::files::Error::none;
    platform::free_external(bytes);
    platform::log(platform::LogLevel::warn, kTag, written ? "record %s: %lu frames, mark %lu" : "record %s: cannot write",
                  file_, static_cast<unsigned long>(frame_), static_cast<unsigned long>(mark_));
}

}  // namespace gbx::app
