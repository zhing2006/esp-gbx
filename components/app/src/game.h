#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>

#include "faults.h"
#include "gbx/app.h"
#include "gbx/display.h"
#include "gbx/files.h"
#include "gbx/gb/system.h"
#include "gbx/gba/system.h"
#include "gbx/systems.h"
#include "gbx/worker.h"
#include "replay.h"
#include "saves.h"
#include "slots.h"
#include "sounds.h"

namespace gbx::app {

// Runs one game at a time, on a GB or a GBA, of those the build runs (see
// gbx/systems.h; the GBA's headers stay for its constants). The emulation
// task owns the system on the CPU core; an audio task on the other core
// moves its PCM to the output, with the menus' sound cues, which it also plays while no game runs. Frames
// go from the PPU's worker straight to the display's game layer, so the
// emulation task never converts or copies them. The UI reads counters and
// sends requests, which the emulation task takes at frame boundaries: pause
// and resume, a backend, the speed, quitting. The cartridge save goes
// through Saves, the instant saves through Slots.
//
// Tasks: emulation on core 1 at priority 5. Audio on core 0 at priority 7,
// above the PPU and APU worker (5), the present task (4) and LVGL (3): it
// only copies a few KiB at a time, and a late copy is heard. At 1X the
// worker sleeps whenever it runs out of work, and the emulation task wakes
// it every half queue: core 1 has time to spare for the wakeups, and core 0
// gives the time the worker would poll to the present task and LVGL. At 2X
// the worker polls, as core 1 then has the most work.
//
// With the GB's JIT at 1X, core 1 also converts each game frame for the
// display, below the emulation, see lend_emulator_core(): core 0 has the
// worker, the present task and LVGL, and core 1 the most time to spare. At
// 2X core 1 has less left than core 0. With the interpreter at 1X it has a
// few points more, fewer than the conversion takes, so moving it would leave
// core 1 the busier one. The GBA keeps core 1 busy on either backend while
// core 0 has time left after its renderer and mixer (in Mario Golf on the
// JIT: core 1 idle 0 %, core 0 idle 23 to 39 %), and a core 1 lent for
// conversion never leaves its idle task a tick when the game falls behind.
// In these cases the present task converts the frames on core 0.
//
// A chip with one core, the ESP32-C5, runs every task on it. There the
// emulation task runs at priority 1, with the main task's input loop and
// below LVGL, the present task and the app's file tasks, so that the touch,
// the screen and the cards keep up when the emulation falls behind. It
// draws the lines and mixes the sound itself, see gb::Offload.
class Game final : public gb::FrameSink {
public:
    // Output frames per second. 48 kHz is what the codecs and SDL use without
    // resampling of their own.
    static constexpr uint32_t kAudioRate = 48000;

    // Starts the tasks. `window` is the game window of the asset pack.
    bool initialize(std::span<const int16_t, 4> window, Saves& saves, Faults& faults, Sounds& sounds);
    // Ends the tasks and releases the game, for the PC build's exit. A
    // changed save is written first.
    void stop();

    // Loads and runs the ROM at a card path on the emulation task, replacing
    // the running game; its changed save is written first. `cgb` puts the
    // cartridge in a GBC, see run_mode(). The header tells a GBA game, see
    // load_gba().
    void start(const char* path, Backend backend, bool cgb = false);
    // For the GBA games started after it: the BIOS file, a path as
    // platform::files takes it, and the ROM cache's budget in bytes. Empty
    // and 0 take the card's bios/gba_bios.bin and kGbaRomCache.
    void set_gba_options(const char* bios, size_t rom_cache_bytes);
    // Whether the game loaded runs on the GBA. Any task.
    bool gba() const { return GBX_HAS_GBA && gba_.load(std::memory_order_acquire); }
    GameState state() const { return state_.load(std::memory_order_acquire); }
    // Counts start() calls, so a caller can tell a new load from the state
    // of an old one, however fast the load ends.
    uint32_t starts() const { return start_count_.load(std::memory_order_acquire); }
    // Why the latest load or run failed, a sentence for the user. Valid
    // while the state is ERROR.
    const char* error() const { return error_; }
    // Loading progress, 0 to 100.
    int load_progress() const;

    // Questions the load asks before the game runs, see Saves::Found and
    // the clock check. The state stays LOADING until answer().
    enum class Prompt : uint8_t { none, mismatch, damaged, clock };
    // CLOCK NOT SET: the board's clock is not trusted now, is behind the
    // save's time, or was not trusted when the save was taken.
    enum class Clock : uint8_t { untrusted, behind, untrusted_save };
    struct Question {
        Prompt prompt = Prompt::none;
        Clock clock = Clock::untrusted;
        char save_title[17] = {};
        char rom_title[17] = {};
    };
    Question question() const;
    // yes: LOAD SAVE or OK; no: START NEW; cancel: the game does not start.
    enum class Answer : uint8_t { yes, no, cancel };
    void answer(Answer answer);

    // Stops at the next frame boundary; the state turns PAUSED. The game
    // layer keeps the frame until the caller hides it.
    void pause();
    // Runs on from where it paused, showing the game layer with the paused
    // frame. The cartridge clock counts the pause as time that passed.
    void resume();
    // The frame the game paused or stopped on, BGR555, valid while PAUSED,
    // or ERROR after running: a GB's 160x144 in GB mode in the current
    // palette, in color as the game set it, or `fixed` by COLOR FIX while
    // that is on; a GBA's 240x160 as the game set it. `out` takes
    // kMaxFramePixels; returns the frame's width, its height following.
    static constexpr size_t kMaxFramePixels = app::kMaxFramePixels;
    int paused_pixels(std::span<uint16_t> out, bool fixed = false) const;
    // Takes the backend at the next frame boundary or while paused. A
    // backend that cannot start is reported as a fault and the old one stays.
    void set_backend(Backend backend);
    Backend backend() const { return backend_.load(); }
    // How the game loaded last runs, see gb::run_mode(). Any task.
    gb::Mode mode() const { return mode_.load(std::memory_order_acquire); }
    // COLOR FIX: the LCD color correction of the frames of a game in color,
    // from the next frame shown, see color_fix.h. Any task.
    void set_color_fix(bool on);
    bool color_fix() const { return color_fix_.load(std::memory_order_relaxed); }
    // The speed switch: 2X runs twice as fast as the original, without sound,
    // and draws only the frames the screen can show, see draw_now(). Taken
    // at the next frame boundary, also by a game loaded later.
    void set_fast(bool fast);
    // BGR555 colors of the four shades, lightest first. Any task.
    void set_palette(const std::array<uint16_t, 4>& palette);
    std::array<uint16_t, 4> palette() const;

    // The instant saves of the game: the list, and the progress of a write.
    const Slots& slots() const { return slots_; }
    // While PAUSED: copies the state at once and has it written to `slot`,
    // 1 to 4, while the menus or the game go on.
    void save_slot(int slot);
    // While PAUSED: reads `slot` and goes on paused from its state.
    // `other_rom` takes a state of a ROM with another identity.
    void load_slot(int slot, bool other_rom);
    // How the latest load_slot() went. CLOCK: loaded, but the cartridge
    // clock did not move on by the time since the state was taken; `clock`
    // says why. FAILED: `kind` says what the slot held.
    enum class SlotLoad : uint8_t { none, loading, loaded, clock, failed };
    struct SlotResult {
        SlotLoad load = SlotLoad::none;
        Clock clock = Clock::untrusted;
        Slots::Kind kind = Slots::Kind::empty;
    };
    SlotResult slot_result() const;

    // Quitting, while PAUSED or ERROR after running: finish() takes the last
    // changes to the save and has it written now; finished() turns true once
    // it has, and an instant save being written is on the card. Then
    // unload() ends the game; the state turns NO ROM.
    void finish();
    bool finished() const {
        return finished_.load(std::memory_order_acquire) == finish_.load() && slots_.work() != Slots::Work::writing;
    }
    void unload();
    // The ROM's card path; empty without a game.
    void path(char* out, size_t size) const;
    bool has_battery() const { return battery_.load(std::memory_order_relaxed); }

    // Held buttons in GB bits, with platform::keys::kL and kR for the GBA.
    // A press shorter than a frame still reaches the next frame.
    void set_buttons(uint16_t buttons);

    // Counters since the latest load, for the telemetry. Each count comes
    // with the wall time it was reached at, so rates need not sample them.
    struct Counters {
        uint32_t session = 0;   // counts loads; a change restarts the windows
        Backend backend = Backend::interpreter;
        uint64_t emulated = 0;  // ticks run
        uint64_t emulated_us = 0;    // when the latest frame run ended
        uint64_t frames = 0;    // frames emulated up to the latest the PPU delivered
        uint64_t last_frame_us = 0;  // when the latest of them arrived
        size_t queued = 0;      // PCM frames waiting to go to the output
        size_t lowest = 0;      // the fewest since the previous read
        uint32_t underruns = 0;
        bool audio = false;     // there is an output
        bool fast = false;      // 2X, without sound; the session starts over on a change
        std::array<uint32_t, 120> work{};  // frame emulation samples, microseconds
        uint32_t work_count = 0;           // samples taken, the newest work_count are valid
        uint32_t work_base = 0;            // samples taken before the first frame at the speed
        // Intervals between frames handed to the display, microseconds, in
        // the same form as work.
        std::array<uint32_t, 120> gap{};
        uint32_t gap_count = 0, gap_base = 0;
    };
    // Reading also starts the next span for `lowest`. One reader at a time.
    void read(Counters& out);

    // Copies the next frame into `out`, kMaxFramePixels, see
    // app::capture_frame(). `width` is the frame's, 160 or 240.
    bool capture(uint16_t* out, uint32_t timeout_ms, int& width);
#ifdef GBX_PROFILER
    // The slowest frame run since the previous call with the JIT blocks
    // compiled and restored during it, and the JIT's running totals.
    struct Profile {
        uint32_t run_us, compiles, restores;
        uint32_t compiled, restored, evictions, invalidations, code_bytes, store_bytes, store_records;
        uint32_t compiled_bytes, interpreted, stored_entries, store_drops;
        // The GBA's frames on the CPU core: mcycle and minstret in
        // millions, conditional branches and their mispredictions in
        // thousands.
        uint32_t cycles, retired, branches, mispredicts;
        // The GBA's slowest frame: ROM pages it read on a miss, and how
        // long it waited for them and for the renderer.
        uint32_t misses, miss_us, wait_us;
    };
    Profile take_profile();
    // Times the GBA frames from `skip` to `skip + frames` after the next
    // slot load, or with `absolute` from frame `skip` of the game running:
    // the cycles of the CPU core's frames, the parts of
    // gba::arm7_cycle_counts, and the renderer's time.
    void bench(uint32_t skip, uint32_t frames, bool absolute) {
        bench_skip_.store(skip, std::memory_order_relaxed);
        bench_absolute_.store(absolute, std::memory_order_relaxed);
        bench_frames_.store(frames, std::memory_order_release);
    }
    // The JIT's careful accesses since the previous call, see
    // gb::Sm83JitCounters, with the six IO registers written most.
    struct SlowAccess {
        uint32_t reads[6], writes[6], horizon_resets;
        std::array<uint8_t, 6> io;
        std::array<uint32_t, 6> io_writes;
    };
    SlowAccess take_slow_access();
    // Key recordings; a request applies to the next start().
    Replay& replay() { return replay_; }
#endif

private:
    static constexpr unsigned kEmulationPriority = 5;
    static constexpr unsigned kSingleCorePriority = 1;

    // A count and the wall time it was reached at, written by one task at a
    // time and read as a pair by another.
    class Mark {
    public:
        void store(uint64_t count, uint64_t us);
        void load(uint64_t& count, uint64_t& us) const;
    private:
        std::atomic<uint32_t> sequence_{0};  // odd while a store is under way
        std::atomic<uint64_t> count_{0}, us_{0};
    };

    // FrameSink, on the PPU's worker, or on the emulation task where the
    // chip has one core.
    uint16_t* frame_buffer() override;
    // The buffer the display takes the next frame from, due or not.
    uint16_t* drawing_buffer() override { return platform::display::game_frame_buffer(); }
    bool shades() const override { return shades_.load(std::memory_order_relaxed); }
    void frame_ready(uint64_t id) override;
    // The game layer's pixels for the GB game loaded: GB mode's shades
    // where the display takes them, see platform::display::PixelFormat.
    platform::display::PixelFormat gb_format() const;
#if GBX_HAS_GBA
    // The GBA's frames and sound go the same way.
    class GbaSink final : public gba::FrameSink {
    public:
        explicit GbaSink(Game& game) : game_(game) {}
        uint16_t* frame_buffer() override { return game_.frame_buffer(); }
        void frame_ready(uint64_t id) override { game_.frame_ready(id); }
    private:
        Game& game_;
    };
#endif
    void work_done(uint64_t id, uint64_t began, uint64_t now);
    void gap_done(uint64_t id, uint64_t now);

    static void emulate_entry(void* self) { static_cast<Game*>(self)->emulate(); }
    static void play_entry(void* self) { static_cast<Game*>(self)->play(); }
    void emulate();
    void play();
    void load(const char* path, Backend backend, bool cgb);
#if GBX_HAS_GBA
    void load_gba(const char* path, Backend backend, int64_t size, const char* title);
    void step_gba();
#endif
    // Frees all the last game held, when it ends and before another loads.
    void release();
    // Sets jit_reserved_ aside unless it is already.
    void reserve_jit();
    // Gives the running game's JIT its second level from the PSRAM left,
    // where the board says so, see GBX_GB_JIT_STORE_LEAVE_KB and
    // GBX_GBA_JIT_STORE_LEAVE_KB.
    void fill_jit_store();
    void unshare();
    bool load_save(const char* path, std::span<const uint8_t, 24> identity, Saves::System system, size_t capacity,
                   const char* title);
#if GBX_HAS_GBA
    SaveCartridge& cartridge() { return gba() ? static_cast<SaveCartridge&>(gba_cart_) : gb_cart_; }
#else
    SaveCartridge& cartridge() { return gb_cart_; }
#endif
    // The running system's sound settings and level.
    void set_audio_speed(int32_t ppm);
    size_t audio_buffered() const;
    bool has_rtc() const;
    void advance_rtc(uint64_t us);
    Answer ask(const Question& question);
    void step();
    void enter_pause();
    void keep_frame();
    void show_colors() const;
    void count_pause();
    void leave_pause();
    void change_backend();
    void change_speed(bool fast);
    void lend_core();
    void save_slot_now(int slot);
    void restore_slot();
    void end_game(bool keep_changes);
    void pace_restart();
    void start_cushion();
    void pace_trim(size_t level, size_t target);
    bool draw_now(uint64_t now);
    size_t level() const;
    void fail(const char* reason);
    void stop_running(const char* reason);

    std::array<int16_t, 4> window_{};
    Saves* saves_ = nullptr;
    Slots slots_;
    Faults* faults_ = nullptr;
    Sounds* sounds_ = nullptr;
    platform::Worker emulation_, audio_task_;
    platform::Notification wake_;
    std::atomic<GameState> state_{GameState::no_rom};
    std::atomic<bool> stopping_{false};
    std::atomic<Backend> backend_{Backend::interpreter};
    std::atomic<gb::Mode> mode_{gb::Mode::dmg};
    std::atomic<bool> color_fix_{false};
    std::atomic<uint64_t> palette_{0};  // four BGR555 colors, see set_palette()
    std::atomic<bool> battery_{false};
    std::atomic<uint32_t> start_count_{0};
    char error_[160] = {};  // written before the state turns ERROR

    // Requests from the UI's task, taken by the emulation task.
    std::atomic<bool> pause_requested_{false}, resume_requested_{false}, unload_requested_{false};
    std::atomic<int> backend_requested_{-1};
    std::atomic<bool> fast_requested_{false};
    std::atomic<uint32_t> finish_{0}, finished_{0};
    std::atomic<int> slot_save_{0}, slot_load_{0};  // the slot asked for, 0 for none
    std::atomic<bool> slot_other_rom_{false};
    mutable std::mutex slot_mutex_;
    SlotResult slot_result_;  // under slot_mutex_
    mutable std::mutex question_mutex_;
    Question question_;
    std::atomic<int> answer_{-1};
    std::atomic<size_t> load_done_{0}, load_size_{0};

    // A start request, from the UI's task to the emulation task.
    mutable std::mutex request_mutex_;
    char request_path_[platform::files::kMaxPath] = {};  // empty when the path did not fit
    Backend request_backend_ = Backend::interpreter;
    bool request_cgb_ = false;
    std::atomic<bool> requested_{false};

    // Emulation task.
    std::unique_ptr<gb::System> system_;
    // The block that holds the code pool of either system's JIT, see
    // gb::System::select_backend(): set aside at initialize(), while
    // internal RAM is in one piece, and kept for every game until stop().
    // Freed between games, it would let any allocation made meanwhile, such
    // as a task's first use of something it creates once, split it.
    platform::ExecutableMemory jit_reserved_{};
    // The running system for the other tasks, which read its sound and set
    // the GB's palette: system_ or gba_system_, once a ROM has loaded.
    // A task counts itself in readers_ while it uses one, see Reader.
    std::atomic<gb::System*> shared_{nullptr};
#if GBX_HAS_GBA
    std::atomic<gba::System*> gba_shared_{nullptr};
#endif
    mutable std::atomic<uint32_t> readers_{0};
    class Reader;
    GbSaveCartridge gb_cart_;
#if GBX_HAS_GBA
    GbaSaveCartridge gba_cart_;
#endif
    uint8_t* rom_ = nullptr;
    bool loaded_ = false;  // system_ holds a game, running, paused or stopped
    char path_[platform::files::kMaxPath] = {};  // under request_mutex_
    // The frame paused on: shades in GB mode, BGR555 in color.
    std::array<uint8_t, gb::kFramePixels> paused_shades_{};
    std::array<uint16_t, gb::kFramePixels> paused_colors_{};

    // The GBA, created by a game of it and released with it: the
    // system, its ROM's cache, the BIOS in internal RAM, which the CPU reads
    // for every BIOS opcode, and the frame paused on (gba::kFramePixels
    // BGR555, external memory).
    std::atomic<bool> gba_{false};
#if GBX_HAS_GBA
    GbaSink gba_sink_{*this};
    std::unique_ptr<gba::System> gba_system_;
    std::unique_ptr<gba::RomCache> gba_rom_;
    uint8_t* gba_bios_ = nullptr;
    uint16_t* gba_paused_ = nullptr;
    Tick gba_ticks_ = 0;  // since the load
#endif
    char gba_bios_path_[platform::files::kMaxPath] = {};  // under request_mutex_
    size_t gba_rom_cache_ = 0;                            // under request_mutex_
    int slot_reading_ = 0;     // the slot Slots reads for load_slot()
    bool slot_other_rom_reading_ = false;
    // The game is paused, whatever state_ says once a new start is asked:
    // the cartridge clock has counted the pause up to paused_us_.
    bool paused_ = false;
    uint64_t paused_us_ = 0;
    bool audio_ = false;
    bool suspended_ = false;
    std::atomic<bool> fast_{false};  // the speed running, written by the emulation task
    int32_t trim_ = 0;      // set_audio_speed(), parts per million
    int64_t smoothed_ = 0;  // level, frames << 8
    // A GBA game's start, after its load or the load of an instant save:
    // the frames the audio level may stand above kGbaTarget, and the level
    // the output waits for before it plays, see start_cushion().
    size_t cushion_ = 0;
    std::atomic<size_t> preroll_{0};
    bool slot_restored_ = false;  // an instant save loaded during the pause
    uint64_t deadline_us_ = 0;
    uint64_t rested_us_ = 0;  // the emulation task's last rest, see step()
    // 2X: when the next frame drawn should show, when the latest frame
    // began, and the smoothed time between frame starts, see draw_now().
    uint64_t show_us_ = 0, began_us_ = 0, interval_us_ = 0;

    std::atomic<uint32_t> buttons_{0};  // held, and presses not yet run (high half)
    std::atomic<size_t> held_{0};       // frames the audio task took and has not written

    // Counters.
    std::atomic<uint32_t> session_{0};
    uint64_t emulated_ = 0;  // emulation task
    uint64_t frames_ = 0;    // PPU worker, or the emulation task while no sink is set
    Mark ran_, delivered_;   // emulated_ and frames_ with their times
    std::atomic<size_t> lowest_{SIZE_MAX};
    std::atomic<uint32_t> underruns_at_load_{0};
    uint64_t last_id_ = 0;  // PPU worker: the latest frame delivered
    uint64_t frame_began_us_ = 0;  // emulation task: when the frame under way began
    // Emulation task: whether the frame under way is drawn, and how long the
    // latest frame drawn took, which a frame not drawn says nothing about.
    bool drawing_ = true;
    uint64_t drawn_us_ = 0;
    struct Start {
        std::atomic<uint64_t> id{0};
        std::atomic<uint64_t> us{0};
        std::atomic<uint32_t> speed{0};  // speed_changes_ when it began
    };
    std::array<Start, 8> starts_;  // when emulation of a frame ID began
    std::array<std::atomic<uint32_t>, 120> work_{};
    std::atomic<uint32_t> work_count_{0};
    std::atomic<uint32_t> work_base_{0};  // see Counters
    std::array<std::atomic<uint32_t>, 120> gap_{};
    std::atomic<uint32_t> gap_count_{0};
    std::atomic<uint32_t> gap_base_{0};
    uint64_t gap_us_ = 0;  // when the worker handed over the frame before
    // Speed changes, counted by the emulation task, and those of the frames
    // the latest samples are of: work's on the emulation task, gap's on the
    // worker.
    uint32_t speed_changes_ = 0, work_speed_ = 0, gap_speed_ = 0;

    // The game layer takes shades, see gb_format(); set before it shows.
    std::atomic<bool> shades_{false};
    // Where the PPU's worker copies the next frame, see capture().
    std::atomic<uint16_t*> capture_{nullptr};
    std::atomic<bool> captured_{false};
#ifdef GBX_PROFILER
    // See take_profile(). The emulation task writes them, a reader on
    // another task takes them.
    struct RunStart { uint64_t us; uint32_t compiled, restored; };
    RunStart profile_start() const;
    // Returns the run's duration in microseconds.
    uint32_t profile_end(const RunStart& start);
    std::atomic<uint32_t> worst_run_us_{0}, worst_compiles_{0}, worst_restores_{0}, compiled_{0}, restored_{0};
    std::atomic<uint32_t> worst_misses_{0}, worst_miss_us_{0}, worst_wait_us_{0};
    std::atomic<uint32_t> evictions_{0}, invalidations_{0}, code_bytes_{0}, store_bytes_{0}, store_records_{0};
    std::atomic<uint32_t> compiled_bytes_{0}, interpreted_{0}, stored_entries_{0}, store_drops_{0};
    std::atomic<uint64_t> cpu_cycles_{0}, cpu_retired_{0}, cpu_branches_{0}, cpu_mispredicts_{0};
    // bench(): the request, then the frames it times and its sums so far.
    std::atomic<uint32_t> bench_skip_{0}, bench_frames_{0};
    std::atomic<bool> bench_absolute_{false};
    struct Bench {
        uint64_t first = 0, last = 0, cycles = 0, retired = 0, began_us = 0, wait_us = 0;
        uint32_t draw_us = 0, draw_cycles = 0, draw_retired = 0;
        std::array<uint32_t, gba::kRenderParts> part_cycles{}, part_retired{};
        uint32_t drawn = 0, waits = 0, compiled = 0, restored = 0, evictions = 0;
        std::array<uint32_t, 16> finds{};
        std::array<uint64_t, 28> parts{};
        std::array<std::array<uint32_t, 16 + 0x108>, 2> accesses{};
        std::array<uint32_t, 32> stubs{};
        uint64_t rom_misses = 0, rom_miss_us = 0, payload_wait_us = 0;
        uint32_t payload_waits = 0;
        uint64_t ppu_frame = 0;
    } bench_;
#if GBX_HAS_GBA
    void bench_start();
    void bench_frame(uint64_t frame, uint32_t cycles, uint32_t retired);
#endif
    // The JIT's careful access counts as of the last run, for
    // take_slow_access(), and the counts it took last.
    std::array<std::atomic<uint32_t>, 6> slow_reads_{}, slow_writes_{};
    std::array<std::atomic<uint32_t>, 128> slow_io_{};
    std::atomic<uint32_t> horizon_resets_{0};
    struct { uint32_t reads[6], writes[6], io[128], horizon_resets; } slow_seen_{};
    Replay replay_;
#endif
};

}  // namespace gbx::app
