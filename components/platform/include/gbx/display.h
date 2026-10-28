#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// The screen, LVGL and the asset pack. Coordinates are logical UI pixels in
// the orientation the UI is designed for; the platform turns them to fit the
// panel.
namespace gbx::platform::display {

struct Size {
    int width;
    int height;
};

// Starts the display and LVGL. The firmware creates LVGL and present tasks;
// the host drives both on its main thread. Returns false on startup failure.
bool start();

// Logical UI size; zero before start() succeeds.
Size size();

// Hold this lock while touching LVGL objects outside its task. On the host,
// callers must be on the main thread. Returns false if the lock is not held.
bool lock(uint32_t timeout_ms);
void unlock();

enum class Scaling : uint8_t { nearest, smooth };
bool scaling_supported(Scaling mode);
Scaling default_scaling();
// Sets the requested mode for the next show_game(), including resume. It
// does not change a running layer or reset emulation. An unavailable mode
// falls back to nearest.
void set_scaling(Scaling mode);

// The game layer: game frames go straight into the game window, outside
// LVGL. Each new frame is scaled by the largest whole factor that fits the
// window, using the selected sampling mode, and centered; the rest of the
// window is black. The UI is not drawn in the window while the layer shows.

// The pixels of game frames: BGR555; RGB565 as the panel takes them, which
// the display shows without converting them and without the color table;
// or a GB's shades, a byte a pixel from 0, the lightest, to 3, shown in the
// colors of set_game_shades(), where game_shades_supported() says so.
enum class PixelFormat : uint8_t { bgr555, rgb565, shades };

// Shows the layer in `window`, logical UI pixels {x, y, width, height}, for
// frames of width x height pixels, at most the GBA's 240 x 160, or the GB's
// 160 x 144 in a build without the GBA. The window stays black until the
// first frame is published. Call from the task that drives the app.
bool show_game(std::span<const int16_t, 4> window, int width, int height,
               PixelFormat format = PixelFormat::bgr555);
void hide_game();
// Whether show_game() takes PixelFormat::shades: on an SPI panel whose
// present task converts the frame as it sends it, where a GB's frame then
// needs no conversion to colors before it is published.
bool game_shades_supported();
// The BGR555 colors of shades 0 to 3, which go through the color table as
// a BGR555 frame's pixels do. Frames shown from now on use them. Any task.
void set_game_shades(std::span<const uint16_t, 4> colors);

// Producer side, one task at a time while the layer shows: fill the buffer,
// a byte a pixel for shades, then publish it. The buffer is the producer's
// until publish_game_frame(); the layer shows the newest frame published and
// skips older ones. A frame may repeat an id, as a blank screen does.
uint16_t* game_frame_buffer();
void publish_game_frame(uint64_t id);
// Whether the layer would take a frame published in_us from now, after
// the one published and not taken yet, if any. A display that shows only
// some frames says no to the others, so the producer need not draw them.
// Producer side.
bool game_frame_due(uint64_t in_us);

// Frames with a new id that became the screen's scanout source since
// show_game(), or -1 where the display cannot tell when a buffer is shown.
int64_t game_frames_shown();

// The colors of the game frames: 32768 RGB565 colors indexed by a frame's
// BGR555 pixel, such as an LCD color correction, or nullptr to show the
// pixels as they are. The table stays valid while set; frames drawn from
// now on use it. Any task.
void set_color_table(const uint16_t* table);

// Whether the emulator's core has time to spare for the display: the
// firmware then converts each game frame on that core, below the
// emulation's priority, before the hardware scales it. Off at start; the
// game decides, as it knows the backend and speed. Nothing changes where
// there is one core or no hardware scaling. Any task.
void lend_emulator_core(bool lend);

// Backlight brightness, 0 to 100 percent. The backlight comes on with the
// first frame shown, at the brightness set by then. False where it cannot be
// changed, as on the PC; set_brightness() then does nothing. Any task.
bool brightness_supported();
void set_brightness(int percent);

#ifdef GBX_PROFILER
// Totals since the previous call. Wall times in microseconds include
// preemption. present_us covers a pass up to submission, not scanout waiting;
// ui_us excludes the LVGL lock wait. render_us is LVGL's render event interval,
// not the menu's CPU rasterization. These stages may overlap on two tasks.
struct PresentTimes {
    uint32_t ui_us, game_us, flush_us, loops, games;
    uint32_t lock_us, fill_us, present_us, scanout_us;
    uint32_t render_us, renders, rects, pixels;
    size_t buffer_bytes;
};
PresentTimes take_present_times();

// Completion includes submission, cache maintenance and waiting for the
// hardware callback to wake the presenting task. These are not CPU times.
struct HardwareOperationTimes { uint32_t submit_us, complete_us, count, max_complete_us; };
struct HardwareTimes { HardwareOperationTimes copy, rotate, fill, scale, rows; };
HardwareTimes take_hardware_times();

// Frame handoff diagnostics. Buckets are <5, 5-12, 12-20, >=20 ms;
// scan_cycles uses 0, 1, 2, >=3 framebuffer-completion callbacks per busy
// present pass: one per frame sent on a DPI panel, one per buffer switch on
// an RGB panel. These callbacks are not an external panel measurement.
// Task-state columns are running, ready, blocked, other. State and stage are
// sampled at overwrite time; they describe correlation, not the cause.
enum class PresentStage : uint8_t {
    notify, dispatch, lvgl_lock, ui_copy, fill, nearest, convert, ppa_submit,
    ppa_wait, rows_wait, submit, scanout, finish, count
};
struct FrameFlow {
    uint32_t published, overwritten, taken, scans;
    uint32_t publish_interval[4], take_age[4], overwrite_age[4], scan_cycles[4];
    uint32_t overwrites[static_cast<size_t>(PresentStage::count)][4];
    uint32_t lock_max_us, busy_max_us;
};
FrameFlow take_frame_flow();
#endif

}  // namespace gbx::platform::display

namespace gbx::platform::touch {

struct Point {
    float x;
    float y;
};

// Starts the touch controller. Returns false if there is none.
bool start();

// Reads the current touch points, in logical UI pixels. Returns how many
// points were written to `points`, or -1 if the controller could not be read.
int read(std::span<Point> points);

}  // namespace gbx::platform::touch

namespace gbx::platform::keys {

// Keys held on physical controls, such as a PC keyboard: the GB keys from bit
// 0, Right, Left, Up, Down, A, B, Select, Start, then the shell's L, R and
// MENU in bits 8 to 10. A key pressed and released since the previous read
// counts as held once. 0 without keys.
inline constexpr uint16_t kL = 1 << 8, kR = 1 << 9, kMenu = 1 << 10;
uint16_t read();

}  // namespace gbx::platform::keys

namespace gbx::platform {

// The UI asset pack, read in place. On the firmware this is the memory-mapped
// assets partition. Empty if there is none.
std::span<const std::byte> asset_pack();

}  // namespace gbx::platform
