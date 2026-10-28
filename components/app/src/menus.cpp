#include "menus.h"

#if GBX_UI_LVGL
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <string_view>

#include "gbx/audio.h"
#include "gbx/display.h"
#include "gbx/executable.h"
#include "gbx/files.h"
#include "gbx/platform.h"
#include "gbx/systems.h"
#include "color_fix.h"
#include "rom_info.h"

namespace gbx::app {
namespace {

using Screen = ui::MenuScreen;
using platform::keys::kL;
using platform::keys::kMenu;
using platform::keys::kR;

constexpr const char* kTag = "menus";
constexpr uint16_t kRight = 1, kLeft = 2, kUp = 4, kDown = 8, kA = 16, kB = 32, kStart = 128;
constexpr uint16_t kRepeatable = kRight | kLeft | kUp | kDown | kL | kR;
// A held key repeats after this long, then at this rate.
constexpr uint64_t kRepeatDelayUs = 350'000;
constexpr uint64_t kRepeatUs = 90'000;
// A touch that moves this far, in logical pixels, scrolls instead of tapping.
constexpr float kDragPixels = 8;
// The ROM list follows a running scan at this rate at most; each listing
// sorts the folder again.
constexpr uint64_t kListingUs = 250'000;
constexpr int kRows = Screen::kRows;

enum SettingRow { kVolume, kBrightness, kCpu, kPalette, kColorFix, kScaling, kSettingRows };
// Whether each setting can change while a game runs. All of them can, but
// PALETTE and COLOR FIX depend on how the game runs, see setting_enabled();
// the gray rule stays for settings that will not.
constexpr bool kInGame[kSettingRows] = {true, true, true, true, true, true};
// The id of the PALETTE choice that puts cartridges in a GBC.
constexpr std::string_view kColorPalette = "color";

template <size_t N>
void put(char (&out)[N], std::string_view text) {
    std::snprintf(out, N, "%.*s", static_cast<int>(text.size()), text.data());
}

const char* file_name(const char* path) {
    const char* name = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') name = p + 1;
    }
    return name;
}

// A file name without its extension, for "Last played".
std::string_view stem(const char* path) {
    const std::string_view name(file_name(path));
    const size_t dot = name.rfind('.');
    return dot == std::string_view::npos || dot == 0 ? name : name.substr(0, dot);
}

// The board's time as "2026-10-19 14:32", in the clock's own time zone,
// from days since 1970 (Howard Hinnant, chrono-Compatible Low-Level Date
// Algorithms, civil_from_days).
void date(int64_t us, char (&out)[24]) {
    const int64_t seconds = us / 1'000'000;
    int64_t days = seconds / 86400;
    int64_t rest = seconds % 86400;
    if (rest < 0) {
        rest += 86400;
        --days;
    }
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t day = doy - (153 * mp + 2) / 5 + 1;
    const int64_t month = mp < 10 ? mp + 3 : mp - 9;
    const int64_t year = yoe + era * 400 + (month <= 2);
    // Bounded, so the text always fits.
    std::snprintf(out, sizeof out, "%04u-%02u-%02u %02u:%02u", static_cast<unsigned>(std::clamp<int64_t>(year, 0, 9999)),
                  static_cast<unsigned>(month) % 13, static_cast<unsigned>(day) % 32,
                  static_cast<unsigned>(rest / 3600) % 24, static_cast<unsigned>(rest / 60) % 60);
}

uint16_t bgr555(const uint8_t* rgb) {
    return static_cast<uint16_t>((rgb[0] >> 3) | (rgb[1] >> 3) << 5 | (rgb[2] >> 3) << 10);
}

// How long ago, from the monotonic clock, so it needs no board clock.
void age(uint64_t us, char (&out)[24]) {
    const uint64_t minutes = us / 60'000'000;
    if (minutes == 0) {
        put(out, "<1m");
    } else if (minutes < 60) {
        std::snprintf(out, sizeof out, "%um", static_cast<unsigned>(minutes));
    } else if (minutes < 24 * 60) {
        std::snprintf(out, sizeof out, "%uh", static_cast<unsigned>(minutes / 60));
    } else {
        std::snprintf(out, sizeof out, "%ud", static_cast<unsigned>(minutes / (24 * 60)));
    }
}

ui::MenuScreen* new_screen() {
    // Zeroed first, so the padding compares equal in show().
    void* memory = platform::allocate_external(sizeof(Screen));
    if (!memory) return nullptr;
    std::memset(memory, 0, sizeof(Screen));
    return new (memory) Screen{};
}

}  // namespace

bool Menus::begin(const Parts& parts, const Settings& settings) {
    parts_ = parts;
    settings_ = settings;
    listing_ = static_cast<int32_t*>(platform::allocate_external(sizeof(int32_t) * Library::kMaxEntries));
    frame_ = static_cast<uint16_t*>(platform::allocate_external(sizeof(uint16_t) * Game::kMaxFramePixels));
    screen_ = new_screen();
    shown_ = new_screen();
    blank_ = new_screen();
    void* info = platform::allocate_external(sizeof(Slots::Info));
    slot_info_ = info ? new (info) Slots::Info{} : nullptr;
    void* thumbnail = platform::allocate_external(sizeof(Slots::Thumbnail));
    thumbnail_ = thumbnail ? new (thumbnail) Slots::Thumbnail{} : nullptr;
    thumbnail_pixels_ = static_cast<uint16_t*>(platform::allocate_external(sizeof(uint16_t) * Game::kMaxFramePixels));
    if (!listing_ || !frame_ || !screen_ || !shown_ || !blank_ || !slot_info_ || !thumbnail_ || !thumbnail_pixels_) {
        platform::log(platform::LogLevel::error, kTag, "no memory for the menus");
        return false;
    }
    if (!platform::executable_available()) settings_.backend = Backend::interpreter;
    platform::audio::set_volume(settings_.volume);
    platform::display::set_brightness(settings_.brightness);
    platform::display::set_scaling(settings_.scaling);
    apply_palette();
    parts_.game->set_color_fix(settings_.color_fix);
    put(restore_, settings_.last_rom);
    page_ = Page::home;
    show();
    return true;
}

void Menus::end() {
    platform::free_external(listing_);
    platform::free_external(frame_);
    platform::free_external(screen_);
    platform::free_external(shown_);
    platform::free_external(blank_);
    platform::free_external(slot_info_);
    slot_info_ = nullptr;
    platform::free_external(thumbnail_);
    platform::free_external(thumbnail_pixels_);
    thumbnail_ = nullptr;
    thumbnail_pixels_ = nullptr;
    thumbnail_id_ = 0;
    listing_ = nullptr;
    frame_ = nullptr;
    screen_ = shown_ = blank_ = nullptr;
}

uint16_t Menus::update(const MenuInput& input, uint64_t now) {
    now_ = now;
    const uint16_t pressed = input.keys & ~held_;
    held_ = input.keys;
    follow_game();
    follow_slots();
    if (quitting_ || unloading_) follow_quit();
    if (page_ == Page::game) {
        blocked_ &= input.keys;
        if ((pressed & kMenu) && !pausing_ && parts_.game->state() == GameState::run) {
            parts_.game->pause();
            pausing_ = true;
            parts_.sounds->play(Sounds::Cue::confirm);
        }
        return pausing_ ? 0 : static_cast<uint16_t>(input.keys & ~blocked_ & ~platform::keys::kMenu);
    }
    if (const uint16_t keys = pressed | repeats(input.keys, pressed, now)) {
        intent_ |= keys;
        press(keys);
    }
    touch(input);
    if (page_ == Page::roms) refresh_listing(false);
    show();
    return 0;
}

// Follows the game's state: loading ends, questions come, a game stops.
void Menus::follow_game() {
    Game& game = *parts_.game;
    if (const uint32_t starts = game.starts(); starts != starts_seen_) {
        // The console or the PC's command line started a game.
        starts_seen_ = starts;
        answered_ = Game::Prompt::none;
        game.path(played_, sizeof played_);
        pausing_ = quitting_ = unloading_ = false;
        console_slot_ = 0;
        in_game_ = false;
        dialog_ = Dialog::none;
        slot_used_ = 0;
        slot_watching_ = false;
        slot_loading_ = false;
        if (page_ != Page::loading) back_ = page_ == Page::roms ? Page::roms : Page::home;
        page_ = Page::loading;
    }
    const GameState state = game.state();
    // Resume stays in the menus until the display has accepted the layer.
    // A failed attempt leaves the game paused, with its reason in ERRORS.
    if (in_game_ && state == GameState::run) {
        page_ = Page::game;
        in_game_ = false;
        dialog_ = Dialog::none;
        blocked_ = held_;
    }
    if (pausing_) {
        if (state == GameState::paused || state == GameState::error) {
            show_pause();
            if (console_slot_ && state == GameState::paused) load_slot(console_slot_, false);
            console_slot_ = 0;
        }
        return;
    }
    switch (page_) {
        case Page::game:
            if (state == GameState::error) show_pause();
            break;
        case Page::loading: {
            if (state == GameState::run) {
                page_ = Page::game;
                dialog_ = Dialog::none;
                // Keys held from the menu, such as the A that started the
                // game, do not reach it until released.
                blocked_ = held_;
                break;
            }
            if (state == GameState::error || state == GameState::no_rom) {
                page_ = back_;
                if (page_ == Page::roms) open_roms();
                dialog_ = state == GameState::error ? Dialog::cant_load : Dialog::none;
                dialog_button_ = 0;
                break;
            }
            const Game::Question q = game.question();
            if (q.prompt == Game::Prompt::none) answered_ = Game::Prompt::none;
            Dialog d = Dialog::none;
            if (q.prompt != answered_) {
                d = q.prompt == Game::Prompt::mismatch  ? Dialog::mismatch
                    : q.prompt == Game::Prompt::damaged ? Dialog::damaged
                    : q.prompt == Game::Prompt::clock   ? Dialog::clock
                                                        : Dialog::none;
            }
            if (d != dialog_) {
                dialog_ = d;
                dialog_button_ = 0;
                question_ = q;
            }
            break;
        }
        default: break;
    }
}

// QUIT: the save is written, then the game unloads and the ROM list shows
// with the cursor on the game.
void Menus::follow_quit() {
    Game& game = *parts_.game;
    if (unloading_) {
        if (game.state() != GameState::no_rom) return;
        unloading_ = false;
        in_game_ = false;
        dialog_ = Dialog::none;
        if (const int entry = parts_.library->find(played_); entry >= 0) {
            char path[kSavePathSize];
            bool saved = false;
            for (int slot = 0; slot < 2 && !saved; ++slot) {
                save_path(played_, slot, path, sizeof path);
                saved = platform::files::exists(path);
            }
            parts_.library->set_save(entry, saved);
        }
        put(restore_, played_);
        open_roms();
        return;
    }
    if (dialog_ == Dialog::save_failed || dialog_ == Dialog::slot_failed) return;
    // A slot being written goes on the card first, and the write the menus
    // asked for shows its result first, see follow_slots(). The game takes
    // a SAVE before anything else while paused, and each one ends, see
    // Slots::writes(), so this does not wait for ever.
    const bool writing = game.slots().work() == Slots::Work::writing;
    if (writing) {
        dialog_slot_ = game.slots().work_slot();
        dialog_ = Dialog::slot_saving;
    }
    if (writing || slot_watching_) return;
    if (dialog_ == Dialog::slot_saving) dialog_ = Dialog::none;
    if (!game.finished()) return;
    switch (parts_.saves->state()) {
        case Saves::State::dirty:
        case Saves::State::writing: dialog_ = Dialog::saving; break;
        case Saves::State::error:
            dialog_ = Dialog::save_failed;
            dialog_button_ = 0;
            break;
        case Saves::State::none:
        case Saves::State::clean:
            quitting_ = false;
            unloading_ = true;
            game.unload();
            break;
    }
}

// A write the menus started that fails while the game is paused shows a
// dialog; one that fails after the game resumed only adds its fault. A load
// ends in the game, or in a dialog first.
void Menus::follow_slots() {
    const Slots& slots = parts_.game->slots();
    if (slot_watching_ && slots.writes() != slot_writes_) {
        slot_watching_ = false;
        char error[48];
        slots.error(error, sizeof error);
        if (error[0] && page_ != Page::game && page_ != Page::loading) {
            dialog_slot_ = slot_watched_;
            dialog_ = Dialog::slot_failed;
            dialog_button_ = 0;
            // QUIT stops: the game goes on paused.
            quitting_ = false;
        }
    }
    if (!slot_loading_) return;
    const Game::SlotResult result = parts_.game->slot_result();
    switch (result.load) {
        case Game::SlotLoad::none:
        case Game::SlotLoad::loading: return;
        case Game::SlotLoad::loaded:
            slot_loading_ = false;
            // The loaded frame under the pause menu, until the game shows.
            show_paused_frame();
            resume();
            break;
        case Game::SlotLoad::clock:
            slot_loading_ = false;
            show_paused_frame();
            slot_clock_ = result.clock;
            dialog_ = Dialog::slot_clock;
            dialog_button_ = 0;
            break;
        case Game::SlotLoad::failed:
            slot_loading_ = false;
            slot_kind_ = result.kind;
            dialog_ = Dialog::slot_load_failed;
            dialog_button_ = 0;
            break;
    }
}

void Menus::show_pause() {
    Game& game = *parts_.game;
    pausing_ = false;
    in_game_ = true;
    dialog_ = Dialog::none;
    show_paused_frame();
    page_ = Page::pause;
    std::array<Item, 6> items;
    pause_items(items);
    pause_item_ = items[0];
    if (pause_item_ == Item::resume && game.state() != GameState::paused) pause_item_ = Item::quit;
    show();
    // The menu is drawn before the game layer goes, so the window never
    // shows an older menu.
    if (platform::display::lock(UINT32_MAX)) {
        parts_.view->draw_now();
        platform::display::unlock();
    }
    platform::display::hide_game();
}

void Menus::resume() {
    acted_ = true;
    dialog_ = Dialog::none;
    parts_.game->resume();
}

void Menus::open_roms() {
    page_ = Page::roms;
    parts_.library->scan();
    if (depth_ == 0) {
        stack_[0] = {Library::kRoot, 0, 0};
        depth_ = 1;
    }
    refresh_listing(true);
}

void Menus::open_settings(bool in_game) {
    in_game_ = in_game;
    page_ = Page::settings;
}

void Menus::open_errors(bool in_game) {
    in_game_ = in_game;
    errors_button_ = 0;
    page_ = Page::errors;
}

void Menus::open_slots() {
    page_ = Page::slots;
    slot_button_ = -1;
    slot_selected_ = slot_used_ ? slot_used_ - 1 : 0;
}

void Menus::save_slot(int slot) {
    acted_ = true;
    dialog_ = Dialog::none;
    slot_used_ = slot;
    slot_selected_ = slot - 1;
    slot_button_ = -1;
    // Nothing else writes while SAVE is offered, so the next write to end
    // is this one.
    slot_watching_ = true;
    slot_writes_ = parts_.game->slots().writes();
    slot_watched_ = slot;
    parts_.game->save_slot(slot);
}

void Menus::load_slot_from_console(int slot) {
    Game& game = *parts_.game;
    if (game.state() == GameState::paused && !pausing_ && !slot_loading_) {
        load_slot(slot, false);
    } else if (page_ == Page::game && game.state() == GameState::run && !pausing_) {
        game.pause();
        pausing_ = true;
        console_slot_ = slot;
    }
}

void Menus::load_slot(int slot, bool other_rom) {
    acted_ = true;
    dialog_ = Dialog::none;
    slot_used_ = slot;
    slot_button_ = -1;
    slot_loading_ = true;
    parts_.game->load_slot(slot, other_rom);
}

void Menus::leave_settings() {
    parts_.saves->flush_settings();
    if (in_game_) {
        // The palette or COLOR FIX may have changed under the paused frame.
        show_paused_frame();
    }
    page_ = in_game_ ? Page::pause : Page::home;
}

void Menus::start_rom(int entry) {
    char path[sizeof settings_.last_rom];
    parts_.library->path(entry, path, sizeof path);
    put(settings_.last_rom, path);
    save_settings();
    put(played_, path);
    back_ = Page::roms;
    dialog_ = Dialog::none;
    // PALETTE's COLOR puts the cartridge in a GBC, see gb::run_mode().
    parts_.game->start(path, settings_.backend, palette_index() == color_index());
    starts_seen_ = parts_.game->starts();
    slot_used_ = 0;
    slot_watching_ = false;
    slot_loading_ = false;
    answered_ = Game::Prompt::none;
    page_ = Page::loading;
}

uint16_t Menus::repeats(uint16_t held, uint16_t pressed, uint64_t now) {
    // The latest direction or page key pressed repeats while it is held.
    if (const uint16_t fresh = pressed & kRepeatable) {
        repeat_key_ = fresh & -fresh;
        repeat_at_ = now + kRepeatDelayUs;
        return 0;
    }
    if (!(held & repeat_key_) || now < repeat_at_) return 0;
    repeat_at_ = now + kRepeatUs;
    return repeat_key_;
}

void Menus::press(uint16_t keys) {
    if (dialog_ != Dialog::none) {
        press_dialog(keys);
        return;
    }
    switch (page_) {
        case Page::home: press_home(keys); break;
        case Page::roms: press_roms(keys); break;
        case Page::settings: press_settings(keys); break;
        case Page::errors: press_errors(keys); break;
        case Page::pause: press_pause(keys); break;
        case Page::slots: press_slots(keys); break;
        case Page::loading:
        case Page::game: break;
    }
}

void Menus::press_dialog(uint16_t keys) {
    Game& game = *parts_.game;
    const int buttons = dialog_ == Dialog::mismatch || dialog_ == Dialog::save_failed ||
                                dialog_ == Dialog::slot_overwrite || dialog_ == Dialog::slot_rom
                            ? 2
                        : dialog_ == Dialog::saving || dialog_ == Dialog::slot_saving ? 0
                                                                                      : 1;
    if ((keys & (kLeft | kRight)) && buttons == 2) dialog_button_ ^= 1;
    const bool yes = keys & (kA | kStart);
    const bool no = keys & kB;
    if (!yes && !no) return;
    switch (dialog_) {
        case Dialog::none:
        case Dialog::saving:
        case Dialog::slot_saving: break;
        case Dialog::cant_load:
        case Dialog::slot_failed:
        case Dialog::slot_load_failed: dialog_ = Dialog::none; break;
        case Dialog::slot_overwrite:
            if (yes && dialog_button_ == 0) save_slot(dialog_slot_);
            else dialog_ = Dialog::none;
            break;
        case Dialog::slot_rom:
            if (yes && dialog_button_ == 0) load_slot(dialog_slot_, slot_other_rom_);
            else dialog_ = Dialog::none;
            break;
        case Dialog::slot_clock:
            // The state is loaded; OK goes on with it.
            if (game.state() == GameState::paused) resume();
            else dialog_ = Dialog::none;
            break;
        case Dialog::mismatch:
            // B does not start the game.
            game.answer(no ? Game::Answer::cancel : dialog_button_ == 0 ? Game::Answer::yes : Game::Answer::no);
            answered_ = question_.prompt;
            dialog_ = Dialog::none;
            break;
        case Dialog::damaged:
        case Dialog::clock:
            // OK is the only button, so B is OK too.
            game.answer(Game::Answer::yes);
            answered_ = question_.prompt;
            dialog_ = Dialog::none;
            break;
        case Dialog::save_failed:
            if (no) {
                // Back to the pause menu, not quitting.
                quitting_ = false;
                dialog_ = Dialog::none;
            } else if (dialog_button_ == 0) {
                parts_.saves->retry();
                dialog_ = Dialog::saving;
            } else {
                quitting_ = false;
                unloading_ = true;
                dialog_ = Dialog::none;
                game.unload();
            }
            break;
    }
}

void Menus::press_home(uint16_t keys) {
    if (keys & kUp) home_selected_ = std::max(home_selected_ - 1, 0);
    if (keys & kDown) home_selected_ = std::min(home_selected_ + 1, 2);
    if (!(keys & (kA | kStart))) return;
    switch (home_selected_) {
        case 0: open_roms(); break;
        case 1: open_settings(false); break;
        case 2:
            if (parts_.faults->count()) open_errors(false);
            break;
        default: break;
    }
}

void Menus::press_roms(uint16_t keys) {
    Library& library = *parts_.library;
    if (keys & (kUp | kDown | kL | kR)) restore_[0] = '\0';
    if (keys & kUp) move(-1);
    if (keys & kDown) move(1);
    if (keys & kL) move(-kRows);
    if (keys & kR) move(kRows);
    if (keys & kB) {
        if (depth_ > 1) {
            --depth_;
            refresh_listing(true);
        } else {
            page_ = Page::home;
        }
        return;
    }
    if (!(keys & (kA | kStart))) return;
    if (library.state() == Library::State::no_card) {
        library.scan();
        acted_ = true;
        return;
    }
    Level& l = level();
    if (l.selected < 0 || l.selected >= listing_count_) return;
    const int index = listing_[l.selected];
    const Library::Entry entry = library.entry(index);
    if (entry.directory) {
        if (depth_ < static_cast<int>(stack_.size())) {
            stack_[depth_++] = {index, 0, 0};
            refresh_listing(true);
        }
    } else if (entry.info.problem == RomInfo::Problem::none) {
        start_rom(index);
    }
}

void Menus::press_settings(uint16_t keys) {
    if (keys & kUp) settings_selected_ = std::max(settings_selected_ - 1, 0);
    if (keys & kDown) settings_selected_ = std::min(settings_selected_ + 1, setting_rows() - 1);
    const int row = settings_selected_;
    if (setting_enabled(row)) {
        const int step = keys & kLeft ? -1 : keys & kRight ? 1 : 0;
        switch (row) {
            case kVolume: change(row, settings_.volume + step * 10); break;
            case kBrightness: change(row, settings_.brightness + step * 10); break;
            case kCpu: {
                const int value = settings_.backend == Backend::jit ? 1 : 0;
                change(row, keys & (kA | kStart) ? 1 - value : value + step);
                break;
            }
            case kScaling:
                if (step || (keys & (kA | kStart))) {
                    const int value = settings_.scaling == platform::display::Scaling::smooth ? 1 : 0;
                    change(row, keys & (kA | kStart) ? 1 - value : value + step);
                }
                break;
            case kPalette: {
                // Left and right step over a choice that cannot be taken;
                // A takes the next one there is.
                const int count = std::max(palette_count(), 1);
                int value = palette_index();
                const int by = keys & (kA | kStart) ? 1 : step;
                for (int tries = 0; by && tries < count; ++tries) {
                    value = keys & (kA | kStart) ? (value + 1) % count : value + by;
                    if (value < 0 || value >= count || !palette_off(value)) break;
                }
                change(row, value);
                break;
            }
            case kColorFix:
                if (step || (keys & (kA | kStart))) {
                    const int value = settings_.color_fix ? 1 : 0;
                    change(row, keys & (kA | kStart) ? 1 - value : value + step);
                }
                break;
        }
    }
    if (keys & kB) {
        leave_settings();
    } else if ((keys & kMenu) && in_game_) {
        leave_settings();
        if (parts_.game->state() == GameState::paused) resume();
    }
}

void Menus::press_errors(uint16_t keys) {
    if (keys & (kLeft | kRight)) errors_button_ ^= 1;
    const Page back = in_game_ ? Page::pause : Page::home;
    if (keys & (kA | kStart)) {
        // CLEAR turns the ERR lamp off.
        if (errors_button_ == 0) parts_.faults->clear();
        page_ = back;
    } else if (keys & kB) {
        page_ = back;
    } else if ((keys & kMenu) && in_game_ && parts_.game->state() == GameState::paused) {
        resume();
    }
}

void Menus::press_pause(uint16_t keys) {
    std::array<Item, 6> items;
    const int count = pause_items(items);
    int at = static_cast<int>(std::find(items.begin(), items.begin() + count, pause_item_) - items.begin());
    if (at == count) at = 0;
    if (keys & kUp) at = std::max(at - 1, 0);
    if (keys & kDown) at = std::min(at + 1, count - 1);
    pause_item_ = items[at];
    const bool paused = parts_.game->state() == GameState::paused;
    if (keys & (kA | kStart)) {
        switch (pause_item_) {
            case Item::retry: parts_.saves->retry(); break;
            case Item::resume:
                if (paused) resume();
                break;
            case Item::slots:
                if (paused && parts_.game->slots().active()) open_slots();
                break;
            case Item::settings: open_settings(true); break;
            case Item::errors:
                if (parts_.faults->count()) open_errors(true);
                break;
            case Item::quit:
                quitting_ = true;
                acted_ = true;
                parts_.game->finish();
                break;
        }
    } else if ((keys & (kB | kMenu)) && paused) {
        resume();
    }
}

void Menus::press_slots(uint16_t keys) {
    const Slots& slots = parts_.game->slots();
    const bool busy = slots.work() == Slots::Work::writing || slots.work() == Slots::Work::reading || slot_loading_;
    const int slot = slot_selected_ + 1;
    if (keys & kMenu) {
        if (parts_.game->state() == GameState::paused && !slot_loading_) resume();
        return;
    }
    if (slot_button_ < 0) {
        if (keys & kUp) slot_selected_ = std::max(slot_selected_ - 1, 0);
        if (keys & kDown) slot_selected_ = std::min(slot_selected_ + 1, Slots::kCount - 1);
        if (keys & kB) {
            page_ = Page::pause;
        } else if ((keys & (kA | kStart)) && !busy && slots.info(slot, *slot_info_)) {
            slot_button_ = 0;
        }
        return;
    }
    if (keys & (kLeft | kRight)) slot_button_ ^= 1;
    if (keys & kB) {
        slot_button_ = -1;
        return;
    }
    if (!(keys & (kA | kStart)) || busy || !slots.info(slot, *slot_info_)) return;
    const Slots::Info& info = *slot_info_;
    dialog_slot_ = slot;
    dialog_button_ = 1;  // CANCEL: a second A does not replace or load
    if (slot_button_ == 0) {
        if (info.kind == Slots::Kind::empty) save_slot(slot);
        else dialog_ = Dialog::slot_overwrite;
        return;
    }
    if (info.kind != Slots::Kind::ok) return;
    slot_other_rom_ = info.other_rom;
    if (info.other_rom) dialog_ = Dialog::slot_rom;
    else load_slot(slot, false);
}

void Menus::touch(const MenuInput& input) {
    ui::MenuView& view = *parts_.view;
    if (!input.touched) {
        // A tap acts when it ends where it began.
        if (touching_ && !dragging_) {
            const ui::MenuHit hit = view.hit(*screen_, touch_x_, touch_y_);
            if (hit.target != ui::MenuHit::Target::none && hit.target == touch_hit_.target &&
                hit.index == touch_hit_.index) {
                tap(hit);
            }
        }
        touching_ = dragging_ = false;
        return;
    }
    touch_x_ = input.x;
    touch_y_ = input.y;
    if (!touching_) {
        touching_ = true;
        dragging_ = false;
        touch_y0_ = input.y;
        touch_hit_ = view.hit(*screen_, input.x, input.y);
        drag_first_ = page_ == Page::roms && depth_ ? level().first : 0;
    } else if (page_ == Page::roms && dialog_ == Dialog::none && depth_ &&
               (dragging_ || std::fabs(input.y - touch_y0_) > kDragPixels)) {
        // The list follows the finger, a row at a time.
        dragging_ = true;
        restore_[0] = '\0';
        const float row = static_cast<float>(ui::kMenuRowHeight * parts_.pack->menu().scale);
        const int rows = static_cast<int>((touch_y0_ - input.y) / row);
        level().first = std::clamp(drag_first_ + rows, 0, std::max(0, listing_count_ - kRows));
        return;
    }
    // A level takes the segment under the finger as it moves.
    if (dialog_ == Dialog::none && page_ == Page::settings && touch_hit_.target == ui::MenuHit::Target::segment) {
        const ui::MenuHit hit = view.hit(*screen_, input.x, input.y);
        if (hit.target == ui::MenuHit::Target::segment && hit.index == touch_hit_.index) {
            settings_selected_ = hit.index;
            intent_ |= kLeft;
            if (setting_enabled(hit.index)) change(hit.index, hit.value);
        }
    }
}

// A tap on a row or button only moves the cursor to it; a tap on the row or
// button the cursor is on acts as A, so a stray touch starts nothing.
void Menus::tap(const ui::MenuHit& hit) {
    using Target = ui::MenuHit::Target;
    // A move unless it acts, below.
    intent_ |= kUp;
    if (dialog_ != Dialog::none) {
        if (hit.target != Target::button) return;
        if (hit.index != dialog_button_) {
            dialog_button_ = hit.index;
        } else {
            intent_ |= kA;
            press_dialog(kA);
        }
        return;
    }
    switch (page_) {
        case Page::home:
            if (hit.target != Target::row) break;
            if (hit.index != home_selected_) {
                home_selected_ = hit.index;
            } else {
                intent_ |= kA;
                press_home(kA);
            }
            break;
        case Page::roms: {
            if (hit.target != Target::row || !depth_) break;
            Level& l = level();
            const int index = l.first + hit.index;
            restore_[0] = '\0';
            if (index != l.selected) {
                l.selected = index;
            } else {
                intent_ |= kA;
                press_roms(kA);
            }
            break;
        }
        case Page::settings:
            if (hit.target == Target::row) {
                settings_selected_ = hit.index;
            } else if (hit.target == Target::choice) {
                intent_ |= kA;
                settings_selected_ = hit.index;
                if (setting_enabled(hit.index)) change(hit.index, hit.value);
            }
            break;
        case Page::errors:
            if (hit.target != Target::button) break;
            if (hit.index != errors_button_) {
                errors_button_ = hit.index;
            } else {
                intent_ |= kA;
                press_errors(kA);
            }
            break;
        case Page::slots:
            if (hit.target == Target::row && hit.index < Slots::kCount) {
                if (hit.index != slot_selected_ || slot_button_ >= 0) {
                    slot_selected_ = hit.index;
                    slot_button_ = -1;
                } else {
                    intent_ |= kA;
                    press_slots(kA);
                }
            } else if (hit.target == Target::button) {
                if (slot_button_ < 0) {
                    // The first tap selects it, as A on the slot and then a move would.
                    if (parts_.game->slots().info(slot_selected_ + 1, *slot_info_)) slot_button_ = hit.index;
                } else if (hit.index != slot_button_) {
                    slot_button_ = hit.index;
                } else {
                    intent_ |= kA;
                    press_slots(kA);
                }
            }
            break;
        case Page::pause: {
            if (hit.target != Target::row) break;
            std::array<Item, 6> items;
            const int count = pause_items(items);
            if (hit.index >= count) break;
            if (items[hit.index] != pause_item_) {
                pause_item_ = items[hit.index];
            } else {
                intent_ |= kA;
                press_pause(kA);
            }
            break;
        }
        case Page::loading:
        case Page::game: break;
    }
}

int Menus::setting_rows() const {
    // SCALING samples a frame scaled up. A window that holds no system's
    // frame at twice its size shows them one pixel a screen pixel, and the
    // row is left out; it is the last one.
    const auto window = parts_.pack->layout().display;
    const auto fits = [&](unsigned width, unsigned height) {
        return window[2] >= static_cast<int>(2 * width) && window[3] >= static_cast<int>(2 * height);
    };
    const bool scaled = (GBX_HAS_GB && fits(160, 144)) || (GBX_HAS_GBA && fits(gba::kScreenWidth, gba::kScreenHeight));
    return scaled ? kSettingRows : kScaling;
}

bool Menus::setting_enabled(int row) const {
    const bool supported = row == kBrightness ? platform::display::brightness_supported()
                           : row == kCpu      ? platform::executable_available()
                           : row == kScaling  ? platform::display::scaling_supported(platform::display::Scaling::smooth)
                                              : true;
    // A game does not move between a GB and a GBC: one in color keeps its
    // colors, and one in GB mode has no COLOR FIX. A GBA game has neither:
    // its colors are its own, and COLOR FIX is the GBC screen's. A build of
    // the GBA alone has neither at all.
    if ((row == kPalette || row == kColorFix) && (!GBX_HAS_GB || (in_game_ && parts_.game->gba()))) return false;
    if (in_game_ && row == kPalette && in_color_game()) return false;
    if (in_game_ && row == kColorFix && !in_color_game()) return false;
    return supported && (!in_game_ || kInGame[row]);
}

bool Menus::in_color_game() const {
    return parts_.game->mode() != gb::Mode::dmg && !parts_.game->gba();
}

bool Menus::palette_off(int choice) const {
    return in_game_ && !in_color_game() && !parts_.game->gba() && choice == color_index();
}

void Menus::show_paused_frame() {
    const int width = parts_.game->paused_pixels({frame_, Game::kMaxFramePixels}, true);
    const int height = width == 160 ? 144 : 160;
    parts_.view->set_frame({frame_, static_cast<size_t>(width * height)}, width, height);
}

void Menus::change(int row, int value) {
    switch (row) {
        case kVolume:
            value = std::clamp(value, 0, 100);
            if (value == settings_.volume) return;
            settings_.volume = value;
            platform::audio::set_volume(value);
            break;
        case kBrightness:
            value = std::clamp(value, 10, 100);
            if (value == settings_.brightness) return;
            settings_.brightness = value;
            platform::display::set_brightness(value);
            break;
        case kCpu: {
            const Backend backend = std::clamp(value, 0, 1) ? Backend::jit : Backend::interpreter;
            if (backend == settings_.backend) return;
            settings_.backend = backend;
            if (in_game_) parts_.game->set_backend(backend);
            break;
        }
        case kScaling: {
            const auto scaling = std::clamp(value, 0, 1) ? platform::display::Scaling::smooth
                                                        : platform::display::Scaling::nearest;
            if (settings_.scaling_explicit && scaling == settings_.scaling) return;
            settings_.scaling = scaling;
            settings_.scaling_explicit = true;
            platform::display::set_scaling(scaling);
            break;
        }
        case kPalette: {
            const auto palettes = parts_.pack->menu_palettes();
            const int count = palette_count();
            if (count == 0) return;
            value = std::clamp(value, 0, count - 1);
            if (value == palette_index() || palette_off(value)) return;
            put(settings_.palette, ui::field(palettes[value].id));
            apply_palette();
            break;
        }
        case kColorFix: {
            const bool on = std::clamp(value, 0, 1) == 1;
            if (on == settings_.color_fix) return;
            settings_.color_fix = on;
            parts_.game->set_color_fix(on);
            break;
        }
        default: return;
    }
    save_settings();
}

int Menus::palette_index() const {
    const auto palettes = parts_.pack->menu_palettes();
    for (int i = 0; i < palette_count(); ++i) {
        if (ui::field(palettes[i].id) == settings_.palette) return i;
    }
    return 0;
}

int Menus::palette_count() const {
    return std::min<int>(static_cast<int>(parts_.pack->menu_palettes().size()), 3);
}

int Menus::color_index() const {
    const auto palettes = parts_.pack->menu_palettes();
    for (int i = 0; i < palette_count(); ++i) {
        if (ui::field(palettes[i].id) == kColorPalette) return i;
    }
    return -1;
}

// The colors of GB mode's shades: the palette chosen, or the first while
// COLOR is, for the thumbnails of slots saved in GB mode.
void Menus::apply_palette() {
    const auto palettes = parts_.pack->menu_palettes();
    if (palettes.empty()) return;
    const int index = palette_index();
    const auto& p = palettes[index == color_index() ? 0 : index];
    parts_.game->set_palette({bgr555(p.shades[0]), bgr555(p.shades[1]), bgr555(p.shades[2]), bgr555(p.shades[3])});
}

void Menus::save_settings() {
    char text[1024];
    if (const size_t n = format_settings(settings_, text, sizeof text)) parts_.saves->write_settings({text, n});
}

void Menus::refresh_listing(bool force) {
    Library& library = *parts_.library;
    if (!depth_) return;
    const uint32_t generation = library.generation();
    Level& l = level();
    if (!force && (l.folder == listing_folder_ && generation == listing_generation_)) return;
    if (!force && now_ < listing_us_ + kListingUs) return;
    // The cursor stays on the same entry as the listing grows.
    const int keep = l.folder == listing_folder_ && l.selected >= 0 && l.selected < listing_count_
                         ? listing_[l.selected]
                         : -1;
    listing_count_ = library.list(l.folder, {listing_, static_cast<size_t>(Library::kMaxEntries)});
    listing_folder_ = l.folder;
    listing_generation_ = generation;
    listing_us_ = now_;
    if (keep >= 0) {
        const int32_t* at = std::find(listing_, listing_ + listing_count_, keep);
        if (at != listing_ + listing_count_) l.selected = static_cast<int>(at - listing_);
    }
    l.selected = std::clamp(l.selected, 0, std::max(listing_count_ - 1, 0));
    if (restore_[0]) restore();
    if (!dragging_) scroll_to_selection();
}

// Opens the folders down to restore_ and puts the cursor on it, once the
// scan has found it.
void Menus::restore() {
    Library& library = *parts_.library;
    const int target = library.find(restore_);
    if (target < 0) {
        const auto state = library.state();
        if (state == Library::State::done || state == Library::State::no_card) restore_[0] = '\0';
        return;
    }
    restore_[0] = '\0';
    std::array<int, 32> chain;
    int n = 0;
    for (int f = library.entry(target).parent; f != Library::kRoot && n < static_cast<int>(chain.size()) - 1;
         f = library.entry(f).parent) {
        chain[n++] = f;
    }
    depth_ = 0;
    stack_[depth_++] = {Library::kRoot, 0, 0};
    while (n > 0) stack_[depth_++] = {chain[--n], 0, 0};
    for (int i = 0; i < depth_; ++i) {
        const int wanted = i + 1 < depth_ ? stack_[i + 1].folder : target;
        listing_count_ = library.list(stack_[i].folder, {listing_, static_cast<size_t>(Library::kMaxEntries)});
        const int at = static_cast<int>(std::find(listing_, listing_ + listing_count_, wanted) - listing_);
        Level& l = stack_[i];
        l.selected = at < listing_count_ ? at : 0;
        // The cursor near the middle of the page.
        l.first = std::clamp(l.selected - kRows / 2, 0, std::max(0, listing_count_ - kRows));
    }
    listing_folder_ = level().folder;
}

void Menus::move(int delta) {
    Level& l = level();
    if (!listing_count_) return;
    const int old = l.selected;
    l.selected = std::clamp(l.selected + delta, 0, listing_count_ - 1);
    // A page key moves the page with the cursor.
    if (std::abs(delta) == kRows && l.selected - old == delta) {
        l.first = std::clamp(l.first + delta, 0, std::max(0, listing_count_ - kRows));
    }
    scroll_to_selection();
}

void Menus::scroll_to_selection() {
    Level& l = level();
    if (l.selected < l.first) l.first = l.selected;
    if (l.selected >= l.first + kRows) l.first = l.selected - kRows + 1;
    l.first = std::clamp(l.first, 0, std::max(0, listing_count_ - kRows));
}

int Menus::pause_items(std::array<Item, 6>& out) const {
    int n = 0;
    if (parts_.saves->state() == Saves::State::error) out[n++] = Item::retry;
    for (Item item : {Item::resume, Item::slots, Item::settings, Item::errors, Item::quit}) out[n++] = item;
    return n;
}

void Menus::show() {
    build(*screen_);
    const bool same = std::memcmp(screen_, shown_, sizeof(Screen)) == 0;
    parts_.sounds->play(cue(!same || acted_));
    intent_ = 0;
    acted_ = false;
    if (same) return;
    std::memcpy(static_cast<void*>(shown_), screen_, sizeof(Screen));
    parts_.view->show(*screen_);
}

// The cue for what this update's input did: A that changes nothing is an
// action that cannot happen, as on a gray row; other keys that change
// nothing, as at the end of a list, are silent. A dialog that asks
// something has its own cue, whatever opened it.
Sounds::Cue Menus::cue(bool changed) const {
    using Cue = Sounds::Cue;
    const auto& dialog = screen_->dialog;
    if (dialog.shown && dialog.button_count &&
        (!shown_->dialog.shown || std::strcmp(dialog.title, shown_->dialog.title) != 0)) {
        return Cue::dialog;
    }
    if (intent_ & (kA | kStart)) return changed ? Cue::confirm : Cue::denied;
    if (!changed) return Cue::none;
    if (intent_ & (kB | kMenu)) return Cue::back;
    if (intent_ & kRepeatable) return Cue::move;
    return Cue::none;
}

void Menus::build(Screen& s) {
    std::memcpy(static_cast<void*>(&s), blank_, sizeof(Screen));
    switch (page_) {
        case Page::home: build_home(s); break;
        case Page::roms: build_roms(s); break;
        case Page::settings: build_settings(s); break;
        case Page::errors: build_errors(s); break;
        case Page::pause:
        case Page::game: build_pause(s); break;
        case Page::slots: build_slots(s); break;
        case Page::loading: {
            char path[platform::files::kMaxPath];
            parts_.game->path(path, sizeof path);
            s.kind = Screen::Kind::loading;
            put(s.heading, "LOADING");
            put(s.name, file_name(path));
            s.progress = parts_.game->load_progress();
            break;
        }
    }
    if (dialog_ != Dialog::none) build_dialog(s);
}

void Menus::build_home(Screen& s) {
    Library& library = *parts_.library;
    const auto state = library.state();
    const uint32_t roms = library.roms();
    const int faults = parts_.faults->count();
    s.kind = Screen::Kind::home;
    put(s.title, "HOME");
    if (state == Library::State::no_card) {
        put(s.status, "NO SD CARD");
    } else if (state == Library::State::done) {
        std::snprintf(s.status, sizeof s.status, "SD %u ROMS", static_cast<unsigned>(roms));
    } else {
        put(s.status, "SCANNING");
    }
    struct Card {
        const char* icon;
        const char* label;
        bool enabled;
    };
    const Card cards[] = {{"cartridge", "ROMS", true}, {"gear", "SETTINGS", true}, {"alert", "ERRORS", faults > 0}};
    s.row_count = 3;
    for (int i = 0; i < 3; ++i) {
        put(s.rows[i].icon, cards[i].icon);
        put(s.rows[i].label, cards[i].label);
        s.rows[i].enabled = cards[i].enabled;
    }
    if (roms) std::snprintf(s.rows[0].detail, sizeof s.rows[0].detail, "%u", static_cast<unsigned>(roms));
    if (faults) {
        std::snprintf(s.rows[2].detail, sizeof s.rows[2].detail, "%d", faults);
    } else {
        put(s.rows[2].detail, "NONE");
    }
    s.selected = home_selected_;
    switch (home_selected_) {
        case 0:
            if (settings_.last_rom[0]) {
                const std::string_view name = stem(settings_.last_rom);
                std::snprintf(s.footer, sizeof s.footer, "Last played: %.*s", static_cast<int>(name.size()),
                              name.data());
            } else {
                put(s.footer, "Games in the roms folder of the card.");
            }
            break;
        case 1: put(s.footer, "Sound, display and emulation settings."); break;
        default:
            if (faults) {
                Faults::Fault newest[1];
                parts_.faults->copy(newest);
                std::snprintf(s.footer, sizeof s.footer, "Newest: %.150s", newest[0].text);
            } else {
                put(s.footer, "No errors to show.");
                s.footer_alert = true;
            }
            break;
    }
}

void Menus::build_roms(Screen& s) {
    Library& library = *parts_.library;
    const auto state = library.state();
    s.kind = Screen::Kind::roms;
    put(s.title, "ROMS");
    if (state == Library::State::no_card) {
        s.kind = Screen::Kind::message;
        put(s.icon, "card");
        put(s.heading, "NO SD CARD");
        put(s.body, "Insert a card with your games in a roms folder.");
        return;
    }
    if (!depth_ || (listing_count_ == 0 && depth_ == 1)) {
        s.kind = Screen::Kind::message;
        put(s.icon, "cartridge");
        if (state == Library::State::done) {
            put(s.heading, "NO ROMS");
            put(s.body, !GBX_HAS_GBA  ? "Put .gb or .gbc files in the roms folder on the card."
                        : !GBX_HAS_GB ? "Put .gba files in the roms folder on the card."
                                      : "Put .gb, .gbc or .gba files in the roms folder on the card.");
        } else {
            put(s.heading, "SCANNING");
            put(s.body, "Looking for ROMs on the card.");
        }
        return;
    }
    const Level& l = stack_[depth_ - 1];
    char path[platform::files::kMaxPath];
    library.path(l.folder, path, sizeof path);
    put(s.path, path[4] ? path + 4 : "/");
    if (state != Library::State::done) {
        put(s.status, "SCANNING");
    } else if (library.full()) {
        put(s.status, "FULL");
    } else {
        std::snprintf(s.status, sizeof s.status, "%d/%d", l.selected + 1, listing_count_);
    }
    s.total = listing_count_;
    s.first = l.first;
    s.row_count = std::clamp(listing_count_ - l.first, 0, kRows);
    s.selected = l.selected >= l.first && l.selected < l.first + kRows ? l.selected - l.first : -1;
    for (int row = 0; row < s.row_count; ++row) {
        const int index = listing_[l.first + row];
        const Library::Entry e = library.entry(index);
        auto& r = s.rows[row];
        library.name(index, r.label, sizeof r.label);
        if (e.directory) {
            r.badge = Screen::Badge::folder;
            std::snprintf(r.detail, sizeof r.detail, "%u", static_cast<unsigned>(e.roms));
        } else {
            using Machine = RomInfo::Machine;
            r.badge = e.info.machine == Machine::gba                                         ? Screen::Badge::gba
                      : e.info.machine == Machine::gbc || e.info.machine == Machine::gbc_only ? Screen::Badge::gbc
                                                                                               : Screen::Badge::gb;
            r.enabled = e.info.problem == RomInfo::Problem::none;
            if (e.save) put(r.detail, "SAV");
        }
    }
    if (l.selected < 0 || l.selected >= listing_count_) return;
    const int index = listing_[l.selected];
    const Library::Entry e = library.entry(index);
    if (e.directory) {
        char name[256];
        library.name(index, name, sizeof name);
        std::snprintf(s.footer, sizeof s.footer, "%.120s/  %u ROM%s", name, static_cast<unsigned>(e.roms),
                      e.roms == 1 ? "" : "s");
    } else if (e.info.problem == RomInfo::Problem::none) {
        describe_rom(e.info, s.footer, sizeof s.footer);
    } else {
        describe_problem(e.info, s.footer, sizeof s.footer);
        s.footer_alert = true;
    }
}

void Menus::build_settings(Screen& s) {
    s.kind = Screen::Kind::settings;
    put(s.title, "SETTINGS");
    if (in_game_) put(s.status, "IN GAME");
    s.row_count = setting_rows();
    auto& volume = s.rows[kVolume];
    put(volume.label, "VOLUME");
    volume.setting = Screen::Setting::level;
    volume.value = static_cast<uint8_t>(settings_.volume);
    auto& brightness = s.rows[kBrightness];
    put(brightness.label, "BRIGHTNESS");
    brightness.setting = Screen::Setting::level;
    brightness.value = static_cast<uint8_t>(settings_.brightness);
    brightness.unsupported = !platform::display::brightness_supported();
    auto& cpu = s.rows[kCpu];
    put(cpu.label, "CPU");
    cpu.setting = Screen::Setting::choice;
    cpu.choice_count = 2;
    put(cpu.choices[0], "INT");
    put(cpu.choices[1], "JIT");
    cpu.value = settings_.backend == Backend::jit ? 1 : 0;
    auto& palette = s.rows[kPalette];
    put(palette.label, "PALETTE");
    palette.setting = Screen::Setting::choice;
    const auto palettes = parts_.pack->menu_palettes();
    palette.choice_count = static_cast<uint8_t>(palette_count());
    for (int k = 0; k < palette.choice_count; ++k) {
        const std::string_view id = ui::field(palettes[k].id);
        for (size_t i = 0; i < id.size() && i + 1 < sizeof palette.choices[k]; ++i) {
            const char c = id[i];
            palette.choices[k][i] = c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        }
        palette.swatches[k] = static_cast<int8_t>(k);
        if (palette_off(k)) palette.off |= static_cast<uint8_t>(1 << k);
    }
    palette.value = static_cast<uint8_t>(palette_index());
    auto& fix = s.rows[kColorFix];
    put(fix.label, "COLOR FIX");
    fix.setting = Screen::Setting::choice;
    fix.choice_count = 2;
    put(fix.choices[0], "OFF");
    put(fix.choices[1], "ON");
    fix.value = settings_.color_fix ? 1 : 0;
    auto& scaling = s.rows[kScaling];
    put(scaling.label, "SCALING");
    scaling.setting = Screen::Setting::choice;
    scaling.choice_count = 2;
    put(scaling.choices[0], "NEAREST");
    put(scaling.choices[1], "SMOOTH");
    scaling.value = settings_.scaling == platform::display::Scaling::smooth ? 1 : 0;
    for (int i = 0; i < kSettingRows; ++i) s.rows[i].enabled = setting_enabled(i);
    s.selected = settings_selected_;
    const bool on = setting_enabled(settings_selected_);
    switch (settings_selected_) {
        case kVolume: put(s.footer, "Speaker and headphone volume."); break;
        case kBrightness: put(s.footer, on ? "Screen backlight." : "No backlight control on this platform."); break;
        case kCpu: put(s.footer, on ? "INT interprets, JIT compiles." : "This platform cannot run the JIT."); break;
        case kScaling:
            put(s.footer, !platform::display::scaling_supported(platform::display::Scaling::smooth)
                              ? "This platform uses NEAREST only."
                              : in_game_ ? "Sampling changes on resume." : "Game image sampling.");
            break;
        case kPalette:
            put(s.footer, !GBX_HAS_GB              ? "GBA games have their own colors."
                          : !in_game_              ? "GB colors, or COLOR to play on a GBC."
                          : parts_.game->gba()     ? "Set by the game: GBA games have their own colors."
                          : in_color_game()        ? "Set by the game: it runs on a GBC."
                                                   : "Pick COLOR before a game starts.");
            break;
        case kColorFix:
            put(s.footer, on                   ? "Colors as a GBC screen shows them."
                          : !GBX_HAS_GB        ? "GBA games have their own colors."
                          : parts_.game->gba() ? "Only for GBC games: GBA colors stay as they are."
                                               : "Only for games in color.");
            break;
    }
    s.footer_alert = !on;
}

void Menus::build_errors(Screen& s) {
    s.kind = Screen::Kind::errors;
    put(s.title, "ERRORS");
    if (in_game_) put(s.status, "IN GAME");
    std::array<Faults::Fault, Faults::kMax> faults;
    const int n = parts_.faults->copy(faults);
    s.row_count = std::min(n, Screen::kMaxItems);
    for (int i = 0; i < s.row_count; ++i) {
        put(s.rows[i].label, faults[i].text);
        age(now_ - std::min(now_, faults[i].us), s.rows[i].detail);
    }
    s.selected = errors_button_;
}

void Menus::build_pause(Screen& s) {
    Game& game = *parts_.game;
    s.kind = Screen::Kind::pause;
    put(s.title, "PAUSED");
    std::array<Item, 6> items;
    const int count = pause_items(items);
    const int faults = parts_.faults->count();
    s.row_count = count;
    s.selected = 0;
    for (int i = 0; i < count; ++i) {
        auto& r = s.rows[i];
        switch (items[i]) {
            case Item::retry: put(r.label, "RETRY SAVE"); break;
            case Item::resume:
                put(r.label, "RESUME");
                r.enabled = game.state() == GameState::paused;
                break;
            case Item::slots:
                put(r.label, "SAVE / LOAD");
                r.enabled = game.state() == GameState::paused && game.slots().active();
                // A write that goes on after the game resumed.
                if (game.slots().work() == Slots::Work::writing) {
                    std::snprintf(r.detail, sizeof r.detail, "%d%%", game.slots().progress());
                    r.detail_ink = Screen::Ink::warning;
                }
                break;
            case Item::settings: put(r.label, "SETTINGS"); break;
            case Item::errors:
                put(r.label, "ERRORS");
                r.enabled = faults > 0;
                if (faults) std::snprintf(r.detail, sizeof r.detail, "%d", faults);
                r.detail_ink = Screen::Ink::alert;
                break;
            case Item::quit: put(r.label, "QUIT"); break;
        }
        if (items[i] == pause_item_) s.selected = i;
    }
    switch (parts_.saves->state()) {
        case Saves::State::none: s.save = Screen::Save::none; break;
        case Saves::State::clean: s.save = Screen::Save::clean; break;
        case Saves::State::dirty: s.save = Screen::Save::dirty; break;
        case Saves::State::writing: s.save = Screen::Save::writing; break;
        case Saves::State::error: s.save = Screen::Save::error; break;
    }
}

void Menus::build_slots(Screen& s) {
    Game& game = *parts_.game;
    const Slots& slots = game.slots();
    if (parts_.library->state() == Library::State::no_card) {
        s.kind = Screen::Kind::message;
        put(s.title, "SAVE / LOAD");
        put(s.icon, "card");
        put(s.heading, "NO SD CARD");
        put(s.body, "Insert the card to save to or load from a slot.");
        return;
    }
    s.kind = Screen::Kind::slots;
    put(s.title, "SAVE / LOAD");
    char path[platform::files::kMaxPath];
    game.path(path, sizeof path);
    put(s.path, stem(path));
    const Slots::Work work = slots.work();
    const bool writing = work == Slots::Work::writing;
    const bool busy = writing || work == Slots::Work::reading || slot_loading_;
    const int busy_slot = busy ? (slot_loading_ && work != Slots::Work::writing ? slot_used_ : slots.work_slot()) : 0;
    s.row_count = Slots::kCount;
    s.selected = slot_selected_;
    s.slots.button = slot_button_;
    s.slots.busy = busy;
    s.slots.wide = game.gba();
    for (int i = 0; i < Slots::kCount; ++i) {
        auto& r = s.rows[i];
        std::snprintf(r.label, sizeof r.label, "SLOT %d", i + 1);
        Slots::Info& info = *slot_info_;
        const bool listed = slots.info(i + 1, info);
        if (busy && busy_slot == i + 1) {
            r.progress = static_cast<int8_t>(std::clamp(slots.progress(), 0, 100));
        } else if (!listed) {
            put(r.detail, "READING");
        } else {
            switch (info.kind) {
                case Slots::Kind::ok:
                    if (info.other_rom) {
                        put(r.detail, "OTHER ROM");
                        r.detail_ink = Screen::Ink::warning;
                    } else if (info.wall.trusted) {
                        date(platform::local_time(info.wall.us), r.detail);
                    } else {
                        put(r.detail, "TIME UNKNOWN");
                    }
                    break;
                case Slots::Kind::empty:
                    put(r.detail, "EMPTY");
                    r.detail_ink = Screen::Ink::disabled;
                    break;
                case Slots::Kind::damaged: put(r.detail, "DAMAGED"); break;
                case Slots::Kind::version: put(r.detail, "OTHER VERSION"); break;
                case Slots::Kind::system: put(r.detail, "OTHER SYSTEM"); break;
                case Slots::Kind::incompatible: put(r.detail, "OTHER ROM"); break;
                case Slots::Kind::mode: put(r.detail, "OTHER MODE"); break;
                case Slots::Kind::unreadable: put(r.detail, "CAN'T READ"); break;
            }
            if (info.kind != Slots::Kind::ok && info.kind != Slots::Kind::empty) r.detail_ink = Screen::Ink::alert;
        }
    }
    // The selected slot last, so slot_info_ holds it below.
    const bool listed = slots.info(slot_selected_ + 1, *slot_info_);
    const Slots::Info& info = *slot_info_;
    const bool ok = listed && info.kind == Slots::Kind::ok;
    s.slots.enabled[0] = !busy && listed;
    s.slots.enabled[1] = !busy && ok;
    // A slot of another mode cannot load, but shows its frame.
    if (ok || (listed && info.kind == Slots::Kind::mode)) {
        send_thumbnail(slot_selected_ + 1, slots.generation());
        s.slots.thumbnail = true;
        s.slots.thumbnail_id = thumbnail_id_;
    }
    slot_note(slot_selected_ + 1, info, listed, s);
    if (busy && writing) {
        std::snprintf(s.footer, sizeof s.footer, "Writing SLOT %d; the game can resume.", busy_slot);
        s.footer_alert = s.footer_warning = false;
    } else if (busy) {
        std::snprintf(s.footer, sizeof s.footer, "Loading SLOT %d.", busy_slot);
        s.footer_alert = s.footer_warning = false;
    }
}

void Menus::send_thumbnail(int slot, uint32_t generation) {
    const ThumbnailKey key{slot, generation, parts_.game->palette(), parts_.game->color_fix()};
    if (key == thumbnail_key_ && thumbnail_id_) return;
    if (!parts_.game->slots().thumbnail(slot, *thumbnail_)) return;
    thumbnail_key_ = key;
    const Slots::Thumbnail& t = *thumbnail_;
#if GBX_HAS_GBA
    if (t.gba) {
        // A GBA's colors as the game set them, at full size.
        std::copy_n(t.colors.begin(), gba::kFramePixels, thumbnail_pixels_);
        parts_.view->set_thumbnail({thumbnail_pixels_, gba::kFramePixels}, gba::kScreenWidth, gba::kScreenHeight);
        ++thumbnail_id_;
        return;
    }
#endif
    if (t.mode == gb::Mode::dmg) {
        // GB mode's shades in the current palette.
        for (size_t i = 0; i < gb::kFramePixels; ++i)
            thumbnail_pixels_[i] = key.palette[t.shades[i / 4] >> (i % 4 * 2) & 3];
    } else {
        // The colors as the game set them, shown as COLOR FIX shows them.
        for (size_t i = 0; i < gb::kFramePixels; ++i)
            thumbnail_pixels_[i] = key.color_fix ? color_fix(t.colors[i]) : t.colors[i];
    }
    parts_.view->set_thumbnail({thumbnail_pixels_, gb::kFramePixels}, 160, 144);
    ++thumbnail_id_;
}

// The footer for the selected slot and the cursor's button.
void Menus::slot_note(int slot, const Slots::Info& info, bool listed, Screen& s) const {
    if (!listed) {
        put(s.footer, "Reading the slots on the card.");
        return;
    }
    if (slot_button_ == 0) {
        std::snprintf(s.footer, sizeof s.footer, info.kind == Slots::Kind::empty ? "Save the game now in SLOT %d."
                                                                                : "Save the game now over SLOT %d.",
                      slot);
        return;
    }
    const char* reason = nullptr;
    switch (info.kind) {
        case Slots::Kind::ok: break;
        case Slots::Kind::empty:
            std::snprintf(s.footer, sizeof s.footer, "Nothing is saved in SLOT %d.", slot);
            s.footer_alert = slot_button_ == 1;
            return;
        case Slots::Kind::damaged: reason = "Can't load: it fails its check."; break;
        case Slots::Kind::version: reason = "Can't load: saved by another version."; break;
        case Slots::Kind::system: reason = "Can't load: saved by another system."; break;
        case Slots::Kind::incompatible: reason = "Can't load: its ROM has another save size."; break;
        // PALETTE chose the mode when the game started.
        case Slots::Kind::mode:
            reason = info.mode == gb::Mode::dmg ? "Can't load: PALETTE was not COLOR." : "Can't load: PALETTE was COLOR.";
            break;
        case Slots::Kind::unreadable: reason = "Can't load: the SD card reported an error."; break;
    }
    if (reason) {
        put(s.footer, reason);
        s.footer_alert = true;
        return;
    }
    if (slot_button_ == 1) {
        std::snprintf(s.footer, sizeof s.footer, "Go back to the game saved in SLOT %d.", slot);
    } else if (info.other_rom) {
        std::snprintf(s.footer, sizeof s.footer, "From another ROM: %s.", info.title);
        s.footer_warning = true;
    } else if (!info.wall.trusted) {
        put(s.footer, "Saved with the board's clock not set.");
    } else {
        put(s.footer, info.title);
    }
}

void Menus::build_dialog(Screen& s) {
    auto& d = s.dialog;
    d.shown = true;
    d.selected = dialog_button_;
    auto text = [&d](const char* t) { put(d.lines[d.line_count++].text, t); };
    auto pair = [&d](const char* label, const char* t) {
        put(d.lines[d.line_count].label, label);
        put(d.lines[d.line_count++].text, t);
    };
    char line[160];
    auto buttons = [&d](const char* first, const char* second = nullptr) {
        put(d.buttons[0], first);
        d.button_count = 1;
        if (second) {
            put(d.buttons[1], second);
            d.button_count = 2;
        }
    };
    switch (dialog_) {
        case Dialog::none: break;
        case Dialog::cant_load: {
            char path[platform::files::kMaxPath];
            parts_.game->path(path, sizeof path);
            d.tone = Screen::Tone::alert;
            put(d.title, "CAN'T LOAD");
            text(file_name(path));
            text(parts_.game->error());
            buttons("OK");
            break;
        }
        case Dialog::mismatch:
            put(d.title, "SAVE FROM ANOTHER ROM");
            pair("SAVE", question_.save_title);
            pair("ROM", question_.rom_title);
            text("START NEW keeps the old save as .sav.old.");
            buttons("LOAD SAVE", "START NEW");
            break;
        case Dialog::damaged:
            d.tone = Screen::Tone::alert;
            put(d.title, "SAVE DAMAGED");
            text("The save fails its check, so the game starts with empty save memory.");
            text("A copy of the old save is kept as .sav.old.");
            buttons("OK");
            break;
        case Dialog::clock:
            put(d.title, "CLOCK NOT SET");
            switch (question_.clock) {
                case Game::Clock::untrusted:
                    text("The board's clock is not set, so the game's clock did not move on while the power was "
                         "off.");
                    break;
                case Game::Clock::behind:
                    text("The board's clock is behind the time of the save, so the game's clock did not move on "
                         "while the power was off.");
                    break;
                case Game::Clock::untrusted_save:
                    text("The board's clock was not set when the game saved, so the game's clock did not move on "
                         "while the power was off.");
                    break;
            }
            buttons("OK");
            break;
        case Dialog::saving:
            put(d.title, "SAVING");
            text("Writing the save before quitting.");
            d.progress = parts_.saves->progress();
            break;
        case Dialog::save_failed:
            std::snprintf(line, sizeof line, "The save could not be written: %s", parts_.saves->error_text());
            d.tone = Screen::Tone::alert;
            put(d.title, "SAVE FAILED");
            text(line);
            buttons("RETRY", "QUIT WITHOUT SAVING");
            break;
        case Dialog::slot_overwrite:
            std::snprintf(d.title, sizeof d.title, "REPLACE SLOT %d", dialog_slot_);
            std::snprintf(line, sizeof line, "SLOT %d already holds a save.", dialog_slot_);
            text(line);
            buttons("REPLACE", "CANCEL");
            break;
        case Dialog::slot_rom: {
            char title[17];
            parts_.game->slots().rom_title(title);
            put(d.title, "SLOT FROM ANOTHER ROM");
            if (parts_.game->slots().info(dialog_slot_, *slot_info_)) pair("SLOT", slot_info_->title);
            pair("ROM", title);
            text("The game may not run right.");
            buttons("LOAD", "CANCEL");
            break;
        }
        case Dialog::slot_clock:
            put(d.title, "CLOCK NOT SET");
            switch (slot_clock_) {
                case Game::Clock::untrusted:
                    std::snprintf(line, sizeof line,
                                  "The board's clock is not set, so the game's clock did not move on since SLOT %d "
                                  "was saved.",
                                  slot_used_);
                    break;
                case Game::Clock::behind:
                    std::snprintf(line, sizeof line,
                                  "The board's clock is behind the time SLOT %d was saved, so the game's clock did "
                                  "not move on.",
                                  slot_used_);
                    break;
                case Game::Clock::untrusted_save:
                    std::snprintf(line, sizeof line,
                                  "The board's clock was not set when SLOT %d was saved, so the game's clock did not "
                                  "move on.",
                                  slot_used_);
                    break;
            }
            text(line);
            buttons("OK");
            break;
        case Dialog::slot_failed: {
            char error[48];
            parts_.game->slots().error(error, sizeof error);
            d.tone = Screen::Tone::alert;
            std::snprintf(d.title, sizeof d.title, "SLOT %d NOT SAVED", dialog_slot_);
            std::snprintf(line, sizeof line, "SLOT %d could not be written: %s", dialog_slot_,
                          error[0] ? error : "the SD card reported an error.");
            text(line);
            buttons("OK");
            break;
        }
        case Dialog::slot_load_failed: {
            const char* why = "fails its check.";
            switch (slot_kind_) {
                case Slots::Kind::ok:
                case Slots::Kind::damaged: break;
                case Slots::Kind::empty: why = "is empty."; break;
                case Slots::Kind::version: why = "was saved by another version."; break;
                case Slots::Kind::system: why = "was saved by another system."; break;
                case Slots::Kind::incompatible: why = "is from a ROM with another save size."; break;
                case Slots::Kind::mode: why = "was saved with another PALETTE: GB or color."; break;
                case Slots::Kind::unreadable: why = "cannot be read: the SD card reported an error."; break;
            }
            d.tone = Screen::Tone::alert;
            std::snprintf(d.title, sizeof d.title, "SLOT %d NOT LOADED", slot_used_);
            std::snprintf(line, sizeof line, "SLOT %d %s", slot_used_, why);
            text(line);
            buttons("OK");
            break;
        }
        case Dialog::slot_saving:
            put(d.title, "SAVING");
            std::snprintf(line, sizeof line, "Writing SLOT %d before quitting.", dialog_slot_);
            text(line);
            d.progress = parts_.game->slots().progress();
            break;
    }
}

}  // namespace gbx::app
#endif
