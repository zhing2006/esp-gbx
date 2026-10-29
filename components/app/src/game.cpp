#include "game.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "gbx/audio.h"
#include "gbx/display.h"
#include "gbx/executable.h"
#include "gbx/files.h"
#include "gbx/platform.h"
#include "color_fix.h"
#include "rom_info.h"
#include "sha256.h"

namespace gbx::app {
namespace {

constexpr const char* kTag = "game";
// The emulation runs a frame whenever fewer PCM frames than this wait for
// the output: 40 ms, about two and a half GB frames. A frame runs in a few
// milliseconds on either backend, so the queue stays near the target and
// only a stall longer than the queue and the output's buffers is heard.
constexpr size_t kTarget = Game::kAudioRate * 40 / 1000;
// The GBA's, 64 ms: its frames take most of a frame's time in heavy scenes,
// and the longest, such as those where the JIT first compiles a scene's
// code, reach 50-60 ms (task 6-3).
constexpr size_t kGbaTarget = Game::kAudioRate * 64 / 1000;
// A GBA game starts with the level at 140 ms, and the output waits for
// 100 ms of it before it plays: the JIT compiles most of a game's code as
// it first runs it, and the slowest frames after a load took 60-80 ms
// (task 6-4). The rest of the level is frames the mixer has not finished,
// up to about 21 ms of them open to changes. Once the output plays, the
// level comes down to kGbaTarget by a millisecond a frame, so that the
// emulation runs about 6 % slower for a second and a quarter rather than
// stopping at once. The mixer holds 8192 frames, 170 ms; a frame adds 800
// to the level before the next is held back.
constexpr size_t kGbaStartTarget = Game::kAudioRate * 140 / 1000;
constexpr size_t kGbaPreroll = Game::kAudioRate * 100 / 1000;
constexpr size_t kCushionStep = Game::kAudioRate / 1000;
// The audio task hands over one output buffer at a time, see esp/audio.cpp.
constexpr size_t kChunk = 256;
// While the queue stays more than a GB frame's worth (800 frames) below the
// target, frames come slower than they play: the emulation cannot keep up.
// The trim then makes each emulated second produce up to 0.5 % more frames,
// a pitch change of under 9 cents, so a small shortfall is not heard as
// gaps. A larger one still shows as speed below 100 % and underruns.
constexpr int64_t kDeadBand = 800;
constexpr int32_t kMaxTrim = 5000;
constexpr uint64_t kFrameUs = 16743;  // 140448 ticks
// How long a game that is ending waits for its save to be written. The card
// may be gone, and then the write fails long before this.
constexpr uint32_t kSaveWaitMs = 10'000;

#if GBX_HAS_GBA
// The GBA's ROM cache, unless set_gba_options() gives another: a ROM up to
// this size stays whole in external memory, a larger one is paged. The
// board sets it for its PSRAM, see components/core/Kconfig.
constexpr size_t kGbaRomCache = GBX_GBA_ROM_CACHE_BYTES;
constexpr const char* kGbaBios = "bios/gba_bios.bin";
// The SHA-256 of the GBA BIOS: No-Intro "[BIOS] Nintendo Game Boy Advance
// Boot ROM (World)", as tools/prepare_gba_tests.py BIOS_SHA256 records it.
constexpr std::array<uint8_t, 32> kGbaBiosSha256 = {
    0xfd, 0x25, 0x47, 0x72, 0x4b, 0x50, 0x5f, 0x48, 0x7e, 0x6d, 0xcb, 0x29, 0xec, 0x2e, 0xcf, 0xf3,
    0xaf, 0x35, 0xa8, 0x41, 0xa7, 0x7a, 0xb2, 0xe8, 0x5f, 0xd8, 0x73, 0x50, 0xab, 0xd3, 0x65, 0x70};
// The save memory a GBA game may come to have: 1 Mbit Flash at most
// (GBATEK "GBA Cart Backup IDs").
constexpr size_t kGbaSaveCapacity = 128 * 1024;

// platform::keys' bits in KEYINPUT's order: the GB's directions and
// buttons trade places, and L and R follow (GBATEK "GBA Keypad Input").
uint16_t gba_keys(uint16_t keys) {
    return static_cast<uint16_t>((keys >> 4 & 0xf) | (keys & 0xf) << 4 |
                                 (keys & platform::keys::kR ? gba::key::r : 0) |
                                 (keys & platform::keys::kL ? gba::key::l : 0));
}
#endif

// Emulated ticks in `us` microseconds, without overflow for days.
Tick ticks(uint64_t us) {
    return us / 1'000'000 * kTicksPerSecond + us % 1'000'000 * kTicksPerSecond / 1'000'000;
}
#if GBX_HAS_GBA
// The same in GBA cycles.
Tick gba_cycles(uint64_t us) {
    return us / 1'000'000 * gba::kCyclesPerSecond + us % 1'000'000 * gba::kCyclesPerSecond / 1'000'000;
}
#endif

// The file name in a path, for messages.
const char* file_name(const char* path) {
    const char* name = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') name = p + 1;
    }
    return name;
}

// Gives a JIT that has none a second level from the PSRAM left: up to
// `most`, less `leave`, which stays free for what the menus and the saves
// allocate while the game runs. See GBX_GB_JIT_STORE_LEAVE_KB and
// GBX_GBA_JIT_STORE_LEAVE_KB.
template<class Jit>
[[maybe_unused]] void fill_store(Jit* jit, size_t most, size_t leave) {
    if (!jit || jit->store_capacity()) return;
    const size_t free = platform::heap_stats().external_free;
    if (free <= leave) return;
    const size_t room = free - leave;
    // Its index takes a word for each of twice as many records, rounded up
    // to a power of two, see jit::CodeStore.
    const auto taken = [](size_t bytes) { return bytes + std::bit_ceil(bytes / 256 * 2) * 4; };
    constexpr size_t kStep = 16 * 1024;
    size_t bytes = std::min(most, room);
    for (bytes -= bytes % kStep; bytes >= kStep && taken(bytes) > room;) bytes -= kStep;
    // The free PSRAM may be in pieces smaller than that.
    for (; bytes >= kStep; bytes -= kStep)
        if (jit->initialize_store(bytes, bytes / 256)) break;
    platform::log(platform::LogLevel::info, kTag, "JIT second level %u KiB, %u KiB of PSRAM left",
                  static_cast<unsigned>(jit->store_capacity() / 1024),
                  static_cast<unsigned>(platform::heap_stats().external_free / 1024));
}

#ifdef GBX_PROFILER
// What the system and its backend left of internal RAM, for sizing the
// JIT's pools and tables.
void log_internal_ram() {
    platform::log(platform::LogLevel::warn, kTag, "internal RAM: %u bytes free, largest executable block %u",
                  static_cast<unsigned>(platform::heap_stats().internal_free),
                  static_cast<unsigned>(platform::largest_executable()));
}
#endif

}  // namespace

// Counts a task in while it reads the running system, so that release()
// frees none it still reads. The count and the pointers are sequentially
// consistent: a reader that counts itself before the emulation task clears
// a pointer is waited for, and one that counts itself after it finds the
// pointer cleared.
class Game::Reader {
public:
    explicit Reader(const Game& game) : game_(game) {
        game_.readers_.fetch_add(1);
        gb = game_.shared_.load();
#if GBX_HAS_GBA
        gba = game_.gba_shared_.load();
#endif
    }
    ~Reader() { game_.readers_.fetch_sub(1); }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    gb::System* gb;
#if GBX_HAS_GBA
    gba::System* gba;
#endif
private:
    const Game& game_;
};

bool Game::initialize(std::span<const int16_t, 4> window, Saves& saves, Faults& faults, Sounds& sounds) {
    std::copy(window.begin(), window.end(), window_.begin());
    saves_ = &saves;
    faults_ = &faults;
    sounds_ = &sounds;
    reserve_jit();
    // Built now, before the tasks that use it start; without memory there is no
    // COLOR FIX.
    color_fix_table();
    audio_ = platform::audio::open(kAudioRate);
    if (!audio_) {
        platform::log(platform::LogLevel::warn, kTag, "no sound output, pacing by the clock");
    } else {
        // Paused until a game runs: the menus' cues alone are not a stream
        // whose gaps are underruns.
        platform::audio::pause();
    }
    // On one core the emulation takes the time the other tasks leave, see
    // game.h.
    const unsigned priority = GBX_SINGLE_CORE ? kSingleCorePriority : kEmulationPriority;
    return wake_.initialize(1) && slots_.start(faults, wake_) &&
           emulation_.start(emulate_entry, this, 1, "gbx-emulate", 8192, priority) &&
           audio_task_.start(play_entry, this, 0, "gbx-audio", 4096, 7);
}

void Game::stop() {
    stopping_.store(true);
    wake_.signal();
    emulation_.join();
    audio_task_.join();
    // The emulation task has ended, so this task owns the system now.
    if (loaded_) end_game(true);
    slots_.stop();
    platform::display::hide_game();
    release();
    platform::free_executable(jit_reserved_);
    if (audio_) platform::audio::close();
    // A later initialize() starts over without a game.
    stopping_.store(false);
    requested_.store(false);
    state_.store(GameState::no_rom);
}

int Game::load_progress() const {
    const size_t size = load_size_.load(std::memory_order_relaxed);
    return size ? static_cast<int>(load_done_.load(std::memory_order_relaxed) * 100 / size) : 0;
}

Game::Question Game::question() const {
    std::lock_guard lock(question_mutex_);
    return question_;
}

void Game::answer(Answer answer) {
    answer_.store(static_cast<int>(answer), std::memory_order_release);
    wake_.signal();
}

void Game::pause() {
    pause_requested_.store(true);
    wake_.signal();
}

void Game::resume() {
    resume_requested_.store(true);
    wake_.signal();
}

void Game::set_backend(Backend backend) {
    backend_requested_.store(static_cast<int>(backend));
    wake_.signal();
}

void Game::set_fast(bool fast) {
    fast_requested_.store(fast);
    wake_.signal();
}

void Game::set_palette(const std::array<uint16_t, 4>& palette) {
    uint64_t packed = 0;
    for (int i = 0; i < 4; ++i) packed |= static_cast<uint64_t>(palette[i]) << (16 * i);
    palette_.store(packed, std::memory_order_relaxed);
    platform::display::set_game_shades(palette);
    const Reader reader(*this);
    if (reader.gb) reader.gb->set_palette(palette);
}

std::array<uint16_t, 4> Game::palette() const {
    const uint64_t packed = palette_.load(std::memory_order_relaxed);
    return {static_cast<uint16_t>(packed), static_cast<uint16_t>(packed >> 16), static_cast<uint16_t>(packed >> 32),
            static_cast<uint16_t>(packed >> 48)};
}

int Game::paused_pixels(std::span<uint16_t> out, bool fixed) const {
#if GBX_HAS_GBA
    if (gba()) {
        std::copy_n(gba_paused_, std::min<size_t>(out.size(), gba::kFramePixels), out.begin());
        return gba::kScreenWidth;
    }
#endif
    if (mode() != gb::Mode::dmg) {
        const size_t n = std::min(out.size(), paused_colors_.size());
        if (fixed && color_fix()) {
            for (size_t i = 0; i < n; ++i) out[i] = app::color_fix(paused_colors_[i]);
        } else {
            std::copy_n(paused_colors_.begin(), n, out.begin());
        }
        return 160;
    }
    const uint64_t packed = palette_.load(std::memory_order_relaxed);
    const size_t n = std::min(out.size(), paused_shades_.size());
    for (size_t i = 0; i < n; ++i) out[i] = static_cast<uint16_t>(packed >> (16 * (paused_shades_[i] & 3)));
    return 160;
}

void Game::set_color_fix(bool on) {
    color_fix_.store(on, std::memory_order_relaxed);
    show_colors();
}

// The display's colors for the game loaded: COLOR FIX's table in color, for
// a GB's colors only.
void Game::show_colors() const {
    const bool fix = color_fix() && mode() != gb::Mode::dmg && !gba();
    platform::display::set_color_table(fix ? color_fix_table() : nullptr);
}

// The frame the game stops on, for the pause menu and for resuming.
void Game::keep_frame() {
#if GBX_HAS_GBA
    if (gba()) {
        const auto pixels = gba_system_->frame().pixels;
        std::copy_n(pixels.begin(), std::min<size_t>(pixels.size(), gba::kFramePixels), gba_paused_);
        return;
    }
#endif
    if (system_->memory.color()) {
        const auto pixels = system_->frame().pixels;
        std::copy_n(pixels.begin(), std::min(pixels.size(), paused_colors_.size()), paused_colors_.begin());
        return;
    }
    const auto shades = system_->shades();
    std::copy_n(shades.begin(), std::min(shades.size(), paused_shades_.size()), paused_shades_.begin());
}

void Game::save_slot(int slot) {
    slot_save_.store(slot);
    wake_.signal();
}

void Game::load_slot(int slot, bool other_rom) {
    {
        std::lock_guard lock(slot_mutex_);
        slot_result_ = {SlotLoad::loading};
    }
    slot_other_rom_.store(other_rom);
    slot_load_.store(slot);
    wake_.signal();
}

Game::SlotResult Game::slot_result() const {
    std::lock_guard lock(slot_mutex_);
    return slot_result_;
}

void Game::finish() {
    finish_.fetch_add(1);
    wake_.signal();
}

void Game::unload() {
    unload_requested_.store(true);
    wake_.signal();
}

void Game::path(char* out, size_t size) const {
    std::lock_guard lock(request_mutex_);
    std::snprintf(out, size, "%s", path_);
}

void Game::start(const char* path, Backend backend, bool cgb) {
    {
        std::lock_guard lock(request_mutex_);
        // A path that does not fit is not cut, which could name another file
        // or end inside a character; load() refuses it. The shown name is
        // cut between characters.
        size_t n = std::strlen(path);
        const bool fits = n < sizeof path_;
        if (!fits) {
            n = sizeof path_ - 1;
            while (n && (static_cast<uint8_t>(path[n]) & 0xc0) == 0x80) --n;
        }
        std::memcpy(path_, path, n);
        path_[n] = '\0';
        std::snprintf(request_path_, sizeof request_path_, "%s", fits ? path : "");
        request_backend_ = backend;
        request_cgb_ = cgb;
    }
    // LOADING at once, so the caller never sees the state from before.
    state_.store(GameState::loading, std::memory_order_release);
    start_count_.fetch_add(1, std::memory_order_release);
    requested_.store(true, std::memory_order_release);
    wake_.signal();
}

void Game::set_buttons(uint16_t buttons) {
    uint32_t old = buttons_.load();
    uint32_t next;
    do {
        next = buttons | (old & 0xffff0000u) | (static_cast<uint32_t>(buttons & ~old & 0xffff) << 16);
    } while (!buttons_.compare_exchange_weak(old, next));
}

void Game::set_gba_options(const char* bios, size_t rom_cache_bytes) {
    std::lock_guard lock(request_mutex_);
    std::snprintf(gba_bios_path_, sizeof gba_bios_path_, "%s", bios ? bios : "");
    gba_rom_cache_ = rom_cache_bytes;
}

// Clears the shared pointers and waits for the readers that took them.
void Game::unshare() {
    shared_.store(nullptr);
#if GBX_HAS_GBA
    gba_shared_.store(nullptr);
#endif
    while (readers_.load()) platform::sleep_ms(1);
}

// Frees all the last game held, so that the next one finds the memory as
// the first did: a block left behind would split PSRAM, where the ROM cache
// needs one piece. The JIT's block stays, see jit_reserved_.
void Game::release() {
    unshare();
    gba_.store(false, std::memory_order_release);
    if (system_) {
        system_->set_frame_sink(nullptr);
        gb_cart_.attach(nullptr);
        system_.reset();
    }
    platform::free_external(rom_);
    rom_ = nullptr;
#if GBX_HAS_GBA
    if (gba_system_) {
        gba_system_->set_frame_sink(nullptr);
        gba_cart_.attach(nullptr);
        // The system borrows the cache and the BIOS.
        gba_system_.reset();
    }
    gba_rom_.reset();
    platform::free_external(gba_bios_);
    platform::free_external(gba_paused_);
    gba_bios_ = nullptr;
    gba_paused_ = nullptr;
#endif
}

void Game::reserve_jit() {
    if (jit_reserved_.words) return;
    // The JITs of the systems the build runs.
#if GBX_HAS_GB && GBX_HAS_GBA
    const size_t bytes = std::max(gb::Sm83Jit::reserved_bytes({}), gba::Arm7Jit::reserved_bytes({}));
#elif GBX_HAS_GBA
    const size_t bytes = gba::Arm7Jit::reserved_bytes({});
#else
    const size_t bytes = gb::Sm83Jit::reserved_bytes({});
#endif
    jit_reserved_ = platform::allocate_executable(bytes);
}

void Game::fill_jit_store() {
#if GBX_HAS_GBA
    if (gba()) {
#if GBX_GBA_JIT_STORE_LEAVE_BYTES
        fill_store(gba_system_->jit(), GBX_GBA_JIT_STORE_BYTES, GBX_GBA_JIT_STORE_LEAVE_BYTES);
#endif
        return;
    }
#endif
#if GBX_GB_JIT_STORE_LEAVE_BYTES
    fill_store(system_->jit(), GBX_GB_JIT_STORE_BYTES, GBX_GB_JIT_STORE_LEAVE_BYTES);
#endif
}

void Game::Mark::store(uint64_t count, uint64_t us) {
    const uint32_t sequence = sequence_.load(std::memory_order_relaxed);
    sequence_.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    count_.store(count, std::memory_order_relaxed);
    us_.store(us, std::memory_order_relaxed);
    sequence_.store(sequence + 2, std::memory_order_release);
}

void Game::Mark::load(uint64_t& count, uint64_t& us) const {
    for (;;) {
        const uint32_t sequence = sequence_.load(std::memory_order_acquire);
        count = count_.load(std::memory_order_relaxed);
        us = us_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (!(sequence & 1) && sequence == sequence_.load(std::memory_order_relaxed)) return;
    }
}

void Game::read(Counters& out) {
    out.session = session_.load(std::memory_order_acquire);
    out.backend = backend_.load();
    ran_.load(out.emulated, out.emulated_us);
    delivered_.load(out.frames, out.last_frame_us);
    out.audio = audio_;
    out.fast = fast_.load(std::memory_order_relaxed);
    out.queued = level();
    out.lowest = std::min(lowest_.exchange(SIZE_MAX, std::memory_order_relaxed), out.queued);
    out.underruns = platform::audio::underruns() - underruns_at_load_.load(std::memory_order_relaxed);
    out.work_count = work_count_.load(std::memory_order_acquire);
    out.work_base = work_base_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < out.work.size(); ++i) {
        out.work[i] = work_[i].load(std::memory_order_relaxed);
    }
    out.gap_count = gap_count_.load(std::memory_order_acquire);
    out.gap_base = gap_base_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < out.gap.size(); ++i) {
        out.gap[i] = gap_[i].load(std::memory_order_relaxed);
    }
}

size_t Game::level() const {
    const Reader reader(*this);
#if GBX_HAS_GBA
    const size_t queued = reader.gb ? reader.gb->audio_queued() : reader.gba ? reader.gba->audio_queued() : 0;
    return reader.gb || reader.gba ? queued + held_.load(std::memory_order_relaxed) : 0;
#else
    return reader.gb ? reader.gb->audio_queued() + held_.load(std::memory_order_relaxed) : 0;
#endif
}

platform::display::PixelFormat Game::gb_format() const {
    using platform::display::PixelFormat;
    return mode() == gb::Mode::dmg && platform::display::game_shades_supported() ? PixelFormat::shades
                                                                                 : PixelFormat::bgr555;
}

// A frame drawn is one the display takes, see step(): one that comes early
// waits for it.
uint16_t* Game::frame_buffer() { return platform::display::game_frame_buffer(); }

#ifdef GBX_PROFILER
#if GBX_HAS_GBA
// The start of bench()'s first frame snapshots the counters, and the end
// of its last one logs them.
void Game::bench_start() {
    auto& b = bench_;
    b.parts = gba::arm7_cycle_counts;
    b.began_us = platform::monotonic_us();
    b.drawn = gba::render_counters.frames.load(std::memory_order_relaxed);
    b.draw_us = gba::render_counters.draw_us.read();
    b.draw_cycles = gba::render_counters.draw_cycles.read();
    b.draw_retired = gba::render_counters.draw_retired.read();
    for (unsigned i = 0; i < gba::kRenderParts; ++i) {
        b.part_cycles[i] = gba::render_counters.part_cycles[i].read();
        b.part_retired[i] = gba::render_counters.part_retired[i].read();
    }
    b.waits = gb::offload_counters.waits[0];
    b.wait_us = gb::offload_counters.wait_us[0];
    b.finds = gba::arm7_find_counts;
    b.accesses = gba::arm7_access_counts;
    b.stubs = gba::arm7_stub_counts;
    b.payload_waits = gba::render_counters.payload_waits.load(std::memory_order_relaxed);
    b.payload_wait_us = gba::render_counters.payload_wait_us.load(std::memory_order_relaxed);
    b.ppu_frame = gba_system_->memory.ppu.frames_begun();
    gba::render_counters.worst_draw_us.store(0, std::memory_order_relaxed);
    gba::render_counters.worst_span_us.store(0, std::memory_order_relaxed);
    if (gba_rom_) {
        b.rom_misses = gba_rom_->stats().misses;
        b.rom_miss_us = gba_rom_->stats().miss_us;
    }
    if (const auto* jit = gba_system_->jit()) {
        b.compiled = jit->compiled_blocks();
        b.restored = jit->restored_blocks();
        b.evictions = jit->evictions();
    }
}

void Game::bench_frame(uint64_t frame, uint32_t cycles, uint32_t retired) {
    auto& b = bench_;
    if (frame < b.first || frame > b.last) return;
    b.cycles += cycles;
    b.retired += retired;
    if (frame != b.last) return;
    const auto parts = gba::arm7_cycle_counts;
    const auto part = [&](unsigned i) { return static_cast<double>(parts[i] - b.parts[i]); };
    const double frames = static_cast<double>(b.last - b.first + 1), run = part(0);
    const auto share = [&](unsigned i) { return run ? 100.0 * part(i) / run : 0.0; };
    const uint32_t drawn = gba::render_counters.frames.load(std::memory_order_relaxed) - b.drawn;
    const uint32_t draw_us = gba::render_counters.draw_us.read() - b.draw_us;
    platform::log(platform::LogLevel::warn, "bench",
                  "%.0f frames in %.0f ms: cpu %.2f Mcycles a frame, run %.2f; renderer %.2f ms a frame, "
                  "cpu waited %lu times %.1f ms",
                  frames, (platform::monotonic_us() - b.began_us) / 1000.0, b.cycles / frames / 1e6,
                  run / frames / 1e6, drawn ? draw_us / 1000.0 / drawn : 0.0,
                  static_cast<unsigned long>(gb::offload_counters.waits[0] - b.waits),
                  (gb::offload_counters.wait_us[0] - b.wait_us) / 1000.0);
    platform::log(platform::LogLevel::warn, "bench",
                  "native %.1f%% (writes %.1f reads %.1f boundaries %.1f catch-ups %.1f idle %.1f refills %.1f) "
                  "halt %.1f%% interrupts %.1f%% lookups %.1f%%; ppu events %.1f%% (begin %.1f submit %.1f "
                  "schedule %.1f irq %.1f)",
                  share(1), share(2), share(3), share(4), share(9), share(10), share(8), share(5), share(6), share(7),
                  share(11), share(12), share(13), share(14), share(15));
    // Native code alone: its time without the C++ it called.
    const double native = part(1) - part(2) - part(3) - part(4) - part(8) - part(9) - part(10),
                 instructions = part(27);
    platform::log(platform::LogLevel::warn, "bench",
                  "a frame: %.0f instructions; host %.2f M instructions, %.2f a cycle; native code alone %.1f "
                  "cycles an instruction",
                  instructions / frames, b.retired / frames / 1e6, b.cycles ? double(b.retired) / b.cycles : 0.0,
                  instructions ? native / instructions : 0.0);
    platform::log(platform::LogLevel::warn, "bench",
                  "a frame: %.0f writes %.0f reads %.0f catch-ups (%.0f watched) %.0f boundaries (%.0f watched); "
                  "watches from %.0f IME and %.0f IE & IF changes",
                  part(16) / frames, part(17) / frames, part(18) / frames, part(23) / frames, part(19) / frames,
                  part(24) / frames, part(25) / frames, part(26) / frames);
    if (const auto* jit = gba_system_->jit()) {
        char text[160];
        int used = 0;
        for (size_t i = 0; i < 16; ++i)
            used += std::snprintf(text + used, sizeof text - used, " %.1f", (gba::arm7_find_counts[i] - b.finds[i]) / frames);
        platform::log(platform::LogLevel::warn, "bench", "a frame: compiled %.2f restored %.2f evicted %.2f; finds%s",
                      (jit->compiled_blocks() - b.compiled) / frames, (jit->restored_blocks() - b.restored) / frames,
                      (jit->evictions() - b.evictions) / frames, text);
        platform::log(platform::LogLevel::warn, "bench", "compiling %.1f%% installing %.1f%% storing %.1f%%", share(20),
                      share(21), share(22));
    }
    if (drawn) {
        const auto counted = [&](const gba::ProfileTotal& counter, uint32_t before) {
            return static_cast<double>(static_cast<uint32_t>(counter.read() - before)) * (1u << gba::kCountShift) /
                   drawn / 1e6;
        };
        const double draw_cycles = counted(gba::render_counters.draw_cycles, b.draw_cycles),
                     draw_retired = counted(gba::render_counters.draw_retired, b.draw_retired);
        platform::log(platform::LogLevel::warn, "bench", "renderer: a frame %.2f M cycles, %.2f M instructions, %.2f a cycle",
                      draw_cycles, draw_retired, draw_cycles ? draw_retired / draw_cycles : 0.0);
        // Millions of cycles and instructions a frame by part; the BG and
        // OBJ layers and the conversion are inside the painting.
        static constexpr const char* kParts[gba::kRenderParts]{"pages",     "objects",    "setup",
                                                               "painting",  "layers",     "OBJ layer",
                                                               "convert",   "moving out"};
        char text[320];
        int used = 0;
        for (unsigned i = 0; i < gba::kRenderParts; ++i)
            used += std::snprintf(text + used, sizeof text - used, "%s%s %.2f %.2f", i ? ", " : "", kParts[i],
                                  counted(gba::render_counters.part_cycles[i], b.part_cycles[i]),
                                  counted(gba::render_counters.part_retired[i], b.part_retired[i]));
        platform::log(platform::LogLevel::warn, "bench", "renderer parts (M cycles, M instructions): %s", text);
        used = 0;
        for (unsigned i = 0; i < gba::kRenderParts; ++i)
            used += std::snprintf(text + used, sizeof text - used, "%s%s %.2f", i ? ", " : "", kParts[i],
                                  gba::render_counters.worst_part_retired[i].load(std::memory_order_relaxed) / 1e6);
        platform::log(platform::LogLevel::warn, "bench", "renderer parts in the longest frame (M instructions): %s", text);
    }
    platform::log(platform::LogLevel::warn, "bench",
                  "renderer: longest frame %.1f ms drawing (the bench's frame %lld, %.2f M cycles, %.2f M instructions), "
                  "%.1f ms from its first line's start; the payload ring held the cpu %lu times %.1f ms",
                  gba::render_counters.worst_draw_us.load(std::memory_order_relaxed) / 1000.0,
                  static_cast<long long>(gba::render_counters.worst_draw_frame.load(std::memory_order_relaxed) - b.ppu_frame),
                  gba::render_counters.worst_draw_cycles.load(std::memory_order_relaxed) / 1e6,
                  gba::render_counters.worst_draw_retired.load(std::memory_order_relaxed) / 1e6,
                  gba::render_counters.worst_span_us.load(std::memory_order_relaxed) / 1000.0,
                  static_cast<unsigned long>(gba::render_counters.payload_waits.load(std::memory_order_relaxed) -
                                             b.payload_waits),
                  (gba::render_counters.payload_wait_us.load(std::memory_order_relaxed) - b.payload_wait_us) / 1000.0);
    if (gba_rom_)
        platform::log(platform::LogLevel::warn, "bench", "rom: %lu pages read on a miss in %.1f ms",
                      static_cast<unsigned long>(gba_rom_->stats().misses - b.rom_misses),
                      (gba_rom_->stats().miss_us - b.rom_miss_us) / 1000.0);
    // Calls of the access stubs a frame, by region.
    {
        char text[300];
        int used = 0;
        for (unsigned kind = 0; kind < 2; ++kind) {
            used += std::snprintf(text + used, sizeof text - used, "%s", kind ? "; stub writes:" : "stub reads:");
            for (unsigned i = 0; i < 16; ++i)
                if (const uint32_t n = gba::arm7_stub_counts[16 * kind + i] - b.stubs[16 * kind + i])
                    used += std::snprintf(text + used, sizeof text - used, " %x %.0f", i, n / frames);
        }
        platform::log(platform::LogLevel::warn, "bench", "%s", text);
    }
    // Writes and reads through C++ a frame: by region, then the I/O
    // halfwords with the most.
    for (unsigned kind = 0; kind < 2; ++kind) {
        const auto& now = gba::arm7_access_counts[kind];
        const auto& then = b.accesses[kind];
        char text[400];
        int used = std::snprintf(text, sizeof text, "%s:", kind ? "reads" : "writes");
        for (unsigned i = 0; i < 16; ++i)
            if (now[i] != then[i])
                used += std::snprintf(text + used, sizeof text - used, " %x %.0f", i, (now[i] - then[i]) / frames);
        used += std::snprintf(text + used, sizeof text - used, "; io");
        std::array<uint16_t, 0x108> order{};
        for (unsigned i = 0; i < order.size(); ++i) order[i] = static_cast<uint16_t>(i);
        const auto count = [&](unsigned i) { return now[16 + i] - then[16 + i]; };
        std::partial_sort(order.begin(), order.begin() + 12, order.end(),
                          [&](unsigned x, unsigned y) { return count(x) > count(y); });
        for (unsigned i = 0; i < 12 && count(order[i]); ++i)
            used += std::snprintf(text + used, sizeof text - used, " %03x %.0f", order[i] * 2, count(order[i]) / frames);
        platform::log(platform::LogLevel::warn, "bench", "%s", text);
    }
    b.last = 0;
}
#endif

Game::Profile Game::take_profile() {
    return {worst_run_us_.exchange(0), worst_compiles_.load(), worst_restores_.load(), compiled_.load(),
            restored_.load(), evictions_.load(), invalidations_.load(), code_bytes_.load(), store_bytes_.load(),
            store_records_.load(), compiled_bytes_.load(), interpreted_.load(), stored_entries_.load(),
            store_drops_.load(), static_cast<uint32_t>(cpu_cycles_.load() / 1000000),
            static_cast<uint32_t>(cpu_retired_.load() / 1000000), static_cast<uint32_t>(cpu_branches_.load() / 1000),
            static_cast<uint32_t>(cpu_mispredicts_.load() / 1000), worst_misses_.load(), worst_miss_us_.load(),
            worst_wait_us_.load()};
}

Game::SlowAccess Game::take_slow_access() {
    auto& seen = slow_seen_;
    const auto take = [](const std::atomic<uint32_t>& count, uint32_t& before) {
        const uint32_t current = count.load(std::memory_order_relaxed), n = current - before;
        before = current;
        return n;
    };
    SlowAccess out{};
    for (unsigned i = 0; i < 6; ++i) {
        out.reads[i] = take(slow_reads_[i], seen.reads[i]);
        out.writes[i] = take(slow_writes_[i], seen.writes[i]);
    }
    out.horizon_resets = take(horizon_resets_, seen.horizon_resets);
    std::array<uint32_t, 128> io;
    for (unsigned i = 0; i < 128; ++i) io[i] = take(slow_io_[i], seen.io[i]);
    for (unsigned k = 0; k < out.io.size(); ++k) {
        const auto top = std::max_element(io.begin(), io.end());
        out.io[k] = static_cast<uint8_t>(top - io.begin());
        out.io_writes[k] = *top;
        *top = 0;
    }
    return out;
}

Game::RunStart Game::profile_start() const {
    const auto* jit = system_->jit();
    return {platform::monotonic_us(), jit ? jit->compiled_blocks() : 0, jit ? jit->restored_blocks() : 0};
}

uint32_t Game::profile_end(const RunStart& start) {
    const auto run_us = static_cast<uint32_t>(platform::monotonic_us() - start.us);
    const auto* jit = system_->jit();
    if (run_us > worst_run_us_.load(std::memory_order_relaxed)) {
        worst_compiles_.store(jit ? jit->compiled_blocks() - start.compiled : 0, std::memory_order_relaxed);
        worst_restores_.store(jit ? jit->restored_blocks() - start.restored : 0, std::memory_order_relaxed);
        worst_run_us_.store(run_us, std::memory_order_relaxed);
    }
    if (!jit) return run_us;
    compiled_.store(jit->compiled_blocks(), std::memory_order_relaxed);
    restored_.store(jit->restored_blocks(), std::memory_order_relaxed);
    evictions_.store(jit->evictions(), std::memory_order_relaxed);
    invalidations_.store(jit->invalidations(), std::memory_order_relaxed);
    code_bytes_.store(static_cast<uint32_t>(jit->code_bytes()), std::memory_order_relaxed);
    store_bytes_.store(static_cast<uint32_t>(jit->store_bytes()), std::memory_order_relaxed);
    store_records_.store(static_cast<uint32_t>(jit->store_records()), std::memory_order_relaxed);
    compiled_bytes_.store(jit->compiled_bytes(), std::memory_order_relaxed);
    interpreted_.store(gb::sm83_jit_counters.interpreted, std::memory_order_relaxed);
    stored_entries_.store(jit->stored_entries(), std::memory_order_relaxed);
    store_drops_.store(jit->store_drops(), std::memory_order_relaxed);
    const auto& counters = gb::sm83_jit_counters;
    for (unsigned i = 0; i < 6; ++i) {
        slow_reads_[i].store(counters.read_area[i], std::memory_order_relaxed);
        slow_writes_[i].store(counters.write_area[i], std::memory_order_relaxed);
    }
    for (unsigned i = 0; i < 128; ++i) slow_io_[i].store(counters.io_write[i], std::memory_order_relaxed);
    horizon_resets_.store(counters.horizon_reset, std::memory_order_relaxed);
    return run_us;
}
#endif

bool Game::capture(uint16_t* out, uint32_t timeout_ms, int& width) {
    width = gba() ? gba::kScreenWidth : 160;
    captured_.store(false);
    capture_.store(out, std::memory_order_release);
    for (uint32_t waited = 0; waited < timeout_ms; waited += 5) {
        if (captured_.load(std::memory_order_acquire)) return true;
        platform::sleep_ms(5);
    }
    // The worker may have taken the buffer and still be copying.
    if (capture_.exchange(nullptr) == out) return false;
    while (!captured_.load(std::memory_order_acquire)) platform::sleep_ms(1);
    return true;
}

void Game::frame_ready(uint64_t id) {
    // See capture().
    if (auto* out = capture_.exchange(nullptr, std::memory_order_acquire)) {
        const size_t pixels = gba() ? gba::kFramePixels : gb::kFramePixels;
        if (const auto* frame = platform::display::game_frame_buffer()) {
            if (shades()) {
                // GB mode's shades in the current palette.
                const auto palette = this->palette();
                const auto* shades = reinterpret_cast<const uint8_t*>(frame);
                for (size_t i = 0; i < pixels; ++i) out[i] = palette[shades[i] & 3];
            } else if (gba()) {
                // The GBA's frames are in RGB565: back to BGR555.
                for (size_t i = 0; i < pixels; ++i) {
                    const unsigned c = frame[i];
                    out[i] = static_cast<uint16_t>((c >> 11) | ((c >> 6) & 31) << 5 | (c & 31) << 10);
                }
            } else {
                std::memcpy(out, frame, pixels * sizeof(uint16_t));
            }
        }
        captured_.store(true, std::memory_order_release);
    }
    platform::display::publish_game_frame(id);
    // A blank screen repeats the previous ID and is not a new frame. IDs
    // count the frames emulated, so the frames 2X did not draw count too.
    if (id <= last_id_) return;
    frames_ += id - last_id_;
    last_id_ = id;
    const uint64_t now = platform::monotonic_us();
    delivered_.store(frames_, now);
    gap_done(id, now);
}

// Frame work, on the emulation task: from the start of emulating frame `id`
// until the PPU ends it, which covers waiting for the workers but not the
// waits that keep the speed. The first frame after a speed change starts
// the samples of the new speed.
void Game::work_done([[maybe_unused]] uint64_t id, uint64_t began, uint64_t now) {
    const auto n = work_count_.load(std::memory_order_relaxed);
    if (speed_changes_ != work_speed_) {
        work_speed_ = speed_changes_;
        work_base_.store(n, std::memory_order_relaxed);
    }
    work_[n % work_.size()].store(static_cast<uint32_t>(now - began), std::memory_order_relaxed);
    if (drawing_) drawn_us_ = now - began;
    work_count_.store(n + 1, std::memory_order_release);
#ifdef GBX_PROFILER
    replay_.work(id, static_cast<uint32_t>(now - began));
#endif
}

// The interval since the frame before was handed to the display, when
// both were begun at the same speed.
void Game::gap_done(uint64_t id, uint64_t now) {
    const auto& start = starts_[id % starts_.size()];
    const bool known = start.id.load(std::memory_order_acquire) == id;
    const auto speed = start.speed.load(std::memory_order_relaxed);
    const auto n = gap_count_.load(std::memory_order_relaxed);
    if (known && gap_us_ && speed == gap_speed_) {
        gap_[n % gap_.size()].store(static_cast<uint32_t>(now - gap_us_), std::memory_order_relaxed);
        gap_count_.store(n + 1, std::memory_order_release);
    } else {
        gap_speed_ = speed;
        gap_base_.store(n, std::memory_order_relaxed);
    }
    gap_us_ = known ? now : 0;
}

// A load that failed: the reason goes to the user and the faults.
void Game::fail(const char* reason) {
    std::snprintf(error_, sizeof error_, "%s", reason);
    if (audio_) platform::audio::pause();
    char path[sizeof path_];
    {
        std::lock_guard lock(request_mutex_);
        std::memcpy(path, path_, sizeof path);
    }
    // The reason is a sentence of its own in the dialog; after the colon a
    // word such as "The" goes lowercase, "GBA" stays.
    char first = reason[0];
    if (first >= 'A' && first <= 'Z' && reason[1] >= 'a' && reason[1] <= 'z') first += 'a' - 'A';
    faults_->add("Can't load %s: %c%s", file_name(path), first, first ? reason + 1 : "");
    state_.store(GameState::error, std::memory_order_release);
}

// A running game that cannot go on. It keeps its frame for the pause menu,
// and QUIT still writes its save.
void Game::stop_running(const char* reason) {
    std::snprintf(error_, sizeof error_, "%s", reason);
    if (audio_) platform::audio::pause();
    keep_frame();
    char path[sizeof path_];
    {
        std::lock_guard lock(request_mutex_);
        std::memcpy(path, path_, sizeof path);
    }
    faults_->add("%s stopped: %s", file_name(path), reason);
    state_.store(GameState::error, std::memory_order_release);
}

void Game::emulate() {
    while (!stopping_.load()) {
        if (requested_.exchange(false, std::memory_order_acquire)) {
            char path[sizeof request_path_];
            Backend backend;
            bool cgb;
            {
                std::lock_guard lock(request_mutex_);
                std::memcpy(path, request_path_, sizeof path);
                backend = request_backend_;
                cgb = request_cgb_;
            }
            if (loaded_) end_game(true);
            load(path, backend, cgb);
            continue;
        }
        if (unload_requested_.exchange(false)) {
            if (loaded_) end_game(false);
            release();
            state_.store(GameState::no_rom, std::memory_order_release);
            continue;
        }
        const GameState state = state_.load(std::memory_order_relaxed);
        if (state == GameState::run) {
            if (pause_requested_.exchange(false)) {
                enter_pause();
            } else {
                if (backend_requested_.load(std::memory_order_relaxed) >= 0) change_backend();
                step();
            }
            continue;
        }
        if (state == GameState::paused && slot_reading_) {
            // A load waits for Slots to read and decode the file; a resume
            // waits for the load.
            if (slots_.work() == Slots::Work::none) {
                restore_slot();
                continue;
            }
        } else if (state == GameState::paused) {
            // A save or load asked for before RESUME takes the paused state.
            if (const int slot = loaded_ ? slot_save_.exchange(0) : 0) {
                save_slot_now(slot);
                continue;
            }
            if (const int slot = loaded_ ? slot_load_.exchange(0) : 0) {
                slot_other_rom_reading_ = slot_other_rom_.load();
                if (slots_.read(slot)) {
                    slot_reading_ = slot;
                } else {
                    std::lock_guard lock(slot_mutex_);
                    slot_result_ = {SlotLoad::failed, Clock::untrusted, Slots::Kind::unreadable};
                }
                continue;
            }
            if (resume_requested_.exchange(false)) {
                // The menus ask for a save or load before the RESUME after
                // it, so one asked for since the checks above shows here;
                // it goes first and RESUME waits for the next turn.
                if (loaded_ && (slot_save_.load() || slot_load_.load())) {
                    resume_requested_.store(true);
                    continue;
                }
                leave_pause();
                continue;
            }
        }
        if (loaded_ && (state == GameState::paused || state == GameState::error)) {
            if (backend_requested_.load(std::memory_order_relaxed) >= 0) {
                change_backend();
                continue;
            }
            if (const uint32_t finish = finish_.load(); finished_.load() != finish) {
                if (paused_) count_pause();
                saves_->snapshot_all(cartridge(), true);
                finished_.store(finish, std::memory_order_release);
                continue;
            }
        }
        wake_.wait();
    }
}

// Ends the loaded game. `keep_changes` takes its last changes to the save
// and waits for them to be written, for a game ended without QUIT.
void Game::end_game(bool keep_changes) {
    if (audio_) platform::audio::pause();
    if (keep_changes && battery_.load()) {
        if (paused_) count_pause();
        saves_->snapshot_all(cartridge(), true);
        for (uint32_t waited = 0; waited < kSaveWaitMs; waited += 10) {
            const auto s = saves_->state();
            if (s != Saves::State::dirty && s != Saves::State::writing) break;
            platform::sleep_ms(10);
        }
    }
    saves_->close();
    slots_.close();
    slot_reading_ = 0;
    slot_restored_ = false;
#if GBX_HAS_GBA
    if (gba()) gba_system_->set_frame_sink(nullptr);
    else system_->set_frame_sink(nullptr);
#else
    system_->set_frame_sink(nullptr);
#endif
    platform::display::hide_game();
    loaded_ = false;
    paused_ = false;
    battery_.store(false);
}

void Game::enter_pause() {
    if (audio_) platform::audio::pause();
    saves_->snapshot_all(cartridge(), false);
    keep_frame();
    paused_ = true;
    paused_us_ = platform::monotonic_us();
    state_.store(GameState::paused, std::memory_order_release);
}

// The cartridge clock counts the pause so far, as a real one would: before
// it runs again, and before a save is taken during the pause, which would
// otherwise hold the clock from when the pause began.
void Game::count_pause() {
    const uint64_t now = platform::monotonic_us();
    if (has_rtc()) advance_rtc(now - paused_us_);
    paused_us_ = now;
}

#if GBX_HAS_GBA
bool Game::has_rtc() const { return gba() ? gba_system_->has_rtc() : system_->has_rtc(); }

// The cartridge clock moves on by `us` microseconds that passed outside
// emulation.
void Game::advance_rtc(uint64_t us) {
    if (gba()) gba_system_->advance_rtc(gba_cycles(us));
    else system_->advance_rtc(ticks(us));
}

void Game::set_audio_speed(int32_t ppm) {
    if (gba()) gba_system_->set_audio_speed(ppm);
    else system_->set_audio_speed(ppm);
}

size_t Game::audio_buffered() const {
    return gba() ? gba_system_->audio_buffered() : system_->audio_buffered();
}
#else
bool Game::has_rtc() const { return system_->has_rtc(); }

// The cartridge clock moves on by `us` microseconds that passed outside
// emulation.
void Game::advance_rtc(uint64_t us) { system_->advance_rtc(ticks(us)); }

void Game::set_audio_speed(int32_t ppm) { system_->set_audio_speed(ppm); }

size_t Game::audio_buffered() const { return system_->audio_buffered(); }
#endif

void Game::leave_pause() {
    const bool on_gba = gba();
    const int width = on_gba ? gba::kScreenWidth : 160, height = on_gba ? gba::kScreenHeight : 144;
    // Keep the paused machine and its clock bookkeeping intact on failure.
    const auto format = on_gba ? platform::display::PixelFormat::rgb565 : gb_format();
    shades_.store(format == platform::display::PixelFormat::shades, std::memory_order_relaxed);
    if (!platform::display::show_game(window_, width, height, format)) {
        faults_->add("Can't resume: game display unavailable.");
        return;
    }
    count_pause();
    paused_ = false;
    // The paused frame goes up with the layer, so the window is never black.
    // The PPU's worker is idle until the next frame runs. Set again, the sink
    // has the GB's renderer give back the buffer it may still draw into.
    if (!on_gba) system_->set_frame_sink(this);
    if (uint16_t* frame = platform::display::game_frame_buffer()) {
        if (shades()) std::copy(paused_shades_.begin(), paused_shades_.end(), reinterpret_cast<uint8_t*>(frame));
        else paused_pixels({frame, on_gba ? gba::kFramePixels : gb::kFramePixels});
        // The GBA's layer takes the renderer's RGB565, the kept frame is
        // BGR555; green's new low bit repeats its top bit, as the renderer's.
        if (on_gba) {
            for (size_t i = 0; i < gba::kFramePixels; ++i) {
                const uint32_t c = frame[i], g = (c >> 5) & 0x1f;
                frame[i] = static_cast<uint16_t>((c & 0x1f) << 11 | ((g << 1) | (g >> 4)) << 5 | ((c >> 10) & 0x1f));
            }
        }
#if GBX_HAS_GBA
        platform::display::publish_game_frame(on_gba ? gba_system_->memory.ppu.frame_id()
                                                     : system_->memory.ppu.frame_id());
#else
        platform::display::publish_game_frame(system_->memory.ppu.frame_id());
#endif
    }
    pace_restart();
    // An instant save runs code the JIT may not have compiled yet.
    if (slot_restored_) start_cushion();
    slot_restored_ = false;
    // A new span for the telemetry, which the pause would distort.
    session_.fetch_add(1, std::memory_order_release);
    if (audio_ && !fast_.load(std::memory_order_relaxed)) platform::audio::resume();
    state_.store(GameState::run, std::memory_order_release);
}

// The cartridge clock in the state counts the pause so far, as a save taken
// during it does.
void Game::save_slot_now(int slot) {
    count_pause();
    // A refusal adds its own fault.
#if GBX_HAS_GBA
    if (gba()) slots_.save(slot, *gba_system_);
    else slots_.save(slot, *system_);
#else
    slots_.save(slot, *system_);
#endif
}

// Restores the state Slots has decoded. The game stays paused, from the
// state's frame; the cartridge clock moves on by the time since the state
// was taken, as for a cartridge save after power off, and the pause counts
// from now. The cartridge save takes the loaded RAM and clock as changes.
void Game::restore_slot() {
    slot_reading_ = 0;
    SlotResult result{SlotLoad::loaded};
    const auto kind = slots_.read_kind();
    [[maybe_unused]] const bool on_gba = gba();
    if (kind != Slots::Kind::ok) {
        result = {SlotLoad::failed, Clock::untrusted, kind};
    } else if (
#if GBX_HAS_GBA
        on_gba ? !gba_system_->restore(slots_.gba_snapshot(), slot_other_rom_reading_) :
#endif
               !system_->restore(slots_.snapshot(), slot_other_rom_reading_)) {
        result = {SlotLoad::failed, Clock::untrusted, Slots::Kind::incompatible};
    }
    if (result.load == SlotLoad::loaded) {
        if (has_rtc()) {
            const auto saved = slots_.read_wall();
            const auto now = platform::wall_time();
            if (saved.trusted && now.trusted && now.us >= saved.us) {
                advance_rtc(static_cast<uint64_t>(now.us - saved.us));
            } else {
                result.load = SlotLoad::clock;
                result.clock = !now.trusted ? Clock::untrusted : !saved.trusted ? Clock::untrusted_save : Clock::behind;
            }
        }
#if GBX_HAS_GBA
        if (on_gba) {
            const auto frame = slots_.gba_snapshot().frame();
            std::copy_n(frame.begin(), std::min<size_t>(frame.size(), gba::kFramePixels), gba_paused_);
        } else
#endif
        if (system_->memory.color()) {
            const auto frame = slots_.snapshot().color_frame();
            std::copy_n(frame.begin(), std::min(frame.size(), paused_colors_.size()), paused_colors_.begin());
        } else {
            const auto frame = slots_.snapshot().frame();
            std::copy_n(frame.begin(), std::min(frame.size(), paused_shades_.size()), paused_shades_.begin());
        }
        paused_us_ = platform::monotonic_us();
        slot_restored_ = true;
        saves_->snapshot_all(cartridge(), false);
        // The frame IDs went back with the state. restore() left the PPU's
        // worker idle, and the next row handed to it carries these. The
        // next frame delivered follows the state's frame count.
#if GBX_HAS_GBA
        last_id_ = on_gba ? gba_system_->memory.frames : system_->memory.frames;
#else
        last_id_ = system_->memory.frames;
#endif
        frame_began_us_ = 0;
#ifdef GBX_PROFILER
        if (bench_absolute_.load(std::memory_order_relaxed)) {
        } else if (const uint32_t frames = bench_frames_.exchange(0, std::memory_order_acquire); frames && on_gba) {
            bench_ = {};
            bench_.first = last_id_ + 1 + bench_skip_.load(std::memory_order_relaxed);
            bench_.last = bench_.first + frames - 1;
        }
#endif
        session_.fetch_add(1, std::memory_order_release);
    }
    std::lock_guard lock(slot_mutex_);
    slot_result_ = result;
}

void Game::change_backend() {
    const int requested = backend_requested_.exchange(-1);
    if (requested < 0 || !loaded_) return;
    const auto backend = static_cast<Backend>(requested);
    if (backend == backend_.load()) return;
#if GBX_HAS_GBA
    if (gba()) {
        if (!gba_system_->select_backend(backend == Backend::jit ? gba::Backend::jit : gba::Backend::interpreter,
                                         &jit_reserved_)) {
            faults_->add("Can't start the JIT: not enough memory for its code.");
            return;
        }
        fill_jit_store();
        backend_.store(backend);
        lend_core();
        platform::log(platform::LogLevel::info, kTag, "backend %s", backend == Backend::jit ? "jit" : "int");
        return;
    }
#endif
    if (backend == Backend::jit && !system_->jit()) {
        platform::log(platform::LogLevel::info, kTag, "largest executable block %u bytes",
                      static_cast<unsigned>(platform::largest_executable()));
    }
    if (!system_->select_backend(backend == Backend::jit ? gb::Backend::jit : gb::Backend::interpreter,
                                 &jit_reserved_)) {
        faults_->add("Can't start the JIT: not enough memory for its code.");
        return;
    }
    fill_jit_store();
    backend_.store(backend);
    lend_core();
    platform::log(platform::LogLevel::info, kTag, "backend %s", backend == Backend::jit ? "jit" : "int");
}

// The speed switch, at a frame boundary. 2X makes no sound: the mixer only
// takes the register writes, and the output pauses, where no underrun
// counts. Back at 1X the output fills its buffers again before it counts
// them, and the audio level paces once more. 1X and 2X are counted apart,
// so the telemetry starts a new span.
void Game::change_speed(bool fast) {
    fast_.store(fast, std::memory_order_relaxed);
    ++speed_changes_;
#if GBX_HAS_GBA
    if (gba()) {
        gba_system_->set_audio_rate(fast ? 0 : kAudioRate);
    } else
#endif
    {
        system_->set_audio_rate(fast ? 0 : kAudioRate);
        system_->set_worker_polling(fast);
    }
    lend_core();
    // A STOP that holds on pauses the output again at the next step.
    if (audio_ && fast) {
        platform::audio::pause();
    } else if (audio_) {
        platform::audio::resume();
    }
    pace_restart();
    session_.fetch_add(1, std::memory_order_release);
}

// See the class comment.
void Game::lend_core() {
    platform::display::lend_emulator_core(backend_.load() == Backend::jit && !fast_.load(std::memory_order_relaxed) &&
                                          !gba());
}

void Game::pace_restart() {
    trim_ = 0;
    set_audio_speed(0);
    smoothed_ = static_cast<int64_t>(gba() ? kGbaTarget : kTarget) << 8;
    deadline_us_ = platform::monotonic_us();
    suspended_ = false;
    show_us_ = began_us_ = 0;
    interval_us_ = kFrameUs / 2;
    cushion_ = 0;
    preroll_.store(0, std::memory_order_relaxed);
}

// A GBA game at 1X starting, after pace_restart(): the level goes up to
// kGbaStartTarget, and the output plays from kGbaPreroll.
void Game::start_cushion() {
    if (!gba() || !audio_ || fast_.load(std::memory_order_relaxed)) return;
    cushion_ = kGbaStartTarget - kGbaTarget;
    smoothed_ = static_cast<int64_t>(kGbaStartTarget) << 8;
    preroll_.store(kGbaPreroll, std::memory_order_relaxed);
}

// 2X runs up to about 120 frames a second and the screen shows about 60,
// so a frame is drawn only when it is the one nearest the time the next
// frame should show, 1X's frame time after the last one drawn. The next
// frame starts about interval_us_ later, so this one is nearer when it
// starts less than half an interval before that time. At full speed this
// draws every other frame, with 4 ms to spare against the pacing's 1 ms
// jitter. Below it, the frames drawn advance by 1 and 2 frames by turns,
// as evenly as the speed allows, at up to about 60 a second; drawing every
// other frame would show fewer, some of them twice. The worker neither
// renders nor converts the frames not drawn, and the CPU core, whose work
// doubles, hands over no rows for them.
bool Game::draw_now(uint64_t now) {
    if (began_us_) {
        // Slower than 1X every frame is drawn anyway; the cap keeps a stall
        // from widening the margin for the frames after it.
        const uint64_t interval = std::min(now - began_us_, kFrameUs);
        interval_us_ = (interval_us_ * 7 + interval) / 8;
    }
    began_us_ = now;
    if (now + interval_us_ / 2 < show_us_) return false;
    // The times keep their own beat, so the frames drawn do not drift. A
    // stall that leaves them behind does not make up for what it missed.
    show_us_ += kFrameUs;
    if (show_us_ < now) show_us_ = now + kFrameUs;
    return true;
}

// Waits for the user's answer on the emulation task, which has nothing else
// to do while loading. A new start or stop() ends the wait as a cancel.
Game::Answer Game::ask(const Question& question) {
    answer_.store(-1);
    {
        std::lock_guard lock(question_mutex_);
        question_ = question;
    }
    Answer answer = Answer::cancel;
    while (!stopping_.load() && !requested_.load()) {
        if (const int a = answer_.exchange(-1, std::memory_order_acquire); a >= 0) {
            answer = static_cast<Answer>(a);
            break;
        }
        wake_.wait();
    }
    std::lock_guard lock(question_mutex_);
    question_ = {};
    return answer;
}

// Opens the save of a game with a battery and asks what the load needs to
// ask. False when the game does not start.
bool Game::load_save(const char* path, std::span<const uint8_t, 24> identity, Saves::System system, size_t capacity,
                     const char* title) {
    Saves::Opened opened;
    if (!saves_->open(path, identity, system, capacity, cartridge(), opened)) {
        fail("Not enough memory for the save.");
        return false;
    }
    if (opened.found == Saves::Found::unreadable) {
        fail("The save cannot be read.");
        return false;
    }
    bool use = opened.found == Saves::Found::loaded;
    if (opened.found == Saves::Found::mismatch || opened.found == Saves::Found::damaged) {
        Question q;
        q.prompt = opened.found == Saves::Found::mismatch ? Prompt::mismatch : Prompt::damaged;
        std::snprintf(q.save_title, sizeof q.save_title, "%s", opened.title);
        std::snprintf(q.rom_title, sizeof q.rom_title, "%s", title);
        const Answer answer = ask(q);
        if (answer == Answer::cancel) {
            saves_->close();
            state_.store(GameState::no_rom, std::memory_order_release);
            return false;
        }
        use = opened.found == Saves::Found::mismatch && answer == Answer::yes;
    }
    saves_->use(cartridge(), use);
    if (!use || !opened.has_rtc || !has_rtc()) return true;
    // The clock moves on by the time the board was off, when the board's
    // clock can tell it.
    const auto now = platform::wall_time();
    if (opened.wall.trusted && now.trusted && now.us >= opened.wall.us) {
        const auto off = static_cast<uint64_t>(now.us - opened.wall.us);
        advance_rtc(off);
        platform::log(platform::LogLevel::info, kTag, "clock moved on %llu s",
                      static_cast<unsigned long long>(off / 1'000'000));
        return true;
    }
    Question q;
    q.prompt = Prompt::clock;
    q.clock = !now.trusted ? Clock::untrusted : !opened.wall.trusted ? Clock::untrusted_save : Clock::behind;
    if (ask(q) == Answer::cancel) {
        saves_->close();
        state_.store(GameState::no_rom, std::memory_order_release);
        return false;
    }
    return true;
}

void Game::load(const char* path, Backend backend, bool cgb) {
    state_.store(GameState::loading, std::memory_order_release);
    pause_requested_.store(false);
    resume_requested_.store(false);
    backend_requested_.store(-1);
    load_done_.store(0);
    load_size_.store(0);
    if (audio_) platform::audio::pause();
    // Nothing from the previous game reaches the screen or the counters.
    platform::display::hide_game();
    release();
    // The sink is detached, so the PPU's worker no longer counts frames.
    emulated_ = frames_ = 0;
    ran_.store(0, platform::monotonic_us());
    delivered_.store(0, 0);
    work_base_.store(0);
    work_count_.store(0);
    gap_base_.store(0);
    gap_count_.store(0);
    gap_us_ = 0;
    last_id_ = frame_began_us_ = 0;
    underruns_at_load_.store(platform::audio::underruns());
    session_.fetch_add(1, std::memory_order_release);
    platform::log(platform::LogLevel::info, kTag, "loading %s", path);
    if (!*path) {
        fail("The path is too long.");
        return;
    }

    // The header first: a file that cannot run is not read whole.
    std::array<uint8_t, kRomHeaderBytes> header{};
    const int64_t size = platform::files::read_start(path, header);
    if (size < 0) {
        fail("The file cannot be opened.");
        return;
    }
    const RomInfo info = parse_rom(std::span(header).first(std::min<size_t>(size, header.size())), size);
    if (info.problem != RomInfo::Problem::none) {
        char reason[96];
        describe_problem(info, reason, sizeof reason);
        fail(reason);
        return;
    }
    // parse_rom() leaves only the machines the build runs without a problem.
#if GBX_HAS_GBA
    if (info.machine == RomInfo::Machine::gba) {
        load_gba(path, backend, size, info.title);
        return;
    }
#endif
    auto* rom = static_cast<uint8_t*>(platform::allocate_external(static_cast<size_t>(size)));
    if (!rom) {
        fail("Not enough memory for the ROM.");
        return;
    }
    load_size_.store(static_cast<size_t>(size));
    if (!platform::files::read(path, {rom, static_cast<size_t>(size)}, &load_done_)) {
        platform::free_external(rom);
        fail("The file cannot be read.");
        return;
    }
    reserve_jit();
    system_.reset(new (std::nothrow) gb::System);
    if (!system_) {
        platform::free_external(rom);
        fail("Not enough memory for the emulator.");
        return;
    }
#if GBX_GB_JIT_STORE_LEAVE_BYTES
    {
        // The second level waits for the PSRAM the game leaves, see
        // fill_jit_store().
        gb::Sm83Jit::Capacity capacity;
        capacity.store_bytes = capacity.store_records = 0;
        system_->set_jit_capacity(capacity);
    }
#endif
    gb_cart_.attach(system_.get());
    const std::span<const uint8_t> bytes{rom, static_cast<size_t>(size)};
    const bool loaded = system_->load_rom(bytes, false, gb::run_mode(bytes, cgb));
    rom_ = rom;
    mode_.store(system_->memory.mode(), std::memory_order_release);
    show_colors();
    if (!loaded) {
        // The header passed Cartridge::supports(), so only the RAM is left.
        fail("Not enough memory for the cartridge RAM.");
        return;
    }
#ifdef GBX_PROFILER
    if (!replay_.take({rom, static_cast<size_t>(size)}, path)) {
        fail("The recording cannot be played.");
        return;
    }
#endif
    // The first load creates the APU's mixer, which the other tasks read.
    shared_.store(system_.get(), std::memory_order_release);
    // The APU runs on the machine's clock, which exists once a ROM is loaded.
    const bool fast = fast_requested_.load();
    fast_.store(fast, std::memory_order_relaxed);
    system_->set_audio_rate(fast ? 0 : kAudioRate);
    system_->set_worker_polling(fast);
    const uint64_t packed = palette_.load(std::memory_order_relaxed);
    if (packed) {
        system_->set_palette({static_cast<uint16_t>(packed), static_cast<uint16_t>(packed >> 16),
                              static_cast<uint16_t>(packed >> 32), static_cast<uint16_t>(packed >> 48)});
    }
    if (backend == Backend::jit && !system_->jit()) {
        // The JIT takes its code pool from this block.
        platform::log(platform::LogLevel::info, kTag, "largest executable block %u bytes",
                      static_cast<unsigned>(platform::largest_executable()));
    }
    if (!system_->select_backend(backend == Backend::jit ? gb::Backend::jit : gb::Backend::interpreter,
                                 &jit_reserved_)) {
        fail("The JIT cannot start: not enough memory for its code.");
        return;
    }
#ifdef GBX_PROFILER
    log_internal_ram();
#endif
    backend_.store(backend);
    lend_core();
    battery_.store(system_->has_battery());
#ifdef GBX_PROFILER
    // A replay starts from its recording's cartridge RAM and clock, and
    // writes no save.
    if (replay_.playing()) battery_.store(false);
#endif
    if (battery_.load() && !load_save(path, gb::rom_identity(bytes), Saves::System::gb,
                                      system_->cartridge_ram().size(), info.title)) {
        battery_.store(false);
        return;
    }
    // Without memory for them, the game runs without instant saves.
    if (!slots_.open(path, *system_)) faults_->add("No memory for the instant saves of %s.", file_name(path));
    fill_jit_store();
#ifdef GBX_PROFILER
    replay_.started(*system_);
#endif
    const auto format = gb_format();
    shades_.store(format == platform::display::PixelFormat::shades, std::memory_order_relaxed);
    if (!platform::display::show_game(window_, 160, 144, format)) {
        saves_->close();
        battery_.store(false);
        fail("Game display unavailable.");
        return;
    }
    loaded_ = true;
    pace_restart();
    system_->set_frame_sink(this);
    if (audio_ && !fast) platform::audio::resume();
    state_.store(GameState::run, std::memory_order_release);
}

void Game::step() {
    // Behind the output for long, as in a heavy scene, this task would not
    // block, and its core's idle task and watchdog (5 s) would starve: it
    // rests a tick after a second without one, a millisecond a second at
    // most, as the render worker does.
    if (const uint64_t now = platform::monotonic_us(); now - rested_us_ >= 1'000'000) {
        platform::sleep_ms(1);
        rested_us_ = platform::monotonic_us();
    }
#if GBX_HAS_GBA
    if (gba()) {
        step_gba();
        return;
    }
#endif
    const bool fast = fast_requested_.load(std::memory_order_relaxed);
    if (fast != fast_.load(std::memory_order_relaxed)) change_speed(fast);
    const unsigned speed = fast ? 2 : 1;
    // 1X follows the output, which plays on a clock of its own; 2X has no
    // sound and follows the wall clock.
    if (audio_ && !fast) {
        // Ready PCM alone omits work already handed to the mixer and can
        // make consecutive frames fill the same apparent audio deficit.
        const size_t queued = system_->audio_buffered() + held_.load(std::memory_order_relaxed);
        if (queued >= kTarget) {
            platform::sleep_ms(1);
            return;
        }
        pace_trim(queued, kTarget);
    } else {
        const uint64_t now = platform::monotonic_us();
        if (now < deadline_us_) {
            platform::sleep_ms(static_cast<uint32_t>(std::max<uint64_t>((deadline_us_ - now) / 1000, 1)));
            return;
        }
        // A slow frame does not build up a debt of frames to run back to back.
        deadline_us_ = std::max(deadline_us_, now - kFrameUs / speed);
    }
    const uint32_t buttons = buttons_.fetch_and(0xffff);
    auto keys = static_cast<uint8_t>(buttons | (buttons >> 16));
#ifdef GBX_PROFILER
    keys = replay_.keys(keys, *system_);
#endif
    system_->set_buttons(keys);
    auto& memory = system_->memory;
    const uint64_t next = memory.frames + 1;
    const uint64_t began = platform::monotonic_us();
    // A frame is drawn whole or not at all: one a run stopped inside goes on
    // as it began, or its rows from before would show stale ones.
    if (!system_->in_frame()) {
        // Not drawn either: a frame the display would not take as it ends,
        // about as long from now as the latest frame drawn took.
        drawing_ = (!fast || draw_now(began)) && platform::display::game_frame_due(drawn_us_);
        system_->set_drawing(drawing_);
        frame_began_us_ = began;
    }
    auto& start = starts_[next % starts_.size()];
    start.us.store(began, std::memory_order_relaxed);
    start.speed.store(speed_changes_, std::memory_order_relaxed);
    start.id.store(next, std::memory_order_release);
    const Tick before = memory.clock.now;
#ifdef GBX_PROFILER
    const auto run_start = profile_start();
#endif
    const auto result = system_->run_frame();
    if (result.end == gb::RunEnd::frame && frame_began_us_) work_done(next, frame_began_us_, platform::monotonic_us());
#ifdef GBX_PROFILER
    const auto run_us = profile_end(run_start);
#endif
    const Tick elapsed = result.time - before;
#ifdef GBX_PROFILER
    replay_.ran(*system_, elapsed, result.instructions, run_us);
#endif
    emulated_ += elapsed;
    ran_.store(emulated_, platform::monotonic_us());
    // What the game changed in its save this frame.
    saves_->snapshot(gb_cart_);
    if (!audio_ || fast) deadline_us_ += (elapsed ? elapsed * 1'000'000 / kTicksPerSecond : kFrameUs) / speed;
    if (result.end == gb::RunEnd::unavailable) {
        char reason[64];
        std::snprintf(reason, sizeof reason, "execution stopped at PC %04X.", system_->cpu.pc);
        stop_running(reason);
        return;
    }
    if (!elapsed) {
        // STOP with no key selected, or a locked CPU: time stands still and
        // the game makes no sound, which is not an underrun.
        if (!suspended_ && audio_) platform::audio::pause();
        suspended_ = true;
        platform::sleep_ms(16);
    } else if (suspended_) {
        suspended_ = false;
        if (audio_ && !fast) platform::audio::resume();
    }
}

#if GBX_HAS_GBA
// A GBA game. The BIOS comes from the card, or where set_gba_options()
// says, and is checked before anything else of the load, so that a missing
// or wrong one costs no ROM read and no save prompt. The ROM goes through
// the ROM cache, which reads it once whole or, past the cache's budget, in
// pages as they are used. The save memory and clock go through Saves as a
// GB's do; the save file also records the save memory's type, which a game
// without an ID string decides by its first access.
void Game::load_gba(const char* path, Backend backend, int64_t size, const char* title) {
    char bios_path[sizeof gba_bios_path_];
    size_t budget;
    {
        std::lock_guard lock(request_mutex_);
        std::snprintf(bios_path, sizeof bios_path, "%s", *gba_bios_path_ ? gba_bios_path_ : kGbaBios);
        budget = gba_rom_cache_ ? gba_rom_cache_ : kGbaRomCache;
    }
    reserve_jit();
    // The system holds IWRAM, so it takes internal RAM. The BIOS goes to
    // PSRAM: the JIT reads it when it compiles BIOS code, and at run time
    // only for the BIOS's data and its latch, a few words per interrupt or
    // SWI.
    gba_system_.reset(new (std::nothrow) gba::System);
    gba_bios_ = static_cast<uint8_t*>(platform::allocate_external(gba::kBiosBytes));
    gba_paused_ = static_cast<uint16_t*>(platform::allocate_external(gba::kFramePixels * sizeof(uint16_t)));
    gba_rom_.reset(new (std::nothrow) gba::RomCache);
    if (!gba_bios_ || !gba_paused_ || !gba_system_ || !gba_rom_ || !gba_system_->initialize()) {
        fail("Not enough memory for the emulator.");
        return;
    }
    gba_cart_.attach(gba_system_.get());
    const int64_t bios_size = platform::files::read_start(bios_path, {gba_bios_, gba::kBiosBytes});
    if (bios_size < 0) {
        platform::log(platform::LogLevel::error, kTag, "no GBA BIOS at %s", bios_path);
        fail("No GBA BIOS on the card: GBA games need bios/gba_bios.bin, dumped from a GBA.");
        return;
    }
    if (bios_size != static_cast<int64_t>(gba::kBiosBytes) ||
        sha256({gba_bios_, gba::kBiosBytes}) != kGbaBiosSha256) {
        platform::log(platform::LogLevel::error, kTag, "%s: %lld bytes, not the GBA BIOS", bios_path,
                      static_cast<long long>(bios_size));
        fail("The GBA BIOS on the card is not the original: its size or checksum does not match.");
        return;
    }
    load_size_.store(static_cast<size_t>(size));
    const auto observe = [](void* self, std::span<const uint8_t> bytes) {
        static_cast<Game*>(self)->load_done_.fetch_add(bytes.size(), std::memory_order_relaxed);
    };
    if (!gba_rom_->open(path, budget, observe, this)) {
        fail("The file cannot be read.");
        return;
    }
    if (!gba_system_->load(std::span<const uint8_t, gba::kBiosBytes>(gba_bios_, gba::kBiosBytes), *gba_rom_)) {
        fail("Not enough memory for the cartridge.");
        return;
    }
    gba_.store(true, std::memory_order_release);
    mode_.store(gb::Mode::dmg, std::memory_order_release);
    show_colors();
    const bool fast = fast_requested_.load();
    fast_.store(fast, std::memory_order_relaxed);
    gba_system_->set_audio_rate(fast ? 0 : kAudioRate);
    // The GBA's worker sleeps when it runs out of work, at both speeds: it
    // runs below the tasks of the UI, which its polls would hold up.
    gba_system_->set_worker_polling(false);
    gba_shared_.store(gba_system_.get());
#if GBX_GBA_JIT_STORE_LEAVE_BYTES
    {
        // The second level waits for the PSRAM the game leaves, see
        // fill_jit_store().
        gba::Arm7JitCapacity capacity;
        capacity.store_bytes = capacity.store_records = 0;
        gba_system_->set_jit_capacity(capacity);
    }
#endif
    if (!gba_system_->select_backend(backend == Backend::jit ? gba::Backend::jit : gba::Backend::interpreter,
                                     &jit_reserved_)) {
        fail("The JIT cannot start: not enough memory for its code.");
        return;
    }
#ifdef GBX_PROFILER
    log_internal_ram();
#endif
    backend_.store(backend);
    lend_core();
    // Any GBA game may have a save memory, whose type its first access
    // decides when no ID string names it: the session is open from the
    // start, and the file is written once there is something to save.
    battery_.store(true);
    if (!load_save(path, gba::rom_identity(*gba_rom_), Saves::System::gba, kGbaSaveCapacity, title)) {
        battery_.store(false);
        return;
    }
    if (!slots_.open(path, *gba_system_)) faults_->add("No memory for the instant saves of %s.", file_name(path));
    fill_jit_store();
    shades_.store(false, std::memory_order_relaxed);
    if (!platform::display::show_game(window_, gba::kScreenWidth, gba::kScreenHeight,
                                      platform::display::PixelFormat::rgb565)) {
        saves_->close();
        battery_.store(false);
        fail("Game display unavailable.");
        return;
    }
    loaded_ = true;
    gba_ticks_ = 0;
    pace_restart();
    start_cushion();
    gba_system_->set_frame_sink(&gba_sink_);
    if (audio_ && !fast) platform::audio::resume();
    state_.store(GameState::run, std::memory_order_release);
}

// A frame on the GBA, paced and drawn at 1X and 2X as the GB's are, see
// step(). A run ends at the start of VBlank, so each run draws the whole
// frame or none of it.
void Game::step_gba() {
    const bool fast = fast_requested_.load(std::memory_order_relaxed);
    if (fast != fast_.load(std::memory_order_relaxed)) change_speed(fast);
    const unsigned speed = fast ? 2 : 1;
    if (audio_ && !fast) {
        const size_t queued = gba_system_->audio_buffered() + held_.load(std::memory_order_relaxed);
        const size_t target = kGbaTarget + cushion_;
        if (queued >= target) {
            platform::sleep_ms(1);
            return;
        }
        pace_trim(queued, target);
        // The cushion comes down a step a frame once the output plays.
        if (cushion_ && !preroll_.load(std::memory_order_relaxed)) cushion_ -= std::min(cushion_, kCushionStep);
    } else {
        const uint64_t now = platform::monotonic_us();
        if (now < deadline_us_) {
            platform::sleep_ms(static_cast<uint32_t>(std::max<uint64_t>((deadline_us_ - now) / 1000, 1)));
            return;
        }
        // A slow frame does not build up a debt of frames to run back to back.
        deadline_us_ = std::max(deadline_us_, now - kFrameUs / speed);
    }
    const uint32_t buttons = buttons_.fetch_and(0xffff);
    gba_system_->set_buttons(gba_keys(static_cast<uint16_t>(buttons | (buttons >> 16))));
    const uint64_t next = gba_system_->memory.frames + 1;
    const uint64_t began = platform::monotonic_us();
    drawing_ = (!fast || draw_now(began)) && platform::display::game_frame_due(drawn_us_);
    gba_system_->set_drawing(drawing_);
    auto& start = starts_[next % starts_.size()];
    start.us.store(began, std::memory_order_relaxed);
    start.speed.store(speed_changes_, std::memory_order_relaxed);
    start.id.store(next, std::memory_order_release);
    const Tick before = gba_system_->now();
#ifdef GBX_PROFILER
    // A request by the game's frame number, taken before the frame runs,
    // so that a bench from this frame on starts here.
    if (bench_absolute_.load(std::memory_order_relaxed)) {
        if (const uint32_t frames = bench_frames_.exchange(0, std::memory_order_acquire)) {
            bench_ = {};
            bench_.first = bench_skip_.load(std::memory_order_relaxed);
            bench_.last = bench_.first + frames - 1;
            bench_absolute_.store(false, std::memory_order_relaxed);
        }
    }
    if (bench_.last && next == bench_.first) bench_start();
    const auto* jit = gba_system_->jit();
    const uint32_t compiled = jit ? jit->compiled_blocks() : 0, restored = jit ? jit->restored_blocks() : 0;
    const uint64_t misses = gba_rom_ ? gba_rom_->stats().misses : 0, miss_us = gba_rom_ ? gba_rom_->stats().miss_us : 0,
                   wait_us = gb::offload_counters.wait_us[0];
#if defined(__riscv)
    // mhpmcounter8 counts conditional branch mispredictions and
    // mhpmcounter9 conditional branches, see docs/esp32p4-performance.md.
    uint32_t cycles0, retired0, missed0, branches0;
    asm volatile("csrw 0x328, %0" ::"r"(6));
    asm volatile("csrw 0x329, %0" ::"r"(7));
    asm volatile("csrr %0, mcycle" : "=r"(cycles0));
    asm volatile("csrr %0, minstret" : "=r"(retired0));
    asm volatile("csrr %0, 0xb08" : "=r"(missed0));
    asm volatile("csrr %0, 0xb09" : "=r"(branches0));
#endif
#endif
    const Tick elapsed = gba_system_->run_frame() - before;
    const uint64_t ended = platform::monotonic_us();
    work_done(next, began, ended);
#ifdef GBX_PROFILER
#if defined(__riscv)
    uint32_t cycles1, retired1;
    asm volatile("csrr %0, mcycle" : "=r"(cycles1));
    asm volatile("csrr %0, minstret" : "=r"(retired1));
    uint32_t missed1, branches1;
    asm volatile("csrr %0, 0xb08" : "=r"(missed1));
    asm volatile("csrr %0, 0xb09" : "=r"(branches1));
    cpu_cycles_.fetch_add(cycles1 - cycles0, std::memory_order_relaxed);
    cpu_retired_.fetch_add(retired1 - retired0, std::memory_order_relaxed);
    cpu_mispredicts_.fetch_add(missed1 - missed0, std::memory_order_relaxed);
    cpu_branches_.fetch_add(branches1 - branches0, std::memory_order_relaxed);
    if (bench_.last) bench_frame(next, cycles1 - cycles0, retired1 - retired0);
#endif
    // The counters of "stat", as profile_end() takes the GB JIT's, of the
    // frame alone: bench_frame() may have logged since.
    if (const auto run_us = static_cast<uint32_t>(ended - began);
        run_us > worst_run_us_.load(std::memory_order_relaxed)) {
        worst_compiles_.store(jit ? jit->compiled_blocks() - compiled : 0, std::memory_order_relaxed);
        worst_restores_.store(jit ? jit->restored_blocks() - restored : 0, std::memory_order_relaxed);
        worst_misses_.store(gba_rom_ ? static_cast<uint32_t>(gba_rom_->stats().misses - misses) : 0,
                            std::memory_order_relaxed);
        worst_miss_us_.store(gba_rom_ ? static_cast<uint32_t>(gba_rom_->stats().miss_us - miss_us) : 0,
                             std::memory_order_relaxed);
        worst_wait_us_.store(static_cast<uint32_t>(gb::offload_counters.wait_us[0] - wait_us), std::memory_order_relaxed);
        worst_run_us_.store(run_us, std::memory_order_relaxed);
    }
    if (jit) {
        compiled_.store(jit->compiled_blocks(), std::memory_order_relaxed);
        restored_.store(jit->restored_blocks(), std::memory_order_relaxed);
        evictions_.store(jit->evictions(), std::memory_order_relaxed);
        invalidations_.store(jit->invalidations(), std::memory_order_relaxed);
        code_bytes_.store(static_cast<uint32_t>(jit->code_bytes()), std::memory_order_relaxed);
        stored_entries_.store(jit->stored_entries(), std::memory_order_relaxed);
        store_drops_.store(jit->store_drops(), std::memory_order_relaxed);
        store_bytes_.store(static_cast<uint32_t>(jit->store_bytes()), std::memory_order_relaxed);
        store_records_.store(static_cast<uint32_t>(jit->store_records()), std::memory_order_relaxed);
        compiled_bytes_.store(static_cast<uint32_t>(jit->compiled_bytes()), std::memory_order_relaxed);
        interpreted_.store(static_cast<uint32_t>(jit->interpreted()), std::memory_order_relaxed);
    }
#endif
    // The telemetry counts in the GB's ticks (kTicksPerSecond), half the
    // GBA's.
    gba_ticks_ += elapsed;
    emulated_ = gba_ticks_ * kTicksPerSecond / gba::kCyclesPerSecond;
    ran_.store(emulated_, platform::monotonic_us());
    // What the game changed in its save this frame.
    saves_->snapshot(gba_cart_);
    if (!audio_ || fast) deadline_us_ += elapsed * 1'000'000 / gba::kCyclesPerSecond / speed;
}
#endif

void Game::pace_trim(size_t queued, size_t target) {
    // The level before each frame, smoothed over about 16 frames.
    smoothed_ += ((static_cast<int64_t>(queued) << 8) - smoothed_) / 16;
    const int64_t short_by = static_cast<int64_t>(target) - kDeadBand - (smoothed_ >> 8);
    const auto trim = static_cast<int32_t>(std::clamp<int64_t>(short_by * kMaxTrim / kDeadBand, 0, kMaxTrim));
    // set_audio_speed() adds an entry to the sound batch; skip tiny changes.
    if (std::abs(trim - trim_) >= 100 || (trim == 0 && trim_ != 0)) {
        trim_ = trim;
        set_audio_speed(trim);
    }
}

void Game::play() {
    std::array<int16_t, kChunk * 2> chunk;
    while (!stopping_.load()) {
        size_t n = 0;
        bool sound = false;
        {
            const Reader reader(*this);
#if GBX_HAS_GBA
            sound = (reader.gb || reader.gba) && state_.load(std::memory_order_acquire) == GameState::run;
#else
            sound = reader.gb && state_.load(std::memory_order_acquire) == GameState::run;
#endif
            if (sound) {
#if GBX_HAS_GBA
                const size_t queued = reader.gb ? reader.gb->audio_queued() : reader.gba->audio_queued();
#else
                const size_t queued = reader.gb->audio_queued();
#endif
                size_t lowest = lowest_.load(std::memory_order_relaxed);
                while (queued < lowest && !lowest_.compare_exchange_weak(lowest, queued, std::memory_order_relaxed)) {}
                // A start waits for the level the cushion asks for. The
                // empty read skips the frames from before a reset, whose
                // room the mixer would otherwise keep.
                const size_t preroll = preroll_.load(std::memory_order_relaxed);
                if (preroll && queued < preroll) {
#if GBX_HAS_GBA
                    if (reader.gb) reader.gb->read_audio({});
                    else reader.gba->read_audio({});
#else
                    reader.gb->read_audio({});
#endif
                } else {
                    if (preroll) preroll_.store(0, std::memory_order_relaxed);
#if GBX_HAS_GBA
                    if (queued >= kChunk) n = reader.gb ? reader.gb->read_audio(chunk) : reader.gba->read_audio(chunk);
#else
                    if (queued >= kChunk) n = reader.gb->read_audio(chunk);
#endif
                }
            }
        }
        if (!sound) {
            // No game sound: a cue plays on its own.
            if (audio_ && sounds_->playing()) {
                chunk.fill(0);
                sounds_->mix(chunk);
                platform::audio::write(chunk, 100);
            } else {
                platform::sleep_ms(5);
            }
            continue;
        }
        if (!n) {
            platform::sleep_ms(1);
            continue;
        }
        sounds_->mix({chunk.data(), n * 2});
        held_.store(n, std::memory_order_relaxed);
        if (audio_) platform::audio::write({chunk.data(), n * 2}, 100);
        held_.store(0, std::memory_order_relaxed);
    }
}

}  // namespace gbx::app
