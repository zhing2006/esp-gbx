#include "saves.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>

#include "rom_info.h"
#include "settings.h"

// A save file, little endian:
//
//   0   8  magic "GBXCART\0"
//   8   2  version, 1
//  10   2  header size, 80
//  12   4  sequence: the newer of the two files has the larger one, compared
//          as a signed difference so that it can wrap
//  16   4  RAM bytes that follow the header
//  20   4  flags: bit 0, the clock below is the cartridge's; bit 1, the
//          board's time below was trusted
//  24  24  the ROM's identity, see gb::rom_identity() and gba::rom_identity()
//  48  20  the cartridge's clock. A GB's MBC3: seconds, minutes, hours,
//          halt, days (2), carry, 0, subsecond ticks (4), the five latched
//          registers, three zeros. A GBA's S-3511: year, month, day, day of
//          week, hour (0-23), minute, second, the control register,
//          subsecond cycles (4), eight zeros, see gba::RtcState.
//  68   8  the board's time when the save was taken, microseconds since
//          1970 (platform::wall_time())
//  76   1  system: 0 GB or GBC, 1 GBA
//  77   1  a GBA's save memory type, gba::BackupType; 0 on a GB
//  78   2  zero
//  80   n  cartridge RAM, or a GBA's save memory
//  80+n 4  CRC-32 (the zlib polynomial) of everything before it
//
// Files written before the GBA had saves hold zeros at 76-79, which read as
// a GB's.
//
// Two files hold the save, <stem>.0.sav and <stem>.1.sav under saves/, in
// the ROM's folder below roms/. Each write goes to the older one and is
// flushed before it counts, so a write cut off by power loss leaves the
// other one whole. Renaming a file over another is not atomic on FAT, so the
// files keep their names.
namespace gbx::app {
namespace {

using platform::files::Error;

constexpr const char* kTag = "saves";
constexpr char kMagic[8] = {'G', 'B', 'X', 'C', 'A', 'R', 'T', '\0'};
constexpr uint16_t kVersion = 1;
constexpr size_t kHeader = 80;
constexpr size_t kTrailer = 4;
constexpr uint32_t kHasRtc = 1, kWallTrusted = 2;
// The largest cartridge RAM is 128 KiB (Pan Docs, The Cartridge Header, 0149),
// and so is a GBA's 1 Mbit Flash (GBATEK "GBA Cart Backup IDs").
constexpr size_t kMaxFile = kHeader + 128 * 1024 + kTrailer;
// Games write a save over a few frames to a few seconds. A second after the
// last change puts such a burst in one write. A game that keeps writing, as
// some do to their RAM all the time, is still written ten seconds after its
// first change, which bounds what power loss can take.
constexpr uint64_t kQuietUs = 1'000'000;
constexpr uint64_t kMaxDirtyUs = 10'000'000;
constexpr uint64_t kSettingsDelayUs = 1'000'000;
constexpr uint32_t kForever = UINT32_MAX;

constexpr std::array<uint32_t, 256> kCrcTable = [] {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
    return table;
}();

void put16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void put32(uint8_t* p, uint32_t v) {
    put16(p, static_cast<uint16_t>(v));
    put16(p + 2, static_cast<uint16_t>(v >> 16));
}

void put64(uint8_t* p, uint64_t v) {
    put32(p, static_cast<uint32_t>(v));
    put32(p + 4, static_cast<uint32_t>(v >> 32));
}

uint16_t get16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | p[1] << 8);
}

uint32_t get32(const uint8_t* p) {
    return get16(p) | static_cast<uint32_t>(get16(p + 2)) << 16;
}

uint64_t get64(const uint8_t* p) {
    return get32(p) | static_cast<uint64_t>(get32(p + 4)) << 32;
}

// A whole file of this version, by its size, header and check.
bool valid(std::span<const uint8_t> file) {
    if (file.size() < kHeader + kTrailer || std::memcmp(file.data(), kMagic, sizeof kMagic) != 0 ||
        get16(&file[8]) != kVersion || get16(&file[10]) != kHeader ||
        file.size() != kHeader + get32(&file[16]) + kTrailer) {
        return false;
    }
    return crc32(file.first(file.size() - kTrailer)) == get32(&file[file.size() - kTrailer]);
}

const char* reason(Error e) {
    return e == Error::full ? "the SD card is full." : "the SD card reported an error.";
}

}  // namespace

void GbSaveCartridge::clock(std::span<uint8_t, kClockBytes> r) {
    const gb::RtcState rtc = system_->rtc();
    std::fill(r.begin(), r.end(), uint8_t{0});
    r[0] = rtc.seconds;
    r[1] = rtc.minutes;
    r[2] = rtc.hours;
    r[3] = rtc.halt;
    put16(&r[4], rtc.days);
    r[6] = rtc.carry;
    put32(&r[8], rtc.subsecond);
    std::copy(rtc.latched.begin(), rtc.latched.end(), r.begin() + 12);
}

void GbSaveCartridge::set_clock(std::span<const uint8_t, kClockBytes> r) {
    gb::RtcState rtc;
    rtc.seconds = r[0];
    rtc.minutes = r[1];
    rtc.hours = r[2];
    rtc.halt = r[3];
    rtc.days = get16(&r[4]);
    rtc.carry = r[6];
    rtc.subsecond = std::min<uint32_t>(get32(&r[8]), kTicksPerSecond - 1);
    std::copy_n(r.begin() + 12, 5, rtc.latched.begin());
    system_->set_rtc(rtc);
}

void GbaSaveCartridge::clock(std::span<uint8_t, kClockBytes> r) {
    const gba::RtcState rtc = system_->rtc_state();
    std::fill(r.begin(), r.end(), uint8_t{0});
    std::copy(rtc.datetime.begin(), rtc.datetime.end(), r.begin());
    r[7] = rtc.control;
    put32(&r[8], rtc.subsecond);
}

void GbaSaveCartridge::set_clock(std::span<const uint8_t, kClockBytes> r) {
    gba::RtcState rtc;
    std::copy_n(r.begin(), rtc.datetime.size(), rtc.datetime.begin());
    rtc.control = r[7];
    rtc.subsecond = static_cast<uint32_t>(std::min<uint64_t>(get32(&r[8]), gba::kCyclesPerSecond - 1));
    system_->set_rtc(rtc);
}

uint32_t crc32(std::span<const uint8_t> data) {
    uint32_t c = 0xffffffffu;
    for (uint8_t b : data) c = kCrcTable[(c ^ b) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

std::string_view rom_stem(const char* rom_path) {
    std::string_view rel(rom_path);
    if (rel.starts_with("roms/")) {
        rel.remove_prefix(5);
    } else if (const size_t slash = rel.find_last_of("/\\"); slash != std::string_view::npos) {
        rel.remove_prefix(slash + 1);
    }
    const size_t slash = rel.rfind('/');
    const size_t dot = rel.rfind('.');
    return dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash) ? rel
                                                                                            : rel.substr(0, dot);
}

void save_path(const char* rom_path, int slot, char* out, size_t size) {
    const std::string_view stem = rom_stem(rom_path);
    std::snprintf(out, size, "saves/%.*s.%d.sav", static_cast<int>(stem.size()), stem.data(), slot);
}

bool Saves::start(Faults& faults) {
    faults_ = &faults;
    return wake_.initialize(1) &&
           task_.start(run_entry, this, 0, "gbx-saves", 6144, 2, platform::StackMemory::external);
}

void Saves::stop() {
    flush_settings();
    stopping_.store(true);
    wake_.signal();
    task_.join();
    stopping_.store(false);
}

bool Saves::open(const char* rom_path, std::span<const uint8_t, 24> identity, System system, size_t capacity,
                 SaveCartridge& cartridge, Opened& out) {
    close();
    out = {};
    const std::string_view path(rom_path);
    const size_t slash = path.find_last_of("/\\");
    const std::string_view file = slash == std::string_view::npos ? path : path.substr(slash + 1);

    const auto memory = cartridge.memory();
    const size_t ram_size = memory.size();
    capacity = std::max(capacity, ram_size);
    uint8_t* shadow = capacity ? static_cast<uint8_t*>(platform::allocate_external(capacity)) : nullptr;
    uint8_t* buffer = static_cast<uint8_t*>(platform::allocate_external(kHeader + capacity + kTrailer));
    if ((capacity && !shadow) || !buffer) {
        platform::free_external(shadow);
        platform::free_external(buffer);
        return false;
    }

    char paths[2][sizeof paths_[0]];
    for (int slot = 0; slot < 2; ++slot) save_path(rom_path, slot, paths[slot], sizeof paths[slot]);

    // Read both and keep the newer valid one. The card is read without the
    // mutex, as state() is asked on every update and must not wait for it.
    // A file that cannot be read is not taken as missing or damaged: the
    // other one may be older, and writes would go over this one.
    enum class Read : uint8_t { done, unreadable, no_memory } read = Read::done;
    bool any = false;
    int best = -1;
    uint32_t best_sequence = 0;
    uint8_t* best_file = nullptr;
    for (int slot = 0; slot < 2; ++slot) {
        const int64_t size = platform::files::size(paths[slot]);
        if (size == platform::files::kNoFile) continue;
        any = true;
        if (size >= 0 && (size < static_cast<int64_t>(kHeader + kTrailer) || size > static_cast<int64_t>(kMaxFile))) {
            platform::log(platform::LogLevel::warn, kTag, "%s fails its check", paths[slot]);
            continue;
        }
        uint8_t* data = nullptr;
        if (size >= 0 && !(data = static_cast<uint8_t*>(platform::allocate_external(static_cast<size_t>(size))))) {
            read = Read::no_memory;
            break;
        }
        const std::span<uint8_t> bytes(data, data ? static_cast<size_t>(size) : 0);
        if (size < 0 || !platform::files::read(paths[slot], bytes)) {
            platform::log(platform::LogLevel::warn, kTag, "%s cannot be read", paths[slot]);
            platform::free_external(data);
            read = Read::unreadable;
            break;
        }
        if (!valid(bytes)) {
            platform::log(platform::LogLevel::warn, kTag, "%s fails its check", paths[slot]);
            platform::free_external(data);
            continue;
        }
        const uint32_t sequence = get32(&data[12]);
        if (best < 0 || static_cast<int32_t>(sequence - best_sequence) > 0) {
            platform::free_external(best_file);
            best = slot;
            best_sequence = sequence;
            best_file = data;
        } else {
            platform::free_external(data);
        }
    }
    if (read != Read::done) {
        platform::free_external(best_file);
        platform::free_external(shadow);
        platform::free_external(buffer);
        if (read == Read::no_memory) return false;
        out.found = Found::unreadable;
        return true;
    }

    std::lock_guard lock(mutex_);
    std::memcpy(paths_, paths, sizeof paths_);
    std::snprintf(name_, sizeof name_, "%.*s", static_cast<int>(file.size()), file.data());
    std::copy(identity.begin(), identity.end(), identity_.begin());
    system_ = system;
    has_rtc_ = cartridge.has_clock();
    ram_size_ = ram_size;
    capacity_ = capacity;
    type_ = cartridge.type();
    shadow_ = shadow;
    file_ = buffer;
    file_size_ = kHeader + ram_size + kTrailer;
    if (ram_size) std::copy_n(memory.begin(), ram_size, shadow_);

    saved_rtc_ = false;
    saved_system_ = true;
    saved_size_ = 0;
    saved_type_ = 0;
    sequence_ = best < 0 ? 0 : best_sequence;
    next_slot_ = best < 0 ? 0 : 1 - best;
    if (best < 0) {
        found_ = any ? Found::damaged : Found::none;
    } else {
        const uint8_t* h = best_file;
        const uint32_t ram = get32(&h[16]);
        saved_system_ = h[76] == static_cast<uint8_t>(system);
        saved_type_ = h[77];
        // A GBA's save brings its type, so its size need not match what the
        // ROM's ID string or nothing yet decided.
        const bool same = std::equal(identity_.begin(), identity_.end(), h + 24) && saved_system_ &&
                          (system == System::gba ? ram <= capacity_ : ram == ram_size_);
        found_ = same ? Found::loaded : Found::mismatch;
        if (h[76] == static_cast<uint8_t>(System::gba)) gba_title(std::span<const uint8_t, 12>(h + 24, 12), out.title);
        else rom_title(std::span<const uint8_t, 16>(h + 24, 16), out.title);
        // A save of another size fills what both have.
        saved_size_ = std::min<size_t>(ram, system == System::gba ? capacity_ : ram_size_);
        std::copy_n(h + kHeader, saved_size_, shadow_);
        const uint32_t flags = get32(&h[20]);
        out.has_rtc = flags & kHasRtc;
        std::copy_n(h + 48, clock_.size(), clock_.begin());
        out.wall = {static_cast<bool>(flags & kWallTrusted), static_cast<int64_t>(get64(h + 68))};
        wall_ = out.wall;
        saved_rtc_ = out.has_rtc && saved_system_;
    }
    platform::free_external(best_file);
    out.found = found_;
    if (best < 0) {
        platform::log(platform::LogLevel::info, kTag, "%s: %s", paths_[0], any ? "damaged" : "no save");
    } else {
        platform::log(platform::LogLevel::info, kTag, "%s: sequence %lu%s", paths_[best],
                      static_cast<unsigned long>(sequence_), found_ == Found::loaded ? "" : ", from another ROM");
    }
    return true;
}

void Saves::use(SaveCartridge& cartridge, bool load_save) {
    std::lock_guard lock(mutex_);
    const bool found = found_ == Found::loaded || found_ == Found::mismatch;
    // A save of the other system, which shares the ROM's file name, is
    // never loaded; it is kept as .old before the first write over it.
    const bool load = load_save && found && saved_system_;
    if (load) {
        cartridge.load({shadow_, saved_size_}, saved_type_);
        if (has_rtc_ && saved_rtc_) cartridge.set_clock(clock_);
    }
    // The shadow holds what the RAM holds now, as the cartridge stores it.
    const auto memory = cartridge.memory();
    ram_size_ = memory.size();
    type_ = cartridge.type();
    file_size_ = kHeader + ram_size_ + kTrailer;
    if (ram_size_) std::copy_n(memory.begin(), ram_size_, shadow_);
    if (has_rtc_) cartridge.clock(clock_);
    backup_ = !load && (found_ == Found::mismatch || found_ == Found::damaged);
    generation_ = committed_ = writing_generation_ = 0;
    error_ = writing_ = commit_requested_ = retry_requested_ = false;
    active_ = true;
    session_ = true;
    pending_banks_ = 0;
    pending_rtc_ = false;
    cartridge.take_written();
    cartridge.take_clock_written();
}

void Saves::snapshot(SaveCartridge& cartridge) {
    if (!session_) return;
    pending_banks_ |= cartridge.take_written();
    pending_rtc_ |= cartridge.take_clock_written();
    if (!pending_banks_ && !pending_rtc_) return;
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;
    copy_changes(cartridge, pending_banks_, pending_rtc_);
    pending_banks_ = 0;
    pending_rtc_ = false;
}

void Saves::snapshot_all(SaveCartridge& cartridge, bool final) {
    if (!session_) return;
    cartridge.take_written();
    const bool rtc = cartridge.take_clock_written() || pending_rtc_ || (final && has_rtc_);
    std::lock_guard lock(mutex_);
    copy_changes(cartridge, ~0u, rtc);
    pending_banks_ = 0;
    pending_rtc_ = false;
    if (final && generation_ != committed_) {
        commit_requested_ = true;
        wake_.signal();
    }
}

// Holds the mutex.
void Saves::copy_changes(SaveCartridge& cartridge, uint32_t banks, bool rtc) {
    const auto ram = cartridge.memory();
    bool changed = rtc;
    if (ram.size() != ram_size_ && ram.size() <= capacity_) {
        // A GBA's save memory got its type from the program's first access:
        // the whole of it is new.
        ram_size_ = ram.size();
        type_ = cartridge.type();
        file_size_ = kHeader + ram_size_ + kTrailer;
        std::copy(ram.begin(), ram.end(), shadow_);
        changed = true;
        banks = 0;
    }
    for (uint32_t rest = banks; rest; rest &= rest - 1) {
        const size_t at = static_cast<size_t>(std::countr_zero(rest)) * 8192;
        if (at >= ram.size()) break;
        const size_t n = std::min<size_t>(8192, ram.size() - at);
        if (std::memcmp(shadow_ + at, ram.data() + at, n) != 0) {
            std::memcpy(shadow_ + at, ram.data() + at, n);
            changed = true;
        }
    }
    if (!changed) return;
    if (has_rtc_) cartridge.clock(clock_);
    wall_ = platform::wall_time();
    const uint64_t now = platform::monotonic_us();
    // The first change past what is on the card or being written starts the
    // longest wait.
    if (generation_ == (writing_ ? writing_generation_ : committed_)) dirty_since_us_ = now;
    ++generation_;
    changed_us_ = now;
    wake_.signal();
}

void Saves::close() {
    session_ = false;
    for (;;) {
        {
            std::lock_guard lock(mutex_);
            if (!writing_) {
                active_ = false;
                release();
                return;
            }
        }
        platform::sleep_ms(5);
    }
}

// Holds the mutex.
void Saves::release() {
    platform::free_external(shadow_);
    platform::free_external(file_);
    shadow_ = file_ = nullptr;
    ram_size_ = capacity_ = file_size_ = 0;
    error_ = false;
}

Saves::State Saves::state() const {
    std::lock_guard lock(mutex_);
    // A GBA's session waits for a save memory type before it has a save.
    if (!active_ || (!ram_size_ && !has_rtc_ && generation_ == committed_)) return State::none;
    if (error_) return State::error;
    if (writing_) return State::writing;
    return generation_ != committed_ ? State::dirty : State::clean;
}

int Saves::progress() const {
    std::lock_guard lock(mutex_);
    return file_size_ ? static_cast<int>(written_.load(std::memory_order_relaxed) * 100 / file_size_) : 0;
}

void Saves::commit() {
    {
        std::lock_guard lock(mutex_);
        commit_requested_ = true;
    }
    wake_.signal();
}

void Saves::retry() {
    {
        std::lock_guard lock(mutex_);
        if (!error_) return;
        // DIRTY until the writer takes it.
        error_ = false;
        retry_requested_ = true;
    }
    wake_.signal();
}

const char* Saves::error_text() const {
    std::lock_guard lock(mutex_);
    return reason(last_error_);
}

void Saves::write_settings(std::string_view text) {
    {
        std::lock_guard lock(mutex_);
        settings_size_ = std::min(text.size(), sizeof settings_);
        std::memcpy(settings_, text.data(), settings_size_);
        settings_due_ = true;
        settings_at_us_ = platform::monotonic_us() + kSettingsDelayUs;
    }
    wake_.signal();
}

void Saves::flush_settings() {
    {
        std::lock_guard lock(mutex_);
        if (!settings_due_) return;
        settings_at_us_ = platform::monotonic_us();
    }
    wake_.signal();
}

void Saves::run() {
    for (;;) {
        const uint32_t wait = wait_ms();
        if (wait == kForever) {
            wake_.wait();
        } else if (wait) {
            wake_.wait_for(wait);
        }
        write_save();
        write_settings_file();
        if (stopping_.load()) {
            // A due settings file is written above before the task ends.
            std::lock_guard lock(mutex_);
            if (!settings_due_) break;
        }
    }
}

// How long the task can sleep before a write is due.
uint32_t Saves::wait_ms() {
    std::lock_guard lock(mutex_);
    const uint64_t now = platform::monotonic_us();
    uint64_t due = UINT64_MAX;
    if (active_ && !writing_ && !error_ && generation_ != committed_) {
        due = commit_requested_ || retry_requested_ ? now
                                                    : std::min(changed_us_ + kQuietUs, dirty_since_us_ + kMaxDirtyUs);
    }
    if (settings_due_) due = std::min(due, stopping_.load() ? now : settings_at_us_);
    if (due == UINT64_MAX) return stopping_.load() ? 0 : kForever;
    return due <= now ? 0 : static_cast<uint32_t>(std::min<uint64_t>((due - now + 999) / 1000, 60'000));
}

void Saves::write_save() {
    char path[sizeof paths_[0]];
    char name[sizeof name_];
    bool backup, remount;
    int slot;
    uint32_t sequence;
    size_t size;
    {
        std::lock_guard lock(mutex_);
        if (!active_ || writing_ || error_) return;
        const uint64_t now = platform::monotonic_us();
        const bool due = generation_ != committed_ &&
                         (commit_requested_ || retry_requested_ || now >= changed_us_ + kQuietUs ||
                          now >= dirty_since_us_ + kMaxDirtyUs);
        if (!due) {
            retry_requested_ = false;
            return;
        }
        remount = retry_requested_ && last_error_ == Error::failed;
        retry_requested_ = commit_requested_ = false;
        writing_ = true;
        writing_generation_ = generation_;
        backup = backup_;
        slot = next_slot_;
        sequence = sequence_ + 1;
        std::memcpy(path, paths_[slot], sizeof path);
        std::memcpy(name, name_, sizeof name);
        size = file_size_;
        uint8_t* h = file_;
        std::memset(h, 0, kHeader);
        std::memcpy(h, kMagic, sizeof kMagic);
        put16(h + 8, kVersion);
        put16(h + 10, kHeader);
        put32(h + 12, sequence);
        put32(h + 16, static_cast<uint32_t>(ram_size_));
        put32(h + 20, (has_rtc_ ? kHasRtc : 0) | (wall_.trusted ? kWallTrusted : 0));
        std::copy(identity_.begin(), identity_.end(), h + 24);
        if (has_rtc_) std::copy(clock_.begin(), clock_.end(), h + 48);
        put64(h + 68, static_cast<uint64_t>(wall_.us));
        h[76] = static_cast<uint8_t>(system_);
        h[77] = type_;
        if (ram_size_) std::memcpy(h + kHeader, shadow_, ram_size_);
        written_.store(0, std::memory_order_relaxed);
    }
    // The buffer is this task's until writing_ clears.
    put32(file_ + size - kTrailer, crc32({file_, size - kTrailer}));
    const uint64_t start = platform::monotonic_us();
    Error error = Error::none;
    if (remount && !platform::files::remount()) error = Error::failed;
    // Once the backup is made, a retry after a failed write must not copy
    // the half-written file over it.
    const bool backed_up = error == Error::none && backup && (error = back_up()) == Error::none;
    if (error == Error::none) error = platform::files::make_parents(path);
    if (error == Error::none) error = platform::files::write(path, {file_, size}, &written_);
    {
        std::lock_guard lock(mutex_);
        writing_ = false;
        if (backed_up) backup_ = false;
        if (error == Error::none) {
            sequence_ = sequence;
            next_slot_ = 1 - slot;
            committed_ = writing_generation_;
        } else {
            error_ = true;
            last_error_ = error;
        }
    }
    if (error == Error::none) {
        platform::log(platform::LogLevel::info, kTag, "saved %s, sequence %lu, %u bytes, %llu ms", path,
                      static_cast<unsigned long>(sequence), static_cast<unsigned>(size),
                      static_cast<unsigned long long>((platform::monotonic_us() - start) / 1000));
    } else {
        faults_->add("Can't save %s: %s", name, reason(error));
    }
}

// Copies both files to .old and flushes them, before the first write over a
// save that was not loaded.
Error Saves::back_up() {
    for (int slot = 0; slot < 2; ++slot) {
        char from[sizeof paths_[0]], to[sizeof paths_[0] + 4];
        {
            std::lock_guard lock(mutex_);
            std::memcpy(from, paths_[slot], sizeof from);
        }
        std::snprintf(to, sizeof to, "%s.old", from);
        const int64_t size = platform::files::size(from);
        if (size == platform::files::kNoFile) continue;
        if (size < 0 || size > static_cast<int64_t>(kMaxFile)) return Error::failed;
        auto* data = static_cast<uint8_t*>(platform::allocate_external(std::max<size_t>(size, 1)));
        if (!data) return Error::failed;
        Error error = platform::files::read(from, {data, static_cast<size_t>(size)}) ? Error::none : Error::failed;
        if (error == Error::none) error = platform::files::write(to, {data, static_cast<size_t>(size)});
        platform::free_external(data);
        if (error != Error::none) return error;
        platform::log(platform::LogLevel::info, kTag, "kept %s", to);
    }
    return Error::none;
}

void Saves::write_settings_file() {
    char text[sizeof settings_];
    size_t size;
    {
        std::lock_guard lock(mutex_);
        if (!settings_due_ || (platform::monotonic_us() < settings_at_us_ && !stopping_.load())) return;
        settings_due_ = false;
        size = settings_size_;
        std::memcpy(text, settings_, size);
    }
    char path[64];
    settings_path(path, sizeof path);
    Error error = platform::files::make_parents(path);
    if (error == Error::none) {
        error = platform::files::write(path, {reinterpret_cast<const uint8_t*>(text), size});
    }
    if (error != Error::none) faults_->add("Can't write the settings: %s", reason(error));
}

}  // namespace gbx::app
