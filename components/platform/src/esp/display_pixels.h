#pragma once

#include "display_backend.h"

namespace gbx::platform::display::detail {

// BGR555 to RGB565; green's new low bit repeats its top bit.
inline uint16_t to_rgb565(uint16_t c) {
    const unsigned r = c & 31;
    const unsigned g = (c >> 5) & 31;
    const unsigned b = (c >> 10) & 31;
    return static_cast<uint16_t>((r << 11) | (((g << 1) | (g >> 4)) << 5) | b);
}

// A game pixel in RGB565: as it is in an RGB565 layer, otherwise through the
// layer's color table when it has one.
inline uint16_t game_rgb565(uint16_t c, const GameLayer& layer) {
    if (layer.rgb565) return c;
    return layer.colors ? layer.colors[c & 0x7fff] : to_rgb565(c);
}

// Writes n pixels of one UI row, starting at (ux, uy), into a frame buffer in
// panel orientation.
void put_row(uint16_t* fb, const Config& config, int ux, int uy, const uint16_t* pixels, int n);

// Copies one rectangle of the shadow into a frame buffer in panel
// orientation. With a quarter turn, each UI column is a panel row and is
// written in order; going along the UI rows would write one pixel in each of
// r.x1 - r.x0 cache lines per row instead.
void put_rect(uint16_t* fb, const Config& config, const Rect& r);

// Draws a game frame scaled by layer.scale with nearest-neighbour sampling.
void draw_nearest(uint16_t* fb, const Config& config, const GameLayer& layer, const uint16_t* frame);

// The nearest-neighbour image of a game frame in panel orientation, with
// each run of layer.scale equal rows stored once: row j of `out` is panel
// row j * layer.scale of layer.image, which repeats on the layer.scale - 1
// rows below it. Rows are as wide as layer.image on the panel and packed,
// layer.width * layer.height * layer.scale pixels in all.
void widen_rows(uint16_t* out, const Config& config, const GameLayer& layer, const uint16_t* frame);

}  // namespace gbx::platform::display::detail
