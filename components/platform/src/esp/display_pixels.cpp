#include "display_pixels.h"

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
