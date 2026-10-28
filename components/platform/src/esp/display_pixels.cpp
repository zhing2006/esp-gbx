#include "display_pixels.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace gbx::platform::display::detail {

using board::Rotation;

void put_row(uint16_t* fb, const Config& config, int ux, int uy, const uint16_t* pixels, int n) {
    const auto& panel = config.panel;
    const auto& ui = config.ui;
    const int pw = panel.width;
    const int uw = ui.width;
    const int uh = ui.height;
    switch (panel.rotation) {
        case Rotation::none:
            std::memcpy(fb + uy * pw + ux, pixels, n * sizeof(uint16_t));
            break;
        case Rotation::cw90:
            for (int i = 0; i < n; ++i) {
                fb[(ux + i) * pw + uh - 1 - uy] = pixels[i];
            }
            break;
        case Rotation::cw180: {
            uint16_t* row = fb + (uh - 1 - uy) * pw + uw - 1 - ux;
            for (int i = 0; i < n; ++i) {
                row[-i] = pixels[i];
            }
            break;
        }
        case Rotation::cw270:
            for (int i = 0; i < n; ++i) {
                fb[(uw - 1 - ux - i) * pw + uy] = pixels[i];
            }
            break;
    }
}

void put_rect(uint16_t* fb, const Config& config, const Rect& r) {
    const auto& panel = config.panel;
    const auto& ui = config.ui;
    const auto* shadow = config.shadow;
    const int pw = panel.width;
    const int uw = ui.width;
    const int uh = ui.height;
    switch (panel.rotation) {
        case Rotation::none:
            for (int y = r.y0; y < r.y1; ++y) {
                std::memcpy(fb + y * pw + r.x0, shadow + y * uw + r.x0, (r.x1 - r.x0) * sizeof(uint16_t));
            }
            break;
        case Rotation::cw90:  // panel (x, y) = (uh - 1 - uy, ux)
            for (int ux = r.x0; ux < r.x1; ++ux) {
                uint16_t* row = fb + ux * pw;
                for (int uy = r.y0; uy < r.y1; ++uy) {
                    row[uh - 1 - uy] = shadow[uy * uw + ux];
                }
            }
            break;
        case Rotation::cw180:  // panel (x, y) = (uw - 1 - ux, uh - 1 - uy)
            for (int uy = r.y0; uy < r.y1; ++uy) {
                uint16_t* row = fb + (uh - 1 - uy) * pw;
                for (int ux = r.x0; ux < r.x1; ++ux) {
                    row[uw - 1 - ux] = shadow[uy * uw + ux];
                }
            }
            break;
        case Rotation::cw270:  // panel (x, y) = (uy, uw - 1 - ux)
            for (int ux = r.x0; ux < r.x1; ++ux) {
                uint16_t* row = fb + (uw - 1 - ux) * pw;
                for (int uy = r.y0; uy < r.y1; ++uy) {
                    row[uy] = shadow[uy * uw + ux];
                }
            }
            break;
    }
}

void draw_nearest(uint16_t* fb, const Config& config, const GameLayer& layer, const uint16_t* frame) {
    const auto& panel = config.panel;
    const int s = layer.scale;
    if (panel.rotation == Rotation::none && s == 2 && !((layer.image.x0 | panel.width) & 1)) {
        // The common case: each source pixel is one 32-bit word on two rows,
        // stored straight into the frame buffer.
        for (int y = 0; y < layer.height; ++y) {
            const uint16_t* source = frame + y * layer.width;
            auto* top = reinterpret_cast<uint32_t*>(fb + (layer.image.y0 + y * 2) * panel.width + layer.image.x0);
            uint32_t* bottom = top + panel.width / 2;
            uint16_t last = static_cast<uint16_t>(source[0] ^ 1);
            uint32_t pair = 0;
            for (int x = 0; x < layer.width; ++x) {
                if (source[x] != last) {
                    last = source[x];
                    // GCC 15 hoists the conversion above the comparison in
                    // this standalone function. Keep it dependent on the
                    // changed-color branch; this emits no instruction.
                    uint16_t changed = last;
                    asm volatile("" : "+r"(changed));
                    pair = game_rgb565(changed, layer) * 0x10001u;
                }
                top[x] = pair;
                bottom[x] = pair;
            }
        }
        return;
    }
    uint16_t line[kMaxLine];
    for (int y = 0; y < layer.height; ++y) {
        const uint16_t* source = frame + y * layer.width;
        // Runs of one color are common, so convert only on a change.
        uint16_t last = static_cast<uint16_t>(source[0] ^ 1);
        uint16_t color = 0;
        for (int x = 0; x < layer.width; ++x) {
            if (source[x] != last) {
                last = source[x];
                color = game_rgb565(last, layer);
            }
            for (int k = 0; k < s; ++k) {
                line[x * s + k] = color;
            }
        }
        for (int k = 0; k < s; ++k) {
            put_row(fb, config, layer.image.x0, layer.image.y0 + y * s + k, line, layer.width * s);
        }
    }
}

void set_shade_pairs(GameLayer& layer, std::span<const uint16_t, 4> colors) {
    for (unsigned i = 0; i < 16; ++i) {
        layer.shade_pairs[i] = game_rgb565(colors[i & 3], layer) | uint32_t{game_rgb565(colors[i >> 2], layer)} << 16;
    }
}

namespace {

// One row of the image from a row of shades, see game_band().
void shade_row(uint16_t* pixel, const GameLayer& layer, const uint8_t* source) {
    const int s = layer.scale;
    if (s == 1 && !(layer.width & 3) &&
        !((reinterpret_cast<uintptr_t>(source) | reinterpret_cast<uintptr_t>(pixel)) & 3)) {
        // Four shades a word from PSRAM, two pixels a word out.
        const auto* quads = reinterpret_cast<const uint32_t*>(source);
        auto* pairs = reinterpret_cast<uint32_t*>(pixel);
        for (int i = 0; i < layer.width / 4; ++i) {
            const uint32_t q = quads[i];
            pairs[2 * i] = layer.shade_pairs[(q & 3) | (q >> 6 & 12)];
            pairs[2 * i + 1] = layer.shade_pairs[(q >> 16 & 3) | (q >> 22 & 12)];
        }
        return;
    }
    for (int x = 0; x < layer.width; ++x) {
        const auto color = static_cast<uint16_t>(layer.shade_pairs[source[x] & 3]);
        for (int k = 0; k < s; ++k) *pixel++ = color;
    }
}

}  // namespace

void game_band(uint16_t* out, int x0, int x1, const uint16_t* fb, const Config& config, const GameLayer& layer,
               const uint16_t* frame, int first, int count) {
    const int pw = config.panel.width;
    const int width = x1 - x0;
    const Rect& w = layer.window;
    const Rect& image = layer.image;
    const int s = layer.scale;
    for (int y = first; y < first + count; ++y) {
        uint16_t* row = out + (y - first) * width;
        // Panel column x is row[x - x0].
        uint16_t* const at = row - x0;
        if (x0 < w.x0) std::memcpy(at + x0, fb + y * pw + x0, (w.x0 - x0) * sizeof(uint16_t));
        if (x0 < image.x0) std::fill(at + std::max(x0, w.x0), at + image.x0, uint16_t{0});
        if (x1 > image.x1) std::fill(at + image.x1, at + std::min(x1, w.x1), uint16_t{0});
        if (x1 > w.x1) std::memcpy(at + w.x1, fb + y * pw + w.x1, (x1 - w.x1) * sizeof(uint16_t));
        const uint16_t* source = frame + (y - image.y0) / s * layer.width;
        uint16_t* pixel = at + image.x0;
        if (y > first && (y - image.y0) % s) {
            // The same source row as the one above.
            std::memcpy(pixel, pixel - width, (image.x1 - image.x0) * sizeof(uint16_t));
        } else if (layer.shades) {
            shade_row(pixel, layer, reinterpret_cast<const uint8_t*>(frame) + (y - image.y0) / s * layer.width);
        } else if (s == 1 && !(layer.width & 1) &&
                   !((reinterpret_cast<uintptr_t>(source) | reinterpret_cast<uintptr_t>(pixel)) & 3)) {
            // Two pixels a word, converted when the pair changes: runs of one
            // color are common, and the source is in PSRAM.
            const auto* pairs = reinterpret_cast<const uint32_t*>(source);
            auto* out_pairs = reinterpret_cast<uint32_t*>(pixel);
            uint32_t last = ~pairs[0];
            uint32_t converted = 0;
            for (int i = 0; i < layer.width / 2; ++i) {
                const uint32_t pair = pairs[i];
                if (pair != last) {
                    last = pair;
                    converted = game_rgb565(static_cast<uint16_t>(pair), layer) |
                                uint32_t{game_rgb565(static_cast<uint16_t>(pair >> 16), layer)} << 16;
                }
                out_pairs[i] = converted;
            }
        } else {
            // Runs of one color are common, so convert only on a change.
            uint16_t last = static_cast<uint16_t>(source[0] ^ 1);
            uint16_t color = 0;
            for (int x = 0; x < layer.width; ++x) {
                if (source[x] != last) {
                    last = source[x];
                    color = game_rgb565(last, layer);
                }
                for (int k = 0; k < s; ++k) *pixel++ = color;
            }
        }
    }
}

void widen_rows(uint16_t* out, const Config& config, const GameLayer& layer, const uint16_t* frame) {
    const int w = layer.width;
    const int h = layer.height;
    const int s = layer.scale;
    // Row j of the panel image samples source pixels from first + j * across,
    // `along` apart; see put_row() for where each UI pixel goes.
    const uint16_t* first = frame;
    int rows = h, count = w, across = w, along = 1;
    switch (config.panel.rotation) {
        case Rotation::none:
            break;
        case Rotation::cw90:  // panel row j is source column j, bottom up
            first = frame + (h - 1) * w;
            rows = w, count = h, across = 1, along = -w;
            break;
        case Rotation::cw180:
            first = frame + h * w - 1;
            across = -w, along = -1;
            break;
        case Rotation::cw270:  // panel row j is source column w - 1 - j, top down
            first = frame + w - 1;
            rows = w, count = h, across = -1, along = w;
            break;
    }
    for (int j = 0; j < rows; ++j) {
        const uint16_t* source = first + j * across;
        uint16_t* row = out + j * count * s;
        // Runs of one color are common, so convert only on a change.
        uint16_t last = static_cast<uint16_t>(source[0] ^ 1);
        if (s == 2) {
            // Each source pixel is one 32-bit word; rows are an even number
            // of pixels long, so every row starts on a word.
            auto* pairs = reinterpret_cast<uint32_t*>(row);
            uint32_t pair = 0;
            for (int i = 0; i < count; ++i) {
                const uint16_t pixel = source[i * along];
                if (pixel != last) {
                    last = pixel;
                    // As in draw_nearest(): keep GCC 15 from hoisting the
                    // conversion above the comparison.
                    uint16_t changed = last;
                    asm volatile("" : "+r"(changed));
                    pair = game_rgb565(changed, layer) * 0x10001u;
                }
                pairs[i] = pair;
            }
            continue;
        }
        uint16_t color = 0;
        for (int i = 0; i < count; ++i) {
            const uint16_t pixel = source[i * along];
            if (pixel != last) {
                last = pixel;
                color = game_rgb565(last, layer);
            }
            for (int k = 0; k < s; ++k) {
                row[i * s + k] = color;
            }
        }
    }
}

}  // namespace gbx::platform::display::detail
