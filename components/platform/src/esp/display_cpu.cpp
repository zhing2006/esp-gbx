#include "display_backend.h"
#include "display_pixels.h"

#include <algorithm>

namespace gbx::platform::display::detail {

bool backend_start() { return true; }
void backend_stop() {}

bool game_size_supported(int, int) { return true; }
#ifdef GBX_PROFILER
size_t backend_buffer_bytes() { return 0; }
#endif

// Copies one rectangle of the shadow into a frame buffer in panel orientation.
void copy_ui_rect(uint16_t* fb, const Config& config, const Rect& r) { put_rect(fb, config, r); }

void fill_black(uint16_t* fb, const Config& config, const Rect& r) {
    static const uint16_t black[kMaxLine] = {};
    for (int y = r.y0; y < r.y1; ++y) {
        for (int x = r.x0; x < r.x1; x += kMaxLine) {
            put_row(fb, config, x, y, black, std::min(r.x1 - x, kMaxLine));
        }
    }
}

void draw_game(uint16_t* fb, const Config& config, const GameLayer& layer, const uint16_t* frame) {
    draw_nearest(fb, config, layer, frame);
}

}  // namespace gbx::platform::display::detail

#ifdef GBX_PROFILER
namespace gbx::platform::display {
HardwareTimes take_hardware_times() { return {}; }
}  // namespace gbx::platform::display
#endif
