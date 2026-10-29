#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "gbx/ui/pack.h"
#include "gbx/ui/pack_format.h"

namespace gbx::ui {

// An RGB565 buffer that holds the screen rectangle (x, y, width, height).
// Drawing outside the rectangle is clipped.
struct Canvas {
    uint16_t* pixels;
    int x;
    int y;
    int width;
    int height;
    int stride;  // pixels per row
};

// Blends an image from the pack onto the canvas at the image's own screen position.
void blit(Canvas& canvas, const ImageView& image);

// The 5x7 dot-matrix VFD font, one glowing atlas per dot pitch.
class VfdFont {
public:
    bool load(const Pack& pack);

    // Width of the dot grids of `text` at `pitch`, without the glow.
    static int text_width(std::string_view text, int pitch);

    // Draws `text` with the top-left of its first dot grid at (x, y), rounded to whole pixels.
    void draw(Canvas& canvas, std::string_view text, double x, double y, int pitch) const;

private:
    struct Atlas {
        const pack_format::Font* font = nullptr;
        ImageView image{};
    };
    const Atlas* atlas(int pitch) const;
    struct FreeExternal {
        void operator()(uint8_t* memory) const;
    };

    std::string_view chars_;
    Atlas atlases_[4];
    // The atlases' planes copied out of the pack, which is in flash: each
    // glyph pixel reads them. None with GBX_UI_PACK_IN_FLASH.
    std::unique_ptr<uint8_t[], FreeExternal> copies_[4];
    int atlas_count_ = 0;
};

// A telemetry value and whether it is known, see
// docs/ui-layouts/telemetry-fields.json.
struct Reading {
    enum class State : uint8_t { uncollected, unsupported, stale, warmup, valid };
    State state = State::uncollected;
    double value = 0;
};

// Finds the reading of a field by its name in telemetry-fields.json.
using FieldLookup = Reading (*)(std::string_view field, const void* context);

// Fills a binding such as "EMU {emu_fps:.1f}" with the readings `lookup`
// returns: a value in the field's format, or "--", "N/A", "STALE" or
// "WARMUP". A field without a format shows a whole number. Writes into `out`
// and returns the text, truncated to the buffer.
std::string_view format_binding(std::string_view binding, FieldLookup lookup, const void* context,
                                std::span<char> out);

// A binding with every field shown as uncollected ("--").
std::string_view placeholder(std::string_view binding, std::span<char> out);

// Draws one VFD cell over the background already on the canvas. `unit`
// replaces the cell's unit when given; an empty unit hides it, as for status
// text such as "NO ROM". `lines` holds the text of the cell's lines, in order.
void draw_cell(Canvas& canvas, const VfdFont& font, const pack_format::VfdCell& cell, std::string_view value,
               std::optional<std::string_view> unit, std::span<const std::string_view> lines);

}  // namespace gbx::ui
