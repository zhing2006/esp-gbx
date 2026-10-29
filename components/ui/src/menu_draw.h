#pragma once

#include <array>
#include <climits>
#include <cstdint>
#include <span>
#include <string_view>

#include "gbx/ui/menu.h"
#include "gbx/ui/pack.h"
#include "menu_font.h"

// Draws MenuScreen descriptions on the menus' grid, as tools/ui/menu_kit.py
// does: same building blocks, sizes and colors. Without LVGL.
namespace gbx::ui {

using pack_format::MenuColor;

struct MenuTheme {
    int width = 0;   // the grid
    int height = 0;
    int scale = 1;
    std::array<uint16_t, pack_format::kMenuColorCount> color{};  // RGB565
    std::array<pack_format::Rgb, pack_format::kMenuColorCount> rgb{};
    std::span<const pack_format::MenuPalette> palettes;
    std::span<const pack_format::MenuIcon> icons;

    bool load(const Pack& pack);
    uint16_t operator[](MenuColor c) const { return color[static_cast<int>(c)]; }
};

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b);

// RGB565 pixels with a clip rectangle that every drawing call keeps to.
struct Surface {
    uint16_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    int clip_x0 = 0, clip_y0 = 0, clip_x1 = 0, clip_y1 = 0;

    Surface(uint16_t* p, int w, int h) : pixels(p), width(w), height(h), clip_x1(w), clip_y1(h) {}
    void fill(int x, int y, int w, int h, uint16_t color);
    // Mixes the area toward `color` by amount / 256.
    void blend(int x, int y, int w, int h, pack_format::Rgb color, int amount);
    void rrect(int x, int y, int w, int h, int r, uint16_t color);
    // A rounded rectangle with a 1 px outline.
    void frame(int x, int y, int w, int h, int r, uint16_t line, uint16_t inside);
};

class MenuPainter {
public:
    static constexpr int kLine = 16;
    static constexpr int kHeader = 16;
    static constexpr int kRow = kMenuRowHeight;
    static constexpr int kRows = MenuScreen::kRows;
    static constexpr int kFooter = kHeader + kRows * kRow;  // the rule above the footer line

    MenuPainter(const MenuTheme& theme, const MenuFont& font) : theme_(theme), font_(font) {}

    // Draws `screen` over the whole grid. `backdrop` is the dimmed game frame
    // the pause menu floats on, a full grid of pixels, and `thumbnail` the
    // SAVE / LOAD thumbnail, BGR555 of thumbnail_size() for the screen.
    // `marquee` moves the selected ROM's name that many pixels to the left.
    void draw(const MenuScreen& screen, Surface& s, const uint16_t* backdrop, const uint16_t* thumbnail,
              int marquee) const;
    MenuHit hit(const MenuScreen& screen, int x, int y) const;

    // How far the selected ROM's name can scroll: its width beyond the room
    // it has, or 0 when it fits.
    int marquee_range(const MenuScreen& screen) const;

    // The dimmed game frame under the pause menu, from BGR555 pixels, in
    // the middle of the grid.
    void backdrop(std::span<const uint16_t> frame, int width, int height, uint16_t* out) const;

    // SAVE / LOAD draws the selected slot's thumbnail at half size, one
    // pixel a cell: a GB's 160x144, or a GBA's 240x160 when the screen is
    // wide. At 2x a view shows it at full size instead, one screen pixel a
    // pixel, over the same cells: this gives the top left cell, or false
    // when the screen shows no thumbnail at full size, such as under a
    // dialog.
    bool thumbnail_at(const MenuScreen& screen, int& x, int& y) const;
    static void thumbnail_size(const MenuScreen& screen, int& width, int& height) {
        width = screen.slots.wide ? kWideThumbnailWidth : kThumbnailWidth;
        height = screen.slots.wide ? kWideThumbnailHeight : kThumbnailHeight;
    }
    // The thumbnail's RGB565 pixels in rows `stride` pixels apart, from
    // BGR555 of `width` x `height`.
    static void thumbnail(const uint16_t* pixels, int width, int height, uint16_t* out, int stride);
    static constexpr int kThumbnailWidth = 160;
    static constexpr int kThumbnailHeight = 144;
    static constexpr int kWideThumbnailWidth = 240;
    static constexpr int kWideThumbnailHeight = 160;

private:
    struct Fitted {
        std::string_view text;
        bool cut;  // ends with an ellipsis
    };
    struct DialogBox {
        int x, y, w, h, top, buttons_y;
        int line_count;
        std::array<std::string_view, 12> lines;  // wrapped text; empty view for a two-column line
        std::array<int, 12> source;              // index into the dialog's lines
    };
    struct PauseBox {
        int x, y, w, h, top;
    };

    uint16_t c(MenuColor color) const { return theme_[color]; }
    int measure(std::string_view text) const { return font_.measure(text); }
    Fitted fit(std::string_view text, int width) const;
    enum class Align : uint8_t { left, right, center };
    // A width for text() that does not cut the text.
    static constexpr int kNoWidth = INT_MIN;
    int text(Surface& s, int x, int top, std::string_view text, uint16_t color, int width = kNoWidth,
             Align align = Align::left) const;
    int glyphs(Surface& s, int x, int top, std::string_view text, uint16_t color) const;
    int wrap(std::string_view text, int width, std::span<std::string_view> lines) const;
    void icon(Surface& s, std::string_view name, int x, int y, uint16_t color, int zoom = 1) const;

    void header(Surface& s, std::string_view title, std::string_view path, std::string_view right) const;
    void footer(Surface& s, const MenuScreen& m) const;
    void badge(Surface& s, int x, int y, MenuScreen::Badge badge, bool enabled, bool selected) const;
    void scrollbar(Surface& s, int first, int total) const;
    uint16_t bar(bool enabled) const { return c(enabled ? MenuColor::accent : MenuColor::muted); }
    uint16_t ink_of(MenuScreen::Ink ink) const;
    void triangle(Surface& s, int x, int y, bool left, uint16_t color) const;
    int buttons_width(std::span<const char* const> labels, std::span<int> widths) const;
    // A row of buttons centered on cx. `selected` -1 selects none; `enabled`,
    // when given, grays the buttons that cannot be used.
    void buttons(Surface& s, int cx, int y, std::span<const char* const> labels, int selected,
                 std::span<const bool> enabled = {}) const;
    MenuHit hit_buttons(int x, int y, int cx, int top, std::span<const char* const> labels) const;

    void home(const MenuScreen& m, Surface& s) const;
    void roms(const MenuScreen& m, Surface& s, int marquee) const;
    void message(const MenuScreen& m, Surface& s) const;
    // A message's icon, its heading in `heading` and up to three lines of
    // text; WIRELESS GAMEPAD shows the same over its buttons.
    void notice(const MenuScreen& m, Surface& s, uint16_t heading) const;
    void settings(const MenuScreen& m, Surface& s) const;
    void pause(const MenuScreen& m, Surface& s, const uint16_t* backdrop) const;
    void errors(const MenuScreen& m, Surface& s) const;
    void loading(const MenuScreen& m, Surface& s) const;
    void slots(const MenuScreen& m, Surface& s, const uint16_t* thumbnail) const;
    void gamepad(const MenuScreen& m, Surface& s) const;
    void dialog(const MenuScreen& m, Surface& s) const;

    // Layout shared by drawing and hit testing.
    struct HomeCards {
        int top, height, gap;
    };
    HomeCards home_cards(const MenuScreen& m) const;
    int rom_right(const MenuScreen& m) const;
    int rom_name_right(const MenuScreen& m, const MenuScreen::Row& row) const;
    // SETTINGS: six rows fill the page between the title bar and the footer.
    static constexpr int kSettingRow = 21;
    static constexpr int kSettingLeft = 10;
    int setting_right() const { return theme_.width - 10; }
    int setting_y(int i) const { return kHeader + i * kSettingRow; }
    int level_x(int right) const { return right - 34 - 10 * 6 + 1; }
    int choice_x(const MenuScreen::Row& row, int k) const;
    int choice_width(const MenuScreen::Row& row, int k) const;
    PauseBox pause_box(const MenuScreen& m) const;
    // SAVE / LOAD: rows of two lines on the left; the thumbnail's frame and
    // the buttons, centered under it, on the right. A GBA's wider
    // thumbnail takes room from the rows.
    static constexpr int kSlotRow = 30;
    static constexpr int kSlotTop = kHeader + 3;
    static int slot_right(const MenuScreen& m) { return m.slots.wide ? 112 : 130; }
    // The frame; the picture is one cell in.
    static int thumbnail_x(const MenuScreen& m) { return m.slots.wide ? 116 : 145; }
    static constexpr int kThumbnailY = 19;
    static int slot_buttons_x(const MenuScreen& m) {  // their center
        int w, h;
        thumbnail_size(m, w, h);
        return thumbnail_x(m) + (w / 2 + 2) / 2;
    }
    static constexpr int kSlotButtonsY = kSlotTop + 4 * kSlotRow - 15;
    DialogBox dialog_box(const MenuScreen& m) const;

    const MenuTheme& theme_;
    const MenuFont& font_;
};

}  // namespace gbx::ui
