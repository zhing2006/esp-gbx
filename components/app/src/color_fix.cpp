#include "color_fix.h"

#include <algorithm>
#include <cmath>

#include "gbx/platform.h"
#include "lcd_colors.h"

namespace gbx::app {
namespace {

uint16_t* table = nullptr;

// Linear light to sRGB's encoding, 0 to 1.
float encode(float linear) {
    linear = std::clamp(linear, 0.0f, 1.0f);
    return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1 / 2.4f) - 0.055f;
}

}  // namespace

const uint16_t* color_fix_table() {
    if (table) return table;
    auto* out = static_cast<uint16_t*>(platform::allocate_external(32768 * sizeof(uint16_t)));
    if (!out) return nullptr;
    using lcd::kMatrix, lcd::kOffset, lcd::kResponse;
    for (unsigned c = 0; c < 32768; ++c) {
        const float r = kResponse[c & 31], g = kResponse[(c >> 5) & 31], b = kResponse[c >> 10];
        float rgb[3];
        for (int i = 0; i < 3; ++i) rgb[i] = encode(kMatrix[i][0] * r + kMatrix[i][1] * g + kMatrix[i][2] * b + kOffset[i]);
        out[c] = static_cast<uint16_t>(std::lround(rgb[0] * 31) << 11 | std::lround(rgb[1] * 63) << 5 |
                                       std::lround(rgb[2] * 31));
    }
    table = out;
    return table;
}

uint16_t color_fix(uint16_t color) {
    if (!table) return color;
    const unsigned c = table[color & 0x7fff];
    return static_cast<uint16_t>((c >> 11) | ((c >> 6) & 31) << 5 | (c & 31) << 10);
}

}  // namespace gbx::app
