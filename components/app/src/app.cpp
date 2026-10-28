#include "gbx/app.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <new>

#include "gbx/audio.h"
#include "gbx/display.h"
#include "gbx/files.h"
#include "gbx/platform.h"
#include "gbx/ui/controls.h"
#include "gbx/ui/pack.h"
#if GBX_UI_LVGL
#include "faults.h"
#include "game.h"
#include "gbx/ui/game_page.h"
#include "gbx/ui/menu.h"
#include "library.h"
#include "menus.h"
#include "saves.h"
#include "settings.h"
#include "sounds.h"
#endif

namespace gbx::app {

namespace {

constexpr const char* kTag = "app";

#if GBX_UI_LVGL
constexpr uint32_t kInputPeriodMs = 10;  // one ReleaseFilter sample
constexpr int kMaxTouches = 10;
// Sampling in docs/ui-layouts/telemetry-fields.json: the VFD's readings
// update once a second and rates cover that second, frame work the latest
// 120 samples with fewer than 20 shown as WARMUP. Redrawing the readings
// four times a second cost about 7 % of core 0 in LVGL and the present
// task on the Sila P4. The lamps and the run state show as soon as they
// change, see update().
constexpr uint64_t kStatusPeriodUs = 1'000'000;
constexpr size_t kWindowPoints = 2;
constexpr uint32_t kWarmupSamples = 20;
// No new frame for this long, as with the LCD off, makes the frame work old.
constexpr uint64_t kStaleUs = 2'000'000;
// Rates divide counts by the wall time between the instants the counts were
// reached, the end of a frame run or a frame's arrival. Counts sampled at
// the status period would be off by up to a frame, 1.7 % of the window. A
// count not reached for this long, as when emulation stops, ends its span
// at the sample time instead, so the rate falls.
constexpr uint64_t kMarkLateUs = 100'000;

ui::Pack pack;
ui::GamePage page;
ui::MenuView view;
Faults faults;
Saves saves;
Sounds sounds;
Library library;
Game game;
// In external memory like the objects above. A global Menus starts with
// non-zero values, so it would stay in internal .data, which linker.lf
// does not move.
Menus* menus = nullptr;
bool ready = false;
// For the serial console, see app.h.
std::atomic<uint16_t> console_keys{0};
std::atomic<int> console_speed{-1};  // 0 for 1X, 1 for 2X, -1 for no change
std::atomic<int> console_slot{0};    // the slot to load, 0 for none
std::atomic<uint32_t> status_period_ms{0};
ui::Status shown_status;  // what the game page shows
std::atomic<bool> status_due{false};
std::mutex status_output;  // Held while status lines print, see hold_status().
double frame_work_max_ms = 0;  // the longest of the frame work samples
struct InputState {
    std::array<ui::ControlHit, ui::kMaxControls> hits{};
    std::array<ui::ControlHit, ui::kMaxControls> previous{};
    ui::ReleaseFilter filter;
    uint64_t next_sample_us = 0;
    bool double_speed = false;
} input;

// Counters at the end of each status period; the oldest of the window first.
struct Point {
    uint64_t us;
    uint64_t emulated;
    uint64_t emulated_us;
    uint64_t frames;
    uint64_t frames_us;
    platform::CpuIdle idle;
    int64_t shown;
    uint32_t underruns;
    size_t lowest;  // since the previous point
};
struct Telemetry {
    Game::Counters counters;
    std::array<Point, kWindowPoints> points{};
    size_t count = 0;
    uint32_t session = 0;
    uint64_t next_us = 0;
    // The readings shown while the game last ran, which stand still while
    // it is paused and while the windows fill again after a resume or a
    // speed change, rather than going blank; a load drops them.
    ui::Status shown;
    bool has_shown = false;
    uint32_t starts = 0;
} telemetry;

// Names of the pressed controls, for the log, such as "dpad:up a".
void describe(std::span<const ui::ControlHit> hits, char* out, size_t size) {
    static constexpr const char* kDirections[] = {"",         ":up",       ":down",      ":left",      ":right",
                                                  ":up_left", ":up_right", ":down_left", ":down_right"};
    size_t n = 0;
    out[0] = '\0';
    for (size_t i = 0; i < hits.size() && n < size; ++i) {
        if (!hits[i].touched) {
            continue;
        }
        auto id = ui::field(pack.controls()[i].id);
        n += std::snprintf(out + n, size - n, "%s%.*s%s", n ? " " : "", static_cast<int>(id.size()), id.data(),
                           kDirections[static_cast<int>(hits[i].direction)]);
    }
}

// The keys the controls press, in the bit order of keys::read().
uint16_t control_keys(std::span<const ui::ControlHit> hits) {
    using ui::DpadDirection;
    uint16_t keys = 0;
    for (size_t i = 0; i < hits.size(); ++i) {
        if (!hits[i].touched) {
            continue;
        }
        const auto& control = pack.controls()[i];
        if (control.kind == ui::pack_format::ControlKind::dpad) {
            switch (hits[i].direction) {
                case DpadDirection::none: break;
                case DpadDirection::up: keys |= 4; break;
                case DpadDirection::down: keys |= 8; break;
                case DpadDirection::left: keys |= 2; break;
                case DpadDirection::right: keys |= 1; break;
                case DpadDirection::up_left: keys |= 4 | 2; break;
                case DpadDirection::up_right: keys |= 4 | 1; break;
                case DpadDirection::down_left: keys |= 8 | 2; break;
                case DpadDirection::down_right: keys |= 8 | 1; break;
            }
            continue;
        }
        auto id = ui::field(control.id);
        if (id == "a") keys |= 16;
        else if (id == "b") keys |= 32;
        else if (id == "select") keys |= 64;
        else if (id == "start") keys |= 128;
        else if (id == "l") keys |= platform::keys::kL;
        else if (id == "r") keys |= platform::keys::kR;
        else if (id == "menu") keys |= platform::keys::kMenu;
    }
    return keys;
}

// The first touch in the game window, where the menus show.
MenuInput window_touch(std::span<const platform::touch::Point> points) {
    const auto& w = pack.layout().display;
    for (const auto& p : points) {
        if (p.x >= w[0] && p.x < w[0] + w[2] && p.y >= w[1] && p.y < w[1] + w[3]) return {0, true, p.x, p.y};
    }
    return {};
}

// Reads the settings file; a missing or short card leaves the defaults.
Settings read_settings() {
    Settings settings = default_settings();
    char path[64];
    settings_path(path, sizeof path);
    std::array<uint8_t, 1024> text;
    const int64_t size = platform::files::read_start(path, text);
    if (size > 0) {
        parse_settings({reinterpret_cast<const char*>(text.data()), static_cast<size_t>(std::min<int64_t>(size, text.size()))},
                       settings);
    }
    return settings;
}

ui::Reading valid(double value) {
    return {ui::Reading::State::valid, value};
}

// Counts per second between two marks, see kMarkLateUs.
double rate(uint64_t first, uint64_t first_us, uint64_t last, uint64_t last_us, uint64_t now) {
    if (now > last_us + kMarkLateUs) last_us = now;
    return last_us > first_us ? (last - first) * 1e6 / (last_us - first_us) : 0;
}

// The lamps and the run state into `s`, leaving its readings; true while
// the game runs.
bool fill_state(ui::Status& s) {
    s.error = faults.count() > 0;
    switch (saves.state()) {
        case Saves::State::none: s.save = ui::Status::Save::none; break;
        case Saves::State::clean: s.save = ui::Status::Save::clean; break;
        case Saves::State::dirty: s.save = ui::Status::Save::dirty; break;
        case Saves::State::writing: s.save = ui::Status::Save::writing; break;
        case Saves::State::error: s.save = ui::Status::Save::error; break;
    }
    s.machine = ui::Status::Machine::none;
    s.backend = ui::Status::Backend::none;
    switch (game.state()) {
        case GameState::no_rom: s.run = ui::Status::Run::no_rom; return false;
        case GameState::loading: s.run = ui::Status::Run::loading; return false;
        case GameState::error: s.run = ui::Status::Run::error; return false;
        case GameState::paused: s.run = ui::Status::Run::pause; break;
        case GameState::run: s.run = ui::Status::Run::run; break;
    }
    s.machine = game.gba()                    ? ui::Status::Machine::gba
                : game.mode() == gb::Mode::dmg ? ui::Status::Machine::gb
                                               : ui::Status::Machine::gbc;
    s.backend = game.backend() == Backend::jit ? ui::Status::Backend::jit : ui::Status::Backend::interpreter;
    return s.run == ui::Status::Run::run;
}

// The shell's status from the game's counters, see docs/debug-design.md.
ui::Status status(uint64_t now) {
    using State = ui::Reading::State;
    ui::Status s;
    const auto heap = platform::heap_stats();
    if (heap.known) {
        s.heap_internal_free_kib = valid(heap.internal_free / 1024.0);
        s.heap_psram_free_mib = valid(heap.external_free / (1024.0 * 1024.0));
    } else {
        s.heap_internal_free_kib = s.heap_psram_free_mib = {State::unsupported, 0};
    }
    auto& c = telemetry.counters;
    game.read(c);
    if (c.session != telemetry.session) {
        telemetry.session = c.session;
        telemetry.count = 0;
    }
    if (const uint32_t starts = game.starts(); starts != telemetry.starts) {
        telemetry.starts = starts;
        telemetry.has_shown = false;
    }
    const ui::Status& held = telemetry.shown;
    if (!fill_state(s)) {
        // Paused, the readings stay where the game left them; MEM goes on.
        if (s.run == ui::Status::Run::pause && telemetry.has_shown) {
            s.fast = held.fast;
            s.speed_percent = held.speed_percent;
            s.emu_fps = held.emu_fps;
            s.drw_fps = held.drw_fps;
            s.frame_work_p95_ms = held.frame_work_p95_ms;
            s.frame_work_avg_ms = held.frame_work_avg_ms;
            s.frame_sample_count = held.frame_sample_count;
            s.frame_gap_p95_ms = held.frame_gap_p95_ms;
            s.frame_gap_avg_ms = held.frame_gap_avg_ms;
            s.audio_queue_ms = held.audio_queue_ms;
            s.audio_min_ms = held.audio_min_ms;
            s.audio_underruns_total = held.audio_underruns_total;
            s.audio_underruns_window = held.audio_underruns_window;
            s.idle_core0_percent = held.idle_core0_percent;
            s.idle_core1_percent = held.idle_core1_percent;
        }
        return s;
    }
    s.fast = c.fast;

    auto& points = telemetry.points;
    if (telemetry.count == points.size()) {
        std::copy(points.begin() + 1, points.end(), points.begin());
        --telemetry.count;
    }
    points[telemetry.count++] = {now,
                                 c.emulated,
                                 c.emulated_us,
                                 c.frames,
                                 c.last_frame_us,
                                 platform::cpu_idle(),
                                 platform::display::game_frames_shown(),
                                 c.underruns,
                                 c.lowest};
    const Point& first = points[0];
    const Point& last = points[telemetry.count - 1];
    if (telemetry.count >= 2) {
        const double seconds = (last.us - first.us) / 1e6;
        s.speed_percent = valid(rate(first.emulated, first.emulated_us, last.emulated, last.emulated_us, now) * 100.0 /
                                kTicksPerSecond);
        s.emu_fps = valid(rate(first.frames, first.frames_us, last.frames, last.frames_us, now));
        // A display that cannot tell when a buffer is shown leaves DRW uncollected.
        if (last.shown >= 0 && first.shown >= 0) {
            s.drw_fps = valid((last.shown - first.shown) / seconds);
        }
        // The share of the window each core spent in its idle task.
        ui::Reading* idle[] = {&s.idle_core0_percent, &s.idle_core1_percent};
        const double span = static_cast<double>(last.idle.at_us - first.idle.at_us);
        for (int core = 0; core < 2; ++core) {
            if (!last.idle.known || core >= last.idle.cores) {
                *idle[core] = {State::unsupported, 0};
            } else if (span > 0) {
                *idle[core] = valid((last.idle.idle_us[core] - first.idle.idle_us[core]) * 100.0 / span);
            }
        }
    }

    if (c.fast) {
        // 2X makes no sound: AUDIO reads OFF, and no level or underrun
        // counts, so they are not collected.
    } else if (c.audio) {
        s.audio_queue_ms = valid(c.queued * 1000.0 / Game::kAudioRate);
        size_t lowest = SIZE_MAX;
        for (size_t i = 1; i < telemetry.count; ++i) {
            lowest = std::min(lowest, points[i].lowest);
        }
        if (lowest != SIZE_MAX) {
            s.audio_min_ms = valid(lowest * 1000.0 / Game::kAudioRate);
        }
        s.audio_underruns_total = valid(c.underruns);
        s.audio_underruns_window = valid(last.underruns - first.underruns);
    } else {
        s.audio_queue_ms = s.audio_min_ms = s.audio_underruns_total = s.audio_underruns_window = {State::unsupported, 0};
    }

    // Frame work and the intervals count from the latest speed change, as
    // 1X and 2X are counted apart. A load can start the samples over
    // between the reads.
    // The worker can publish after this UI update took its timestamp. Such
    // a frame is fresh; unsigned subtraction would incorrectly mark it stale.
    const bool frame_stale = now > c.last_frame_us && now - c.last_frame_us > kStaleUs;
    // The p95 and the mean of the newest samples, which the ring holds up
    // to its size; the longest goes to `longest`.
    const auto window = [&](const std::array<uint32_t, 120>& ring, uint32_t count, uint32_t base, ui::Reading& p95,
                            ui::Reading& average, double* longest) {
        const uint32_t samples = std::min(count - std::min(base, count), static_cast<uint32_t>(ring.size()));
        if (samples < kWarmupSamples) {
            p95 = average = {State::warmup, 0};
        } else if (frame_stale) {
            p95 = average = {State::stale, 0};
        } else {
            std::array<uint32_t, 120> sorted;
            for (uint32_t i = 0; i < samples; ++i) {
                sorted[i] = ring[(count - 1 - i) % ring.size()];
            }
            std::sort(sorted.begin(), sorted.begin() + samples);
            uint64_t sum = 0;
            for (uint32_t i = 0; i < samples; ++i) {
                sum += sorted[i];
            }
            // The nearest-rank p95: the smallest sample with 95 % at or below it.
            const uint32_t rank = (samples * 95 + 99) / 100;
            p95 = valid(sorted[rank - 1] / 1000.0);
            average = valid(sum / 1000.0 / samples);
            if (longest) *longest = sorted[samples - 1] / 1000.0;
        }
        return samples;
    };
    s.frame_sample_count =
        valid(window(c.work, c.work_count, c.work_base, s.frame_work_p95_ms, s.frame_work_avg_ms, &frame_work_max_ms));
    window(c.gap, c.gap_count, c.gap_base, s.frame_gap_p95_ms, s.frame_gap_avg_ms, nullptr);

    // Windows that fill again after a resume or a speed change keep the
    // readings shown before. 2X's blank AUDIO readings are not held over.
    if (telemetry.has_shown) {
        if (telemetry.count < 2) {
            s.speed_percent = held.speed_percent;
            s.emu_fps = held.emu_fps;
            s.drw_fps = held.drw_fps;
            s.idle_core0_percent = held.idle_core0_percent;
            s.idle_core1_percent = held.idle_core1_percent;
            if (!c.fast) s.audio_min_ms = held.audio_min_ms;
        }
        if (s.frame_work_p95_ms.state == State::warmup) {
            s.frame_work_p95_ms = held.frame_work_p95_ms;
            s.frame_work_avg_ms = held.frame_work_avg_ms;
            s.frame_sample_count = held.frame_sample_count;
        }
        if (s.frame_gap_p95_ms.state == State::warmup) {
            s.frame_gap_p95_ms = held.frame_gap_p95_ms;
            s.frame_gap_avg_ms = held.frame_gap_avg_ms;
        }
    }
    telemetry.shown = s;
    telemetry.has_shown = true;
    return s;
}
#endif

}  // namespace

bool init() {
#if GBX_UI_LVGL
    if (!pack.open(platform::asset_pack())) {
        return false;
    }
    if (!platform::display::start()) {
        platform::log(platform::LogLevel::error, kTag, "no display");
        return false;
    }
    auto size = platform::display::size();
    if (size.width != pack.width() || size.height != pack.height()) {
        platform::log(platform::LogLevel::error, kTag, "asset pack is %dx%d, the screen is %dx%d", pack.width(),
                      pack.height(), size.width, size.height);
        return false;
    }
    if (!platform::display::lock(UINT32_MAX)) {
        platform::log(platform::LogLevel::error, kTag, "could not lock the display");
        return false;
    }
    // The menus' canvas goes over the game page.
    bool created = page.create(pack) && view.create(pack, pack.layout().display);
    if (!created) {
        view.destroy();
        page.destroy();
    }
    platform::display::unlock();
    if (!created) {
        return false;
    }
    if (!platform::touch::start()) {
        platform::log(platform::LogLevel::warn, kTag, "no touch controller, the controls cannot be pressed");
    }
    if (!platform::keys::start()) {
        platform::log(platform::LogLevel::warn, kTag, "the physical controls could not start");
    }
    if (!platform::files::mount()) {
        platform::log(platform::LogLevel::warn, kTag, "no card, no ROM can be loaded");
    }
    const Settings settings = read_settings();
    if (!saves.start(faults) || !library.start() ||
        !game.initialize(pack.layout().display, saves, faults, sounds)) {
        platform::log(platform::LogLevel::error, kTag, "could not start the app's tasks");
        return false;
    }
    library.scan();
    sounds.load(pack, Game::kAudioRate);
    if (!menus) {
        void* memory = platform::allocate_external(sizeof(Menus));
        menus = memory ? new (memory) Menus : nullptr;
    }
    if (!menus || !menus->begin({&game, &library, &saves, &faults, &view, &pack, &sounds}, settings)) {
        return false;
    }
    input = {};
    telemetry.next_us = 0;
    ready = true;
#else
    platform::log(platform::LogLevel::info, kTag, "this build has no UI");
#endif
    return true;
}

void update() {
#if GBX_UI_LVGL
    if (!ready) {
        return;
    }
    platform::audio::update();
    uint64_t now = platform::monotonic_us();
    // The lamps and the run state show at once. A new run state also takes
    // new readings, since it starts or ends them; the other lamps keep the
    // readings shown, as a sample between periods would cut the window short.
    ui::Status lamps = shown_status;
    fill_state(lamps);
    if (now < telemetry.next_us && lamps.run == shown_status.run) {
        if (lamps.save != shown_status.save || lamps.error != shown_status.error ||
            lamps.backend != shown_status.backend) {
            shown_status = lamps;
            page.show_status(lamps);
        }
    } else {
        telemetry.next_us = now + kStatusPeriodUs;
        const auto s = status(now);
        shown_status = s;
        page.show_status(s);
        // The status on the console when asked, see log_status(). These
        // replies log as warnings, which release builds still print.
        static uint64_t next_log = 0;
        const uint32_t period = status_period_ms.load(std::memory_order_relaxed);
        // A held console defers due lines to a later update.
        std::unique_lock output(status_output, std::try_to_lock);
        if (output.owns_lock() &&
            (status_due.exchange(false) || (period && s.run == ui::Status::Run::run && now >= next_log))) {
            next_log = now + period * 1000ull;
#ifdef GBX_PROFILER
            const auto profile = game.take_profile();
            platform::log(platform::LogLevel::warn, kTag,
                          "run max %.1f ms with %lu compiles %lu restores %lu rom pages %.1f ms waits %.1f ms; "
                          "jit %lu e %lu i %lu r %lu; "
                          "code %lu bytes; store %lu bytes %lu records %lu entries %lu drops; compiled %lu bytes; "
                          "interpreted %lu",
                          profile.run_us / 1000.0, static_cast<unsigned long>(profile.compiles),
                          static_cast<unsigned long>(profile.restores), static_cast<unsigned long>(profile.misses),
                          profile.miss_us / 1000.0, profile.wait_us / 1000.0, static_cast<unsigned long>(profile.compiled),
                          static_cast<unsigned long>(profile.evictions),
                          static_cast<unsigned long>(profile.invalidations),
                          static_cast<unsigned long>(profile.restored), static_cast<unsigned long>(profile.code_bytes),
                          static_cast<unsigned long>(profile.store_bytes),
                          static_cast<unsigned long>(profile.store_records),
                          static_cast<unsigned long>(profile.stored_entries),
                          static_cast<unsigned long>(profile.store_drops),
                          static_cast<unsigned long>(profile.compiled_bytes),
                          static_cast<unsigned long>(profile.interpreted));
            if (profile.cycles)
                platform::log(platform::LogLevel::warn, kTag,
                              "cpu cycles %lu M retired %lu M branches %lu k mispredicted %lu k",
                              static_cast<unsigned long>(profile.cycles), static_cast<unsigned long>(profile.retired),
                              static_cast<unsigned long>(profile.branches),
                              static_cast<unsigned long>(profile.mispredicts));
#if GBX_HAS_GBA
            // The GBA renderer's time a frame, and the CPU core's waits for
            // the worker's rows.
            static uint32_t frames_seen = 0, waits_seen = 0, catch_ups_seen = 0;
            static uint64_t wait_us_seen = 0;
            static uint32_t draw_seen = 0;
            const uint32_t frames = gba::render_counters.frames.load(std::memory_order_relaxed);
            const uint32_t draw_us = gba::render_counters.draw_us.read();
            const uint32_t waits = gb::offload_counters.waits[0];
            const uint64_t wait_us = gb::offload_counters.wait_us[0];
            const unsigned drawn = frames - frames_seen;
            const uint32_t catch_ups = gba::catch_ups;
            static std::array<uint32_t, 3> events_seen{};
            const auto events = gba::event_counts;
            static std::array<uint32_t, 16> gateway_seen{};
            const auto gateway = gba::arm7_gateway_counts;
            platform::log(platform::LogLevel::warn, kTag,
                          "render %u frames %.2f ms each; rows waits %lu %.1f ms; catch-ups %lu; "
                          "events ppu %lu timer %lu dma %lu; links %lu dispatches %lu of %lu accesses %lu stops %lu; dispatch cycles %lu",
                          drawn, drawn ? static_cast<uint32_t>(draw_us - draw_seen) / 1000.0 / drawn : 0.0,
                          static_cast<unsigned long>(waits - waits_seen), (wait_us - wait_us_seen) / 1000.0,
                          static_cast<unsigned long>(catch_ups - catch_ups_seen),
                          static_cast<unsigned long>(events[0] - events_seen[0]),
                          static_cast<unsigned long>(events[1] - events_seen[1]),
                          static_cast<unsigned long>(events[2] - events_seen[2]),
                          static_cast<unsigned long>(gateway[0] - gateway_seen[0]),
                          static_cast<unsigned long>(gateway[1] - gateway_seen[1]),
                          static_cast<unsigned long>(gateway[3] - gateway_seen[3]),
                          static_cast<unsigned long>(gateway[2] - gateway_seen[2]),
                          static_cast<unsigned long>(gateway[4] - gateway_seen[4]),
                          static_cast<unsigned long>(gateway[5] - gateway_seen[5]));
            platform::log(platform::LogLevel::warn, kTag, "loop turns %lu finds %lu enters %lu stops %lu; exits unlinked %lu budget %lu spent %lu stop %lu other %lu spent-in-pool %lu",
                          static_cast<unsigned long>(gateway[6] - gateway_seen[6]),
                          static_cast<unsigned long>(gateway[7] - gateway_seen[7]),
                          static_cast<unsigned long>(gateway[8] - gateway_seen[8]),
                          static_cast<unsigned long>(gateway[9] - gateway_seen[9]),
                          static_cast<unsigned long>(gateway[10] - gateway_seen[10]),
                          static_cast<unsigned long>(gateway[11] - gateway_seen[11]),
                          static_cast<unsigned long>(gateway[12] - gateway_seen[12]),
                          static_cast<unsigned long>(gateway[13] - gateway_seen[13]),
                          static_cast<unsigned long>(gateway[14] - gateway_seen[14]),
                          static_cast<unsigned long>(gateway[15] - gateway_seen[15]));
            {
                static auto sites = gba::arm7_exit_sites;
                sites = gba::arm7_exit_sites;
                gba::arm7_exit_sites = {};
                std::sort(sites.begin(), sites.end(), [](const auto& a, const auto& b) { return a.count > b.count; });
                for (unsigned i = 0; i < 8 && sites[i].count; ++i)
                    platform::log(platform::LogLevel::warn, kTag, "stop site %08lx state %08lx: %lu",
                                  static_cast<unsigned long>(sites[i].r15), static_cast<unsigned long>(sites[i].cpsr),
                                  static_cast<unsigned long>(sites[i].count));
            }
            {
                static std::array<uint32_t, 16> finds_seen{};
                const auto finds = gba::arm7_find_counts;
                char text[200];
                int used = 0;
                for (size_t i = 0; i < 16; ++i)
                    used += std::snprintf(text + used, sizeof text - used, " %lu",
                                          static_cast<unsigned long>(finds[i] - finds_seen[i]));
                platform::log(platform::LogLevel::warn, kTag, "finds:%s", text);
                finds_seen = finds;
            }
            {
                static std::array<uint64_t, 28> cycles_seen{};
                const auto cycles = gba::arm7_cycle_counts;
                const double run = static_cast<double>(cycles[0] - cycles_seen[0]);
                const auto share = [&](unsigned i) { return run ? 100.0 * (cycles[i] - cycles_seen[i]) / run : 0.0; };
                platform::log(platform::LogLevel::warn, kTag,
                              "run %.0f Mcycles: native %.1f%% (writes %.1f reads %.1f boundaries %.1f catch-ups %.1f "
                              "idle %.1f refills %.1f) halt %.1f%% interrupts %.1f%% lookups %.1f%%; ppu events %.1f%% "
                              "(begin %.1f submit %.1f schedule %.1f irq %.1f)",
                              run / 1e6, share(1), share(2), share(3), share(4), share(9), share(10), share(8), share(5),
                              share(6), share(7), share(11), share(12), share(13), share(14), share(15));
                cycles_seen = cycles;
            }
            gateway_seen = gateway;
            catch_ups_seen = catch_ups;
            events_seen = events;
            frames_seen = frames;
            draw_seen = draw_us;
            waits_seen = waits;
            wait_us_seen = wait_us;
#endif
            const auto slow = game.take_slow_access();
            const auto n = [](uint32_t count) { return static_cast<unsigned long>(count); };
            platform::log(platform::LogLevel::warn, kTag,
                          "slow r %lu %lu %lu %lu %lu %lu w %lu %lu %lu %lu %lu %lu horizon %lu; "
                          "io w %02lx %lu %02lx %lu %02lx %lu %02lx %lu %02lx %lu %02lx %lu",
                          n(slow.reads[0]), n(slow.reads[1]), n(slow.reads[2]), n(slow.reads[3]), n(slow.reads[4]),
                          n(slow.reads[5]), n(slow.writes[0]), n(slow.writes[1]), n(slow.writes[2]),
                          n(slow.writes[3]), n(slow.writes[4]), n(slow.writes[5]), n(slow.horizon_resets),
                          n(slow.io[0]), n(slow.io_writes[0]), n(slow.io[1]), n(slow.io_writes[1]), n(slow.io[2]),
                          n(slow.io_writes[2]), n(slow.io[3]), n(slow.io_writes[3]), n(slow.io[4]),
                          n(slow.io_writes[4]), n(slow.io[5]), n(slow.io_writes[5]));
#endif
            platform::log(platform::LogLevel::warn, kTag,
                          "spd %.1f x%d emu %.1f drw %.1f frm %.2f %.2f %.0f max %.1f gap %.2f %.2f buf %.0f %.0f "
                          "u %.0f %.0f mem %.0f %.2f idle %.0f %.0f t %llu %llu",
                          s.speed_percent.value, s.fast ? 2 : 1, s.emu_fps.value, s.drw_fps.value,
                          s.frame_work_p95_ms.value, s.frame_work_avg_ms.value, s.frame_sample_count.value,
                          frame_work_max_ms, s.frame_gap_p95_ms.value, s.frame_gap_avg_ms.value, s.audio_queue_ms.value,
                          s.audio_min_ms.value, s.audio_underruns_total.value, s.audio_underruns_window.value,
                          s.heap_internal_free_kib.value, s.heap_psram_free_mib.value, s.idle_core0_percent.value,
                          s.idle_core1_percent.value, static_cast<unsigned long long>(telemetry.counters.emulated),
                          static_cast<unsigned long long>(telemetry.counters.emulated_us));
        }
    }
    if (now < input.next_sample_us) {
        return;
    }
    // Never manufacture catch-up samples from a single touch snapshot.
    input.next_sample_us = now + kInputPeriodMs * 1000;

    std::array<platform::touch::Point, kMaxTouches> raw{};
    std::array<ui::TouchPoint, kMaxTouches> points{};
    int n = platform::touch::read(raw);
    for (int i = 0; i < n; ++i) {
        points[i] = {raw[i].x, raw[i].y};
    }
    auto current = std::span(input.hits).first(pack.controls().size());
    ui::hit_test(pack.controls(), std::span(points).first(std::max(n, 0)), current);
    input.filter.apply(current, n >= 0);
    const bool double_speed = input.double_speed;
    if (const int speed = console_speed.exchange(-1, std::memory_order_relaxed); speed >= 0) {
        input.double_speed = speed != 0;
    }
    for (size_t i = 0; i < current.size(); ++i) {
        // The speed switch flips when a finger lands on it.
        bool is_switch = pack.controls()[i].kind == ui::pack_format::ControlKind::speed_switch;
        if (is_switch && current[i].touched && !input.previous[i].touched) {
            input.double_speed = !input.double_speed;
        }
    }
    if (input.double_speed != double_speed) {
        platform::log(platform::LogLevel::info, kTag, "speed %s", input.double_speed ? "2x" : "1x");
        game.set_fast(input.double_speed);
    }
    MenuInput menu = window_touch(std::span(raw).first(std::max(n, 0)));
    menu.keys = control_keys(current) | platform::keys::read() | console_keys.load(std::memory_order_relaxed);
    if (const int slot = console_slot.exchange(0, std::memory_order_relaxed)) menus->load_slot_from_console(slot);
    game.set_buttons(menus->update(menu, now));
    page.show_controls(current, input.double_speed);

    bool changed = false;
    for (size_t i = 0; i < current.size(); ++i) {
        changed |= current[i].touched != input.previous[i].touched ||
                   current[i].direction != input.previous[i].direction;
    }
    if (changed) {
        char text[160];
        describe(current, text, sizeof text);
        platform::log(platform::LogLevel::info, kTag, "pressed: %s", text[0] ? text : "none");
    }
    input.previous = input.hits;
#endif
}

uint32_t update_delay_ms() {
#if GBX_UI_LVGL
    uint64_t now = platform::monotonic_us();
    if (ready && now < input.next_sample_us) {
        return static_cast<uint32_t>((input.next_sample_us - now + 999) / 1000);
    }
#endif
    return 0;
}

void shutdown() {
#if GBX_UI_LVGL
    if (ready) {
        // The game writes its save before the writer stops.
        game.stop();
        library.stop();
        saves.stop();
    }
    if (ready && platform::display::lock(UINT32_MAX)) {
        view.destroy();
        page.destroy();
        menus->end();
        platform::display::unlock();
        ready = false;
        input = {};
        pack = {};
    }
#endif
}

void start_game(const char* path, [[maybe_unused]] Backend backend, [[maybe_unused]] bool cgb) {
#if GBX_UI_LVGL
    if (ready) {
        game.start(path, backend, cgb);
        return;
    }
#endif
    platform::log(platform::LogLevel::error, kTag, "cannot run %s: this build has no UI", path);
}

void set_gba_options([[maybe_unused]] const char* bios, [[maybe_unused]] size_t rom_cache_bytes) {
#if GBX_UI_LVGL
    if (ready) game.set_gba_options(bios, rom_cache_bytes);
#endif
}

GameState game_state() {
#if GBX_UI_LVGL
    return game.state();
#else
    return GameState::no_rom;
#endif
}

#if GBX_UI_LVGL
void set_console_keys(uint16_t keys) {
    console_keys.store(keys, std::memory_order_relaxed);
}

void set_double_speed(bool on) {
    console_speed.store(on ? 1 : 0, std::memory_order_relaxed);
}

void load_slot(int slot) {
    console_slot.store(slot, std::memory_order_relaxed);
}

void log_status(uint32_t period_ms) {
    status_period_ms.store(period_ms, std::memory_order_relaxed);
    status_due.store(true);
}

bool capture_frame(uint16_t* out, uint32_t timeout_ms, int& width, int& height) {
    if (!ready || !game.capture(out, timeout_ms, width)) return false;
    height = width == 160 ? 144 : 160;
    return true;
}

bool capture_menu(std::span<uint16_t> out, int& width, int& height) {
    if (!ready || !platform::display::lock(1000)) return false;
    const bool copied = view.capture(out, width, height);
    platform::display::unlock();
    return copied;
}

void hold_status(bool held) {
    if (held) status_output.lock();
    else status_output.unlock();
}

#ifdef GBX_PROFILER
void record_game(const char* path, Backend backend, const char* file, bool cgb) {
    if (!ready) return;
    game.replay().request_record(file, cgb);
    game.start(path, backend, cgb);
}

void mark_recording() { game.replay().request_mark(); }
void bench_slot(uint32_t skip, uint32_t frames, bool absolute) { game.bench(skip, frames, absolute); }
void stop_recording() { game.replay().request_stop(); }

bool replay_game(const char* file, Backend backend) {
    static char path[platform::files::kMaxPath];  // the console task's stack is small
    bool cgb = false;
    if (!ready || !Replay::read_path(file, path, sizeof path, cgb)) return false;
    game.replay().request_play(file);
    game.start(path, backend, cgb);
    return true;
}
#endif
#else
void set_console_keys(uint16_t) {}
void set_double_speed(bool) {}
void load_slot(int) {}
void log_status(uint32_t) {}
bool capture_frame(uint16_t*, uint32_t, int&, int&) { return false; }
bool capture_menu(std::span<uint16_t>, int&, int&) { return false; }
void hold_status(bool) {}
#ifdef GBX_PROFILER
void record_game(const char*, Backend, const char*, bool) {}
void mark_recording() {}
void stop_recording() {}
void bench_slot(uint32_t, uint32_t, bool) {}
bool replay_game(const char*, Backend) { return false; }
#endif
#endif

void run() {
    if (!init()) {
        return;
    }
#if GBX_UI_LVGL
    for (;;) {
        update();
        platform::sleep_ms(update_delay_ms());
    }
#else
    shutdown();
#endif
}

}  // namespace gbx::app
