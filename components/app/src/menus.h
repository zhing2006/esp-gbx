#pragma once

#include <array>
#include <cstdint>

#include "faults.h"
#include "game.h"
#include "gbx/ui/menu.h"
#include "gbx/ui/pack.h"
#include "library.h"
#include "saves.h"
#include "settings.h"
#include "sounds.h"

#if GBX_UI_LVGL
namespace gbx::app {

// Input for the menus, sampled every 10 ms.
struct MenuInput {
    uint16_t keys = 0;  // GB bits and keys::kL, kR and kMenu
    bool touched = false;
    float x = 0, y = 0;  // the first touch, logical screen pixels
};

// The menus' state and flow: HOME, ROMS, SETTINGS, ERRORS, the pause menu,
// SAVE / LOAD, loading and the dialogs. They live in the app, which also owns the game,
// the saves and the settings they act on; ui::MenuView only draws the
// MenuScreen descriptions they build and maps touches back to rows. All
// calls come from the app's task.
class Menus {
public:
    struct Parts {
        Game* game;
        Library* library;
        Saves* saves;
        Faults* faults;
        ui::MenuView* view;
        const ui::Pack* pack;
        Sounds* sounds;
    };

    // Applies the settings and shows HOME.
    bool begin(const Parts& parts, const Settings& settings);
    void end();
    // Takes a sample of input and moves the menus on. Returns the keys the
    // game gets, GB bits with L and R: none while a menu shows. A game started elsewhere, as
    // by the console or the PC's command line, shows LOADING and runs.
    uint16_t update(const MenuInput& input, uint64_t now);
    // The console's slot command: pauses a running game, then loads `slot`
    // as the SLOTS page does, which runs on once it is loaded.
    void load_slot_from_console(int slot);

private:
    enum class Page : uint8_t { home, roms, settings, errors, pause, slots, loading, game };
    enum class Dialog : uint8_t {
        none, cant_load, mismatch, damaged, clock, saving, save_failed,
        slot_overwrite, slot_rom, slot_clock, slot_failed, slot_load_failed, slot_saving
    };
    enum class Item : uint8_t { retry, resume, slots, settings, errors, quit };  // pause menu
    struct Level {
        int folder;
        int selected;  // in the folder's listing
        int first;     // the listing index on the first row
    };

    // Flow.
    void follow_game();
    void follow_quit();
    void follow_slots();
    void show_pause();
    void resume();
    void open_roms();
    void open_settings(bool in_game);
    void open_errors(bool in_game);
    void open_slots();
    void save_slot(int slot);
    void load_slot(int slot, bool other_rom);
    void leave_settings();
    void start_rom(int entry);

    // Input.
    uint16_t repeats(uint16_t held, uint16_t pressed, uint64_t now);
    void press(uint16_t keys);
    void press_dialog(uint16_t keys);
    void press_home(uint16_t keys);
    void press_roms(uint16_t keys);
    void press_settings(uint16_t keys);
    void press_errors(uint16_t keys);
    void press_pause(uint16_t keys);
    void press_slots(uint16_t keys);
    void touch(const MenuInput& input);
    void tap(const ui::MenuHit& hit);

    // Settings.
    void change(int row, int value);
    void apply_palette();
    void save_settings();
    int palette_index() const;
    int palette_count() const;
    // The PALETTE choice that puts cartridges in a GBC, or -1.
    int color_index() const;
    // In a game: how it runs. A game in GB mode cannot choose COLOR, and
    // one in color has no PALETTE and takes COLOR FIX.
    bool in_color_game() const;
    bool palette_off(int choice) const;
    // The rows SETTINGS shows: all, or all but SCALING.
    int setting_rows() const;
    bool setting_enabled(int row) const;
    // The paused frame for the pause menu, as COLOR FIX shows it.
    void show_paused_frame();

    // ROMS.
    Level& level() { return stack_[depth_ - 1]; }
    void refresh_listing(bool force);
    void restore();
    void move(int delta);
    void scroll_to_selection();

    // Building the screens.
    void build(ui::MenuScreen& s);
    void build_home(ui::MenuScreen& s);
    void build_roms(ui::MenuScreen& s);
    void build_settings(ui::MenuScreen& s);
    void build_errors(ui::MenuScreen& s);
    void build_pause(ui::MenuScreen& s);
    void build_slots(ui::MenuScreen& s);
    void slot_note(int slot, const Slots::Info& info, bool listed, ui::MenuScreen& s) const;
    // Gives the view the selected slot's thumbnail when it changed; false
    // while the view has none of it.
    bool send_thumbnail(int slot, uint32_t generation);
    void build_dialog(ui::MenuScreen& s);
    int pause_items(std::array<Item, 6>& out) const;
    void show();
    Sounds::Cue cue(bool changed) const;

    Parts parts_{};
    Settings settings_;
    Page page_ = Page::home;
    Page back_ = Page::home;  // where SETTINGS, ERRORS and CAN'T LOAD return to
    Dialog dialog_ = Dialog::none;
    int dialog_button_ = 0;
    int home_selected_ = 0;
    int settings_selected_ = 0;
    int errors_button_ = 0;
    Item pause_item_ = Item::resume;
    bool in_game_ = false;  // SETTINGS and ERRORS opened from the pause menu

    // SAVE / LOAD: the slot the cursor is on, 0 to 3, and its button, 0 SAVE,
    // 1 LOAD or -1 while on the list. dialog_slot_ is the slot a dialog is
    // about, 1 to 4.
    int slot_selected_ = 0;
    int slot_button_ = -1;
    int slot_used_ = 0;  // saved or loaded last in this game, 1 to 4
    int dialog_slot_ = 0;
    bool slot_other_rom_ = false;  // the load a dialog asks about
    // A write the menus asked for, followed so that a failure while paused
    // shows: it has ended once Slots::writes() is no longer slot_writes_.
    bool slot_watching_ = false;
    uint32_t slot_writes_ = 0;
    int slot_watched_ = 0;
    bool slot_loading_ = false;  // waiting for Game::slot_result()
    Game::Clock slot_clock_ = Game::Clock::untrusted;
    Slots::Kind slot_kind_ = Slots::Kind::empty;  // why a load failed
    Slots::Info* slot_info_ = nullptr;  // external memory, for building the screen
    // The thumbnail the view has: the slot's frame as the screen shows it,
    // what it shows, and its number, see MenuScreen::Slots::thumbnail_id.
    Slots::Thumbnail* thumbnail_ = nullptr;  // external memory
    uint16_t* thumbnail_pixels_ = nullptr;   // 160x144 BGR555, external memory
    struct ThumbnailKey {
        int slot = 0;
        uint32_t generation = 0;
        std::array<uint16_t, 4> palette{};
        bool color_fix = false;
        bool operator==(const ThumbnailKey&) const = default;
    } thumbnail_key_;
    uint32_t thumbnail_id_ = 0;

    // ROMS: the folders opened, the root first, and the listing of the last.
    std::array<Level, 32> stack_{};
    int depth_ = 0;
    int32_t* listing_ = nullptr;  // Library::kMaxEntries
    int listing_count_ = 0;
    int listing_folder_ = Library::kRoot - 1;
    uint32_t listing_generation_ = 0;
    uint64_t listing_us_ = 0;
    char restore_[platform::files::kMaxPath] = {};  // a ROM to put the cursor on once the scan finds it

    // The game.
    Game::Question question_;  // the one the dialog shows
    Game::Prompt answered_ = Game::Prompt::none;  // until the game takes the answer
    uint32_t starts_seen_ = 0;  // Game::starts() the menus know of
    bool pausing_ = false;   // waiting for the game to pause
    int console_slot_ = 0;   // to load once paused, see load_slot_from_console()
    bool quitting_ = false;  // waiting for the save before unloading
    bool unloading_ = false;
    char played_[platform::files::kMaxPath] = {};  // the ROM that ran, for the cursor after QUIT

    // Input.
    uint16_t held_ = 0;
    // What the input of this update asked for, for its sound cue: the keys
    // pressed, A for a tap that acts and a direction for one that moves the
    // cursor or sets a value. acted_ marks an action the screen does not
    // show at once, such as RESUME.
    uint16_t intent_ = 0;
    bool acted_ = false;
    uint16_t blocked_ = 0;  // keys the game ignores until they are released
    uint16_t repeat_key_ = 0;
    uint64_t repeat_at_ = 0;
    uint64_t now_ = 0;
    bool touching_ = false;
    bool dragging_ = false;
    float touch_x_ = 0, touch_y_ = 0, touch_y0_ = 0;
    ui::MenuHit touch_hit_;
    int drag_first_ = 0;

    // The screen built last and the one the view has; external memory.
    ui::MenuScreen* screen_ = nullptr;
    ui::MenuScreen* shown_ = nullptr;
    ui::MenuScreen* blank_ = nullptr;  // a default screen with zero padding
    uint16_t* frame_ = nullptr;        // the paused frame, 160x144 BGR555
};

}  // namespace gbx::app
#endif
