#pragma once

#include "gbx/display.h"
#include "gbx/esp/board.h"

namespace gbx::platform::display::detail {

constexpr int kMaxLine = 1024;  // the widest scaled game frame

// Logical UI pixels, end exclusive.
struct Rect {
    int x0;
    int y0;
    int x1;
    int y1;

    bool contains(const Rect& r) const {
        return x0 <= r.x0 && y0 <= r.y0 && x1 >= r.x1 && y1 >= r.y1;
    }
    bool operator==(const Rect&) const = default;
};

// Both lists read the latest shadow, so an enclosing rectangle covers every
// contained update regardless of its age. Keep the first equal rectangle;
// leave partial overlaps separate rather than copying unrelated pixels.
template <typename Apply>
void for_each_damage_rect(std::span<const Rect> carry, std::span<const Rect> now, Apply&& apply) {
    const size_t count = carry.size() + now.size();
    const auto at = [&](size_t i) -> const Rect& {
        return i < carry.size() ? carry[i] : now[i - carry.size()];
    };
    for (size_t i = 0; i < count; ++i) {
        const auto& r = at(i);
        bool covered = false;
        for (size_t j = 0; j < count; ++j) {
            if (j == i) continue;
            const auto& other = at(j);
            if (other.contains(r) && (j < i || other != r)) {
                covered = true;
                break;
            }
        }
        if (!covered) apply(r);
    }
}

struct GameLayer {
    bool shown = false;
    Rect window{};  // logical UI pixels
    Rect image{};   // the scaled frame, centered in the window
    int width = 0;
    int height = 0;
    int scale = 1;
    Scaling scaling = Scaling::nearest;
    const uint16_t* colors = nullptr;  // see set_color_table()
    bool rgb565 = false;               // see PixelFormat
};

struct Config {
    board::Display panel;
    Size ui;
    uint16_t* shadow;
};

// One owner uses the backend at a time. Stop only after its operations return.
bool backend_start();
void backend_stop();
#if CONFIG_LV_USE_PPA
bool configure_lvgl_acceleration();
#endif
#ifdef GBX_PROFILER
size_t backend_buffer_bytes();
void set_present_stage(PresentStage stage);
#endif
bool game_size_supported(int width, int height);
void copy_ui_rect(uint16_t* fb, const Config& config, const Rect& r);
#if CONFIG_GBX_DISPLAY_ACCEL
// Hardware submission; copy_ui_rect selects CPU processing for small regions.
void copy_ui_rect_accelerated(uint16_t* fb, const Config& config, const Rect& r);
#endif
void fill_black(uint16_t* fb, const Config& config, const Rect& r);
void draw_game(uint16_t* fb, const Config& config, const GameLayer& layer, const uint16_t* frame);
// For draw_game(): runs step(arg) to completion, on the emulator's core
// while it is lent, see lend_emulator_core(), otherwise on the caller.
void run_frame_step(void (*step)(void*), void* arg);

}  // namespace gbx::platform::display::detail
