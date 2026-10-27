#pragma once

#include <cstdint>

// On-flash layout of the UI asset pack written by tools/ui/pack_assets.py. The
// record sizes and field order must match the struct formats in that script;
// bump kPackVersion in both places when they change. All values are little
// endian and every entry starts on a 64-byte boundary.
namespace gbx::ui::pack_format {

inline constexpr char kMagic[8] = {'G', 'B', 'X', 'U', 'I', 'P', 'K', '\0'};
inline constexpr uint32_t kPackVersion = 4;
inline constexpr uint32_t kAlign = 64;

struct Header {
    char magic[8];
    uint32_t version;
    uint32_t total_size;  // whole pack, header included
    uint32_t crc32;       // CRC-32 (zlib) of the bytes after the header
    uint32_t entry_count;
    uint32_t entries_offset;
    uint16_t width;  // logical screen size
    uint16_t height;
    char board[32];
};
static_assert(sizeof(Header) == 64);

// The pack has one "layout" entry, one "menu" entry and the "menu_font" entry,
// which holds assets/ui/common/menu_font.bin as tools/ui/menu_font.py wrote it.
// The menus' sound cues are "sound.<cue>" entries.
enum class EntryType : uint32_t { image = 1, layout = 2, menu = 3, font = 4, sound = 5 };

struct Entry {
    char name[32];
    EntryType type;
    uint32_t offset;  // from the start of the pack
    uint32_t size;
    uint32_t reserved;
};
static_assert(sizeof(Entry) == 48);

enum class PixelFormat : uint16_t {
    rgb565 = 1,    // width * height RGB565 pixels
    rgb565a8 = 2,  // the RGB565 plane, then a width * height plane of 8-bit alpha
};

// Image entries start with this header. Sprites are already scaled and placed:
// (x, y) is the top-left corner on the logical screen.
struct Image {
    PixelFormat format;
    uint16_t width;
    uint16_t height;
    uint16_t stride;  // bytes per row of the RGB565 plane
    int16_t x;
    int16_t y;
    uint32_t data_offset;  // from the start of the pack, 64-byte aligned
};
static_assert(sizeof(Image) == 16);

// Sound entries: this header, then `frames` 16-bit mono samples at `rate`,
// from the WAV files of assets/ui/common/sounds/.
struct Sound {
    uint32_t rate;
    uint32_t frames;
};
static_assert(sizeof(Sound) == 8);

// The "layout" entry: this header, then control_count Control records,
// lamp_count Lamp records, font_count Font records and cell_count VfdCell
// records, back to back.
struct Layout {
    int16_t display[4];  // game window: x, y, width, height
    uint16_t control_count;
    uint16_t lamp_count;
    uint16_t font_count;
    uint16_t cell_count;
    char font_chars[64];  // characters of the VFD font, in atlas order
};
static_assert(sizeof(Layout) == 80);

enum class ControlKind : uint8_t { button = 0, dpad = 1, speed_switch = 2 };
enum class Shape : uint8_t { circle = 0, capsule = 1, rect = 2 };

// Images are named "ctl.<id>.<state>": normal and pressed for buttons, normal
// and the eight directions for the D-pad, 1x and 2x for the speed switch.
struct Control {
    char id[16];
    ControlKind kind;
    Shape shape;
    uint8_t reserved[2];
    float cx;     // shape center
    float cy;
    float a;      // circle: radius; capsule: half the segment length; rect: half width
    float b;      // capsule: radius; rect: half height
    float angle;  // capsule direction, radians
    float margin;  // distance outside the shape that still counts as a press
    float dead;    // D-pad: radius around the center with no direction
};
static_assert(sizeof(Control) == 48);

// A lamp lit in color c is the image "lamp.<id>.<c>"; unused colors are empty.
struct Lamp {
    char id[8];
    char colors[2][8];
};
static_assert(sizeof(Lamp) == 24);

// The atlas image "font.<pitch>" holds one glyph with its glow per cell, in
// font_chars order, `columns` cells per row. The dot grid starts `margin` px
// inside the cell; the pen advances `advance` px per character.
struct Font {
    uint8_t pitch;
    uint8_t margin;
    uint16_t cell_width;
    uint16_t cell_height;
    uint16_t columns;
    uint16_t advance;
    uint16_t reserved;
};
static_assert(sizeof(Font) == 12);

enum class UnitPosition : uint8_t { none = 0, right = 1, below = 2 };
enum class Align : uint8_t { left = 0, right = 1 };

// A line of small text in a VFD cell besides its value, such as "AVG 5.9".
struct VfdLine {
    float x;    // where the text starts, or ends when aligned right
    float top;  // top of the dot grid at the configured pitch
    char text[96];  // binding such as "EMU {emu_fps:.1f}"
    uint8_t pitch;
    Align align;
    uint8_t reserved[2];
};
static_assert(sizeof(VfdLine) == 108);

inline constexpr int kMaxCellLines = 8;

// One VFD cell, laid out by tools/ui/export_boards.py; see docs/ui-layouts.
struct VfdCell {
    char id[8];
    float rect[4];
    float label[4];
    float right;  // the value and unit end here
    float top;    // top of the value's dot grid at the configured pitch
    float left_limit;
    char value[48];   // binding such as "{emu_fps:.1f}"
    char unit[8];
    uint8_t pitch;
    uint8_t unit_pitch;
    UnitPosition unit_position;
    uint8_t line_count;  // at most kMaxCellLines
    VfdLine lines[kMaxCellLines];  // the first line_count are used
};
static_assert(sizeof(VfdCell) == 976);

// The menus' colors from assets/ui/common/menu.json, in pack order.
enum class MenuColor : uint8_t {
    paper, card, ink, muted, disabled, rule, header, header_ink, header_muted, accent, accent_ink, accent_dim,
    alert, gb, gbc, gba, badge_ink, clean, dirty, error, scrim, shadow,
};
inline constexpr int kMenuColorCount = 22;

struct Rgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t reserved;
};
static_assert(sizeof(Rgb) == 4);

// The "menu" entry: this header, then color_count Rgb records in MenuColor
// order, palette_count MenuPalette and icon_count MenuIcon records.
struct Menu {
    uint16_t grid_width;   // the menus are drawn on this grid
    uint16_t grid_height;
    uint8_t scale;         // and shown this many times larger in the game window
    uint8_t color_count;
    uint8_t palette_count;
    uint8_t icon_count;
};
static_assert(sizeof(Menu) == 8);

// The four colors of black and white games, lightest first.
struct MenuPalette {
    char id[8];
    uint8_t shades[4][3];  // RGB
};
static_assert(sizeof(MenuPalette) == 20);

// A 16x16 one-color icon; bit 15 of a row is its leftmost pixel.
struct MenuIcon {
    char id[12];
    uint16_t rows[16];
};
static_assert(sizeof(MenuIcon) == 44);

}  // namespace gbx::ui::pack_format
