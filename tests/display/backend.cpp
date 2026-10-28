#include "display_backend.h"
#include "display_pixels.h"
#include "display_test.h"
#include "transport.h"

#include <algorithm>
#include <array>
#include <string_view>

#include "esp_private/esp_cache_private.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

namespace gbx::platform::display::detail {
namespace {
constexpr int kWidth = 100, kHeight = 72;
constexpr size_t kBytes = kWidth * kHeight * sizeof(uint16_t);
constexpr const char* kTag = "display-check";

uint16_t pattern(int x, int y) {
    return static_cast<uint16_t>((((x ^ y) & 31) << 11) | (((x * 7 + y * 3) & 63) << 5) | ((x + 3 * y) & 31));
}
uint16_t sentinel(int i) { return static_cast<uint16_t>(0xa55a ^ (i & 255)); }
bool contains(const Rect& r, int x, int y) { return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1; }

// Invert the panel coordinates independently of the backend's forward map.
void logical(const Config& c, int x, int y, int& ux, int& uy) {
    switch (c.panel.rotation) {
        case board::Rotation::none: ux = x; uy = y; break;
        case board::Rotation::cw90: ux = y; uy = kHeight - 1 - x; break;
        case board::Rotation::cw180: ux = kWidth - 1 - x; uy = kHeight - 1 - y; break;
        case board::Rotation::cw270: ux = kWidth - 1 - y; uy = x; break;
    }
}

}  // namespace

bool run_pattern_checks(std::array<uint32_t, 8>& masks) {
    Config config{};
    std::array<unsigned, 8> counts{};
    size_t alignment = 0;
    ESP_ERROR_CHECK(esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &alignment));
    auto* source = static_cast<uint16_t*>(heap_caps_aligned_alloc(alignment, kBytes, MALLOC_CAP_SPIRAM));
    auto* allocation = static_cast<uint16_t*>(heap_caps_aligned_alloc(alignment, kBytes + 2 * alignment,
                                                                    MALLOC_CAP_SPIRAM));
    if (!source || !allocation) {
        heap_caps_free(source);
        heap_caps_free(allocation);
        ESP_LOGE(kTag, "cannot allocate pattern buffers");
        return false;
    }
    if (!backend_start()) {
        heap_caps_free(source);
        heap_caps_free(allocation);
        return false;
    }
    auto* output = allocation + alignment / sizeof(uint16_t);
    config.shadow = source;
    config.ui = {kWidth, kHeight};
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x) source[y * kWidth + x] = pattern(x, y);
    const auto reset = [&] {
        std::fill_n(allocation, (kBytes + 2 * alignment) / 2, 0x73e1);
        for (int i = 0; i < kWidth * kHeight; ++i) output[i] = sentinel(i);
    };
    const auto guard_ok = [&] {
        for (size_t i = 0; i < alignment / 2; ++i)
            if (allocation[i] != 0x73e1 || output[kWidth * kHeight + i] != 0x73e1) return false;
        return true;
    };
    const auto verify = [&](unsigned group, const char* operation, const auto& expected) {
        const uint32_t bit = 1u << counts[group]++;
        if (!guard_ok()) {
            ESP_LOGE(kTag, "%s damaged guard pixels", operation);
            return false;
        }
        for (int y = 0; y < config.panel.height; ++y) {
            for (int x = 0; x < config.panel.width; ++x) {
                int ux = 0, uy = 0;
                logical(config, x, y, ux, uy);
                const int i = y * config.panel.width + x;
                const uint16_t want = expected(ux, uy, i);
                if (output[i] != want) {
                    ESP_LOGE(kTag, "%s rotation %d panel (%d,%d): got %04x expected %04x",
                             operation, static_cast<int>(config.panel.rotation), x, y, output[i], want);
                    return false;
                }
            }
        }
        masks[group] |= bit;
        return true;
    };
    const Rect rectangles[] = {{0, 0, kWidth, kHeight}, {3, 5, 31, 28},
                               {99, 71, 100, 72}, {0, 0, 1, 1}, {1, 1, 99, 71}};
    // Primary colors independently encode the BGR555-to-RGB565 contract.
    constexpr uint16_t bgr[] = {0x001f, 0x03e0, 0x7c00, 0x7fff, 0};
    constexpr uint16_t rgb[] = {0xf800, 0x07e0, 0x001f, 0xffff, 0};
    std::array<uint16_t, 40> frame;
    for (size_t i = 0; i < frame.size(); ++i) frame[i] = bgr[i % 5];
    for (const auto turn : {board::Rotation::none, board::Rotation::cw90,
                           board::Rotation::cw180, board::Rotation::cw270}) {
        config.panel.rotation = turn;
        const bool sideways = turn == board::Rotation::cw90 || turn == board::Rotation::cw270;
        config.panel.width = sideways ? kHeight : kWidth;
        config.panel.height = sideways ? kWidth : kHeight;
        for (const auto& r : rectangles) {
            reset();
            copy_ui_rect(output, config, r);
            verify(0, "copy", [&](int x, int y, int i) {
                return contains(r, x, y) ? pattern(x, y) : sentinel(i);
            });
#if CONFIG_GBX_DISPLAY_ACCEL
            reset();
            copy_ui_rect_accelerated(output, config, r);
            verify(1, "hardware copy", [&](int x, int y, int i) {
                return contains(r, x, y) ? pattern(x, y) : sentinel(i);
            });
#endif
            reset();
            fill_black(output, config, r);
            verify(2, "fill", [&](int x, int y, int i) -> uint16_t {
                return contains(r, x, y) ? 0 : sentinel(i);
            });
        }
        for (int scale = 1; scale <= 3; ++scale) {
            GameLayer layer{};
            layer.width = 7;
            layer.height = 5;
            layer.scale = scale;
            layer.image = {scale == 2 ? 4 : 3, 7, 0, 7 + scale * 5};
            layer.image.x1 = layer.image.x0 + scale * 7;
            reset();
            draw_game(output, config, layer, frame.data());
            verify(3, "nearest", [&](int x, int y, int i) {
                if (!contains(layer.image, x, y)) return sentinel(i);
                const int sx = (x - layer.image.x0) / scale;
                const int sy = (y - layer.image.y0) / scale;
                return rgb[(sy * 7 + sx) % 5];
            });
        }
        // Equal and contained updates must not be copied twice or both
        // dropped. Partial overlaps must not expand into untouched pixels.
        const Rect carry[] = {{2, 3, 22, 17}, {5, 5, 15, 11}, {18, 13, 35, 29}};
        const Rect now[] = {{2, 3, 22, 17}, {50, 30, 70, 50}};
        reset();
        unsigned copies = 0;
        for_each_damage_rect(carry, now, [&](const Rect& r) {
            ++copies;
            copy_ui_rect(output, config, r);
        });
        const uint32_t bit = 1u << counts[6];
        verify(6, "damage containment", [&](int x, int y, int i) {
            for (const auto& r : carry) if (contains(r, x, y)) return pattern(x, y);
            for (const auto& r : now) if (contains(r, x, y)) return pattern(x, y);
            return sentinel(i);
        });
        if (copies != 3) {
            masks[6] &= ~bit;
            ESP_LOGE(kTag, "damage containment copied %u rectangles, expected 3", copies);
        }
    }
    // Bands of an SPI panel's rows, unrotated: the image at each scale, an
    // odd width and an even one with runs of one color, whole rows and the
    // image's columns, from BGR555 pixels and from shades. Black fills the
    // rest of the window, the UI shows beside it, and nothing is written
    // past the band.
    config.panel.rotation = board::Rotation::none;
    config.panel.width = kWidth;
    config.panel.height = kHeight;
    for (const bool shaded : {false, true}) {
        for (int scale = 1; scale <= 3; ++scale) {
            for (const int width : {7, 8}) {
                for (const bool whole : {true, false}) {
                    GameLayer layer{};
                    layer.width = width;
                    layer.height = 5;
                    layer.scale = scale;
                    layer.window = {10, 4, 90, 60};
                    layer.image = {width == 7 ? 21 : 20, 9, 0, 9 + scale * 5};
                    layer.image.x1 = layer.image.x0 + scale * width;
                    // The index into bgr and rgb of each source pixel; shades
                    // take the first four colors.
                    const int colors = shaded ? 4 : 5;
                    std::array<uint8_t, 40> index;
                    for (size_t i = 0; i < index.size(); ++i) index[i] = (width == 7 ? i : i / 3) % colors;
                    std::array<uint16_t, 40> pixels;
                    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = bgr[index[i]];
                    alignas(4) std::array<uint8_t, 40> shades = index;
                    if (shaded) {
                        layer.shades = true;
                        set_shade_pairs(layer, std::span<const uint16_t, 4>(bgr, 4));
                    }
                    const int x0 = whole ? 0 : layer.image.x0, x1 = whole ? kWidth : layer.image.x1;
                    // From inside the first run of rows, so a band can start on a repeated row.
                    const int first = layer.image.y0 + 1, count = layer.image.y1 - first;
                    alignas(4) static uint16_t band[kWidth * 15 + 8];
                    std::fill(std::begin(band), std::end(band), uint16_t{0x73e1});
                    const auto* frame = shaded ? reinterpret_cast<const uint16_t*>(shades.data()) : pixels.data();
                    game_band(band, x0, x1, source, config, layer, frame, first, count);
                    bool ok = true;
                    for (int y = first; y < first + count && ok; ++y) {
                        for (int x = x0; x < x1 && ok; ++x) {
                            uint16_t want = pattern(x, y);
                            if (contains(layer.image, x, y)) {
                                const int sx = (x - layer.image.x0) / scale, sy = (y - layer.image.y0) / scale;
                                want = rgb[index[sy * width + sx]];
                            } else if (contains(layer.window, x, y)) {
                                want = 0;
                            }
                            const uint16_t got = band[(y - first) * (x1 - x0) + x - x0];
                            if (got != want) {
                                ESP_LOGE(kTag, "band shades %d scale %d width %d whole %d (%d,%d): got %04x expected "
                                         "%04x", shaded, scale, width, whole, x, y, got, want);
                                ok = false;
                            }
                        }
                    }
                    for (size_t i = static_cast<size_t>(count) * (x1 - x0); ok && i < std::size(band); ++i) {
                        if (band[i] != 0x73e1) {
                            ESP_LOGE(kTag, "band shades %d scale %d width %d whole %d wrote past its rows", shaded,
                                     scale, width, whole);
                            ok = false;
                        }
                    }
                    const uint32_t bit = 1u << counts[7]++;
                    if (ok) masks[7] |= bit;
                }
            }
        }
    }
#if CONFIG_GBX_DISPLAY_ACCEL
    // A constant image is invariant under bilinear sampling at every scale.
    // It also checks the exact output extent and all untouched destination
    // pixels, without treating the emulator's output as a reference image.
    for (const auto turn : {board::Rotation::none, board::Rotation::cw90,
                           board::Rotation::cw180, board::Rotation::cw270}) {
        config.panel.rotation = turn;
        const bool sideways = turn == board::Rotation::cw90 || turn == board::Rotation::cw270;
        config.panel.width = sideways ? kHeight : kWidth;
        config.panel.height = sideways ? kWidth : kHeight;
        GameLayer layer{};
        layer.width = 7;
        layer.height = 5;
        layer.scale = 3;
        layer.scaling = Scaling::smooth;
        layer.image = {3, 7, 24, 22};
        for (int color = 0; color < 5; ++color) {
            frame.fill(bgr[color]);
            reset();
            draw_game(output, config, layer, frame.data());
            verify(4, "smooth constant", [&](int x, int y, int i) {
                return contains(layer.image, x, y) ? rgb[color] : sentinel(i);
            });
        }
        // Bilinear interpolation of equal red/blue rows must stay equal in
        // Y, contain mixed pixels in X and conserve the channel sum to RGB565
        // quantization. Nearest replication cannot pass the mixed-pixel test.
        layer.width = layer.height = 2;
        layer.scale = 4;
        layer.image = {3, 7, 11, 15};
        frame[0] = frame[2] = bgr[0];
        frame[1] = frame[3] = bgr[2];
        reset();
        draw_game(output, config, layer, frame.data());
        std::array<uint16_t, 64> actual{};
        bool bounds_ok = guard_ok();
        for (int y = 0; y < config.panel.height; ++y) {
            for (int x = 0; x < config.panel.width; ++x) {
                int ux = 0, uy = 0;
                logical(config, x, y, ux, uy);
                const int i = y * config.panel.width + x;
                if (contains(layer.image, ux, uy)) actual[(uy - 7) * 8 + ux - 3] = output[i];
                else bounds_ok &= output[i] == sentinel(i);
            }
        }
        bool mixed = false;
        bool interpolated = true;
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const uint16_t c = actual[y * 8 + x];
                const int red = c >> 11, blue = c & 31;
                mixed |= red > 0 && red < 31 && blue > 0 && blue < 31;
                interpolated &= c == actual[x] && (c & 0x7e0) == 0 && red + blue >= 30 && red + blue <= 31;
            }
        }
        if (!mixed || !interpolated) ESP_LOGE(kTag, "smooth interpolation failed, rotation %d", static_cast<int>(turn));
        const uint32_t bit = 1u << counts[5]++;
        if (bounds_ok && mixed && interpolated) masks[5] |= bit;
    }
#endif
    backend_stop();
    heap_caps_free(source);
    heap_caps_free(allocation);
    return true;
}
}  // namespace gbx::platform::display::detail

namespace gbx::test::display_test {
bool run(uint32_t request_id) {
    std::array<uint32_t, 8> masks{};
    if (!platform::display::detail::run_pattern_checks(masks)) {
        constexpr std::string_view error = "Cannot initialize display pattern checks";
        return transport::send(protocol::Kind::error,
                               {reinterpret_cast<const uint8_t*>(error.data()), error.size()});
    }
    std::array<uint32_t, kResultCount> values{};
#if CONFIG_GBX_DISPLAY_ACCEL
    values[0] = 1;
#endif
    std::copy(masks.begin(), masks.end(), values.begin() + 1);
    for (uint16_t i = 0; i < values.size(); ++i) {
        uint8_t data[12];
        protocol::store16(data, kVersion);
        protocol::store16(data + 2, i);
        protocol::store32(data + 4, request_id);
        protocol::store32(data + 8, values[i]);
        if (!transport::send_data(data)) return false;
    }
    if (!transport::finish()) return false;
    uint8_t done[8];
    protocol::store16(done, kVersion);
    protocol::store16(done + 2, kResultCount);
    protocol::store32(done + 4, request_id);
    return transport::send(protocol::Kind::done, done);
}
}  // namespace gbx::test::display_test
