#pragma once

#include <cstdint>
#include <mutex>
#include <span>

#include "gbx/ui/pack.h"

#if GBX_UI_LVGL
#include "lvgl.h"
#endif

// The menus in the game window: the home screen, the ROM list, SETTINGS, the
// pause menu, SAVE / LOAD, ERRORS, the loading screen and the dialogs over
// them, drawn as in
// tools/ui/menu_kit.py and docs/ui-layouts/index.md#菜单.
//
// The app owns the menu state and navigation and describes each screen with
// a MenuScreen; the UI only draws descriptions and maps touches on them back
// to rows and buttons. So the flow, settings and save handling stay in one
// place that runs without LVGL, and the drawing needs no knowledge of them.
namespace gbx::ui {

// Text fields are UTF-8 and NUL terminated; longer text is cut when drawn.
struct MenuScreen {
    enum class Kind : uint8_t { home, roms, message, settings, pause, errors, loading, slots };
    enum class Badge : uint8_t { none, folder, gb, gbc, gba };
    enum class Setting : uint8_t { level, choice };
    enum class Save : uint8_t { none, clean, dirty, writing, error };
    enum class Tone : uint8_t { normal, alert };
    // Colors of a slot's second line and of the pause menu's details.
    enum class Ink : uint8_t { muted, disabled, alert, warning };
    static constexpr int kRows = 7;      // ROM rows on a page
    static constexpr int kMaxItems = 7;  // rows of the other screens

    // ERRORS: one per error, newest first; label is the reason and detail
    // how long ago. `selected` is the button, 0 CLEAR or 1 BACK.
    struct Row {
        char label[256] = {};
        char detail[24] = {};       // home: right of the card; roms: the count or SAV;
                                    // pause: the number of errors on ERRORS, or the
                                    // progress of a slot's write; slots: the second line
        Ink detail_ink = Ink::muted;  // pause and slots
        int8_t progress = -1;       // slots: a write under way, 0 to 100, for the second line
        char icon[12] = {};         // home, message: an icon of the menu theme
        Badge badge = Badge::none;  // roms
        bool enabled = true;
        // SETTINGS
        Setting setting = Setting::level;
        uint8_t value = 0;      // level: 0 to 100 in steps of 10; choice: its index
        bool unsupported = false;  // the platform cannot do it: N/A, no lit segments
        uint8_t choice_count = 0;
        char choices[3][8] = {};
        int8_t swatches[3] = {-1, -1, -1};  // a menu palette shown in each choice, or -1
        // Bit k: choice k is gray in an enabled row and cannot be chosen.
        uint8_t off = 0;
    };

    struct Dialog {
        struct Line {
            char label[12] = {};  // set: a label and text in two columns, one line
            char text[160] = {};  // otherwise wrapped
        };
        bool shown = false;
        Tone tone = Tone::normal;
        char title[32] = {};
        Line lines[4] = {};
        int line_count = 0;
        char buttons[2][24] = {};
        int button_count = 0;
        int selected = 0;
        int progress = -1;  // 0 to 100 shows a bar
    };

    Kind kind = Kind::home;
    char title[16] = {};    // title bar, left
    char path[256] = {};    // ROMS: the folder, after the title
    char status[24] = {};   // title bar, right
    char footer[160] = {};
    bool footer_alert = false;
    bool footer_warning = false;  // amber, as for a slot of another ROM

    Row rows[kMaxItems] = {};
    int row_count = 0;   // home cards, ROM rows on this page, settings, pause items
    int selected = -1;   // index in rows
    int first = 0;       // roms: index of rows[0] in the folder
    int total = 0;       // roms: entries in the folder

    // pause: the save state
    Save save = Save::none;

    // slots: rows are the four slots, `selected` the one the cursor is on.
    struct Slots {
        int button = -1;  // the cursor's button, 0 SAVE or 1 LOAD; -1 while on the list
        bool enabled[2] = {true, true};
        bool busy = false;       // a write under way: the selection bar is gray
        // The selected slot has a frame: the one MenuView::set_thumbnail()
        // gave last, whose number this is, so that a new one changes the
        // screen.
        bool thumbnail = false;
        uint32_t thumbnail_id = 0;
        // A GBA game's: its 240x160 thumbnail shows one pixel a screen
        // pixel at 2x, beside a narrower list.
        bool wide = false;
    } slots;

    // message: an icon, a heading and wrapped text; loading: the heading
    char icon[12] = {};
    char heading[32] = {};
    char body[160] = {};

    // loading
    char name[256] = {};
    int progress = 0;

    Dialog dialog;
};

// Where a touch lands on a screen, in grid pixels.
struct MenuHit {
    enum class Target : uint8_t { none, row, button, segment, choice };
    Target target = Target::none;
    int index = 0;  // row or button
    int value = 0;  // segment: the level 10 to 100; choice: its index
};

// The fixed sizes the app needs to page and scroll, in grid pixels.
inline constexpr int kMenuRowHeight = 18;

class MenuFont;

#if GBX_UI_LVGL
// The menus' canvas over the game window. The app shows it while no game
// frame is in the window: the platform's game layer covers it otherwise.
class MenuView {
public:
    MenuView();
    ~MenuView();
    MenuView(const MenuView&) = delete;
    MenuView& operator=(const MenuView&) = delete;

    // Builds the canvas over `window`. Hold the LVGL lock. The pack must stay
    // alive until destroy(), including on failure.
    bool create(const Pack& pack, std::span<const int16_t, 4> window);
    // Hold the LVGL lock.
    void destroy();

    // Sets what the canvas shows. Thread safe; the view draws it in the LVGL
    // task, redrawing only what changed.
    void show(const MenuScreen& screen);
    // The game frame under the pause menu: a GB's 160x144 or a GBA's
    // 240x160 BGR555 pixels. Thread safe.
    void set_frame(std::span<const uint16_t> pixels, int width, int height);
    // The SAVE / LOAD thumbnail, as the screen shows its pixels: a GB's
    // 160x144 or a GBA's 240x160 BGR555. Thread safe.
    void set_thumbnail(std::span<const uint16_t> pixels, int width, int height);
    // Draws the newest description now, so that the next LVGL refresh shows
    // it; then the game layer can be hidden without showing an older menu.
    // Hold the LVGL lock.
    void draw_now();

    // A touch at logical screen pixels (x, y) on `screen`, or none outside
    // the window. Thread safe: it reads only the pack.
    MenuHit hit(const MenuScreen& screen, float x, float y) const;

    // Copies the grid pixels the canvas shows, RGB565, into `out`. Sets
    // width and height also when `out` is too small, which returns false, as
    // nothing drawn yet does. Hold the LVGL lock.
    bool capture(std::span<uint16_t> out, int& width, int& height) const;

private:
    struct State;
    void apply();

    State* state_ = nullptr;
    lv_obj_t* canvas_ = nullptr;
    lv_draw_buf_t* buffer_ = nullptr;
    lv_timer_t* timer_ = nullptr;
};
#endif

}  // namespace gbx::ui
