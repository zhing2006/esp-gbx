#include "gbx/display.h"
#include "gbx/host.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "lvgl.h"

#include "gbx/platform.h"

namespace gbx::platform {
namespace {

constexpr const char* kTag = "host.display";
constexpr display::Size kUiSize{720, 1280};
constexpr float kAspect = static_cast<float>(kUiSize.width) / kUiSize.height;

SDL_Window* window = nullptr;
SDL_Renderer* renderer = nullptr;
SDL_Texture* texture = nullptr;
SDL_Texture* game_texture = nullptr;
std::vector<uint32_t> game_pixels;
int game_width = 0, game_height = 0;
SDL_FRect game_area{}, game_destination{};
bool game_shown = false;  // main thread
display::Scaling game_scaling = display::Scaling::nearest;
bool game_rgb565 = false;  // main thread

// The game layer's frames, as on the firmware: the producer fills one slot,
// the main thread reads another, and the third holds the newest frame
// published, traded with one atomic exchange. show_game() and hide_game()
// may run on another thread, so the main thread applies them.
constexpr uint32_t kFresh = 4;
struct Layer {
    bool shown = false;
    SDL_FRect area{};
    int width = 0;
    int height = 0;
    display::Scaling scaling = display::Scaling::nearest;
    bool rgb565 = false;
};
std::mutex layer_mutex;
Layer layer_wanted;  // guarded by layer_mutex
display::Scaling scaling_wanted = display::Scaling::nearest;  // guarded by layer_mutex
bool layer_changed = false;
std::array<std::vector<uint16_t>, 3> slots;
std::atomic<uint32_t> ready{1};
int writer = 2;  // the producer's slot
int reader = 0;  // the main thread's slot
std::atomic<bool> producing{false};
std::atomic<const uint16_t*> color_table{nullptr};  // see set_color_table()
uint32_t frame_event = 0;
lv_display_t* lv_display = nullptr;
lv_draw_buf_t* framebuffer = nullptr;
bool lvgl_started = false;
bool dirty = false;
bool failed = false;
bool quit = false;
uint64_t next_render_ms = 0;
bool initial_show_pending = false;

std::byte* assets = nullptr;
size_t assets_size = 0;
float mouse_x = 0;
float mouse_y = 0;
bool mouse_down = false;
bool touch_pending = false;
uint16_t held_buttons = 0, pressed_buttons = 0;

bool create_game_texture();

bool sdl_error(const char* operation) {
    log(LogLevel::error, kTag, "%s: %s", operation, SDL_GetError());
    failed = true;
    return false;
}

void release_touch() {
    mouse_down = false;
    touch_pending = false;
    SDL_CaptureMouse(false);
}

uint16_t key_button(SDL_Scancode key) {
    switch (key) {
        case SDL_SCANCODE_RIGHT: return 1;
        case SDL_SCANCODE_LEFT: return 2;
        case SDL_SCANCODE_UP: return 4;
        case SDL_SCANCODE_DOWN: return 8;
        case SDL_SCANCODE_Z: return 16;
        case SDL_SCANCODE_X: return 32;
        case SDL_SCANCODE_RSHIFT: return 64;
        case SDL_SCANCODE_RETURN: return 128;
        case SDL_SCANCODE_A: return keys::kL;
        case SDL_SCANCODE_S: return keys::kR;
        case SDL_SCANCODE_ESCAPE: return keys::kMenu;
        default: return 0;
    }
}

// Applies show_game() and hide_game(), and uploads the newest frame.
bool update_game() {
    Layer layer;
    bool changed = false;
    {
        std::lock_guard lock(layer_mutex);
        changed = layer_changed;
        layer_changed = false;
        layer = layer_wanted;
    }
    if (changed) {
        game_shown = layer.shown;
        dirty = true;
        if (!layer.shown) {
            SDL_DestroyTexture(game_texture);
            game_texture = nullptr;
            return true;
        }
        if (layer.width != game_width || layer.height != game_height) {
            SDL_DestroyTexture(game_texture);
            game_texture = nullptr;
            game_width = layer.width;
            game_height = layer.height;
        }
        game_scaling = layer.scaling;
        game_rgb565 = layer.rgb565;
        game_area = layer.area;
        const float scale = std::max(1.0f, std::floor(std::min(game_area.w / game_width, game_area.h / game_height)));
        game_destination = {game_area.x + (game_area.w - game_width * scale) / 2,
                            game_area.y + (game_area.h - game_height * scale) / 2, game_width * scale,
                            game_height * scale};
        // The window is black until the first frame arrives.
        game_pixels.assign(static_cast<size_t>(game_width) * game_height, 0xff000000u);
        if (!game_texture) {
            if (!create_game_texture()) return false;
        } else if (!SDL_SetTextureScaleMode(game_texture, game_scaling == display::Scaling::smooth
                                                              ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST)) {
            return sdl_error("set game sampling");
        }
    }
    if (!game_shown || !(ready.load(std::memory_order_acquire) & kFresh)) return true;
    reader = static_cast<int>(ready.exchange(reader, std::memory_order_acq_rel) & 3);
    // Use a native renderer format instead of SDL's generic 15-bit conversion.
    // Keep the converted frame for renderer resets.
    const auto& frame = slots[reader];
    const uint16_t* table = color_table.load(std::memory_order_acquire);
    for (size_t i = 0; i < game_pixels.size(); ++i) {
        const auto color = frame[i];
        if (game_rgb565) {
            const unsigned r = color >> 11, g = (color >> 5) & 63, b = color & 31;
            game_pixels[i] = 0xff000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) |
                             ((b << 3) | (b >> 2));
            continue;
        }
        if (table) {
            const unsigned c = table[color & 0x7fff], r = c >> 11, g = (c >> 5) & 63, b = c & 31;
            game_pixels[i] = 0xff000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) |
                             ((b << 3) | (b >> 2));
            continue;
        }
        const unsigned r = color & 31, g = (color >> 5) & 31, b = (color >> 10) & 31;
        game_pixels[i] = 0xff000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) |
                         ((b << 3) | (b >> 2));
    }
    if (!SDL_UpdateTexture(game_texture, nullptr, game_pixels.data(), game_width * sizeof(uint32_t))) {
        return sdl_error("upload game pixels");
    }
    dirty = true;
    return true;
}

bool create_game_texture() {
    game_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                     game_width, game_height);
    if (!game_texture || !SDL_SetTextureBlendMode(game_texture, SDL_BLENDMODE_NONE) ||
        !SDL_SetTextureScaleMode(game_texture, game_scaling == display::Scaling::smooth
                                                  ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST) ||
        !SDL_UpdateTexture(game_texture, nullptr, game_pixels.data(), game_width * sizeof(uint32_t))) {
        return sdl_error("create game texture");
    }
    return true;
}

// Leave room for window decorations, and use exact 9:16 content dimensions.
SDL_Point fitted_size(SDL_DisplayID monitor) {
    SDL_Rect bounds{};
    if (!SDL_GetDisplayUsableBounds(monitor, &bounds)) {
        return {360, 640};
    }
    int unit = std::clamp(std::min((bounds.w - 64) / 9, (bounds.h - 64) / 16), 1, 80);
    return {unit * 9, unit * 16};
}

bool create_texture() {
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                                kUiSize.width, kUiSize.height);
    if (!texture || !SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE) ||
        !SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR)) {
        return sdl_error("create UI texture");
    }
    return true;
}

void flush(lv_display_t* disp, const lv_area_t* area, uint8_t* pixels) {
    // DIRECT mode passes the full framebuffer, not an area-sized tile.
    const int stride = static_cast<int>(framebuffer->header.stride);
    SDL_Rect rect{area->x1, area->y1, lv_area_get_width(area), lv_area_get_height(area)};
    const uint8_t* start = pixels + rect.y * stride + rect.x * sizeof(uint16_t);
    if (!failed && !SDL_UpdateTexture(texture, &rect, start, stride)) {
        sdl_error("upload UI pixels");
    }
    dirty = true;
    lv_display_flush_ready(disp);
}

void handle_event(const SDL_Event& event) {
    switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            quit = true;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.which != SDL_TOUCH_MOUSEID && event.button.button == SDL_BUTTON_LEFT) {
                mouse_x = event.button.x;
                mouse_y = event.button.y;
                bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                touch_pending = down != mouse_down;
                mouse_down = down;
                // Capture keeps button-up reports coming when dragging outside the window.
                SDL_CaptureMouse(mouse_down);
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (event.motion.which != SDL_TOUCH_MOUSEID) {
                mouse_x = event.motion.x;
                mouse_y = event.motion.y;
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_HIDDEN:
            release_touch();
            held_buttons = pressed_buttons = 0;
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            const uint16_t button = key_button(event.key.scancode);
            if (event.type == SDL_EVENT_KEY_DOWN) {
                if (!event.key.repeat) pressed_buttons |= button & ~held_buttons;
                held_buttons |= button;
            } else held_buttons &= ~button;
            break;
        }
        case SDL_EVENT_WINDOW_MAXIMIZED: {
            // Native maximization may ignore aspect constraints. Fit a normal
            // window to the current desktop instead of stretching the UI.
            SDL_Point size = fitted_size(SDL_GetDisplayForWindow(window));
            if (!SDL_RestoreWindow(window) || !SDL_SetWindowSize(window, size.x, size.y) ||
                !SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED)) {
                sdl_error("fit maximized window");
            }
            dirty = true;
            break;
        }
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESTORED:
            dirty = true;
            break;
        case SDL_EVENT_RENDER_DEVICE_LOST:
            log(LogLevel::error, kTag, "rendering device lost");
            failed = true;
            break;
        case SDL_EVENT_RENDER_DEVICE_RESET:
            SDL_DestroyTexture(texture);
            texture = nullptr;
            if (create_texture() && !SDL_UpdateTexture(texture, nullptr, framebuffer->data,
                                                      static_cast<int>(framebuffer->header.stride))) {
                sdl_error("restore UI texture");
            }
            if (game_texture) {
                SDL_DestroyTexture(game_texture);
                game_texture = nullptr;
                create_game_texture();
            }
            dirty = true;
            break;
        default:
            break;
    }
}

}  // namespace

namespace display {

bool start() {
    SDL_SetMainReady();
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return sdl_error("initialize SDL video");
    }
    SDL_Point size = fitted_size(SDL_GetPrimaryDisplay());
    window = SDL_CreateWindow("esp-gbx", size.x, size.y,
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (!window || !SDL_SetWindowAspectRatio(window, kAspect, kAspect)) {
        return sdl_error("create window");
    }
    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer || !SDL_SetRenderLogicalPresentation(renderer, kUiSize.width, kUiSize.height,
                                                       SDL_LOGICAL_PRESENTATION_LETTERBOX) ||
        !SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255) || !create_texture()) {
        return sdl_error("initialize renderer");
    }

    frame_event = SDL_RegisterEvents(1);
    if (frame_event == 0) {
        return sdl_error("register the game frame event");
    }
    lv_init();
    lvgl_started = true;
    lv_tick_set_cb([]() -> uint32_t { return static_cast<uint32_t>(SDL_GetTicks()); });
    lv_display = lv_display_create(kUiSize.width, kUiSize.height);
    framebuffer = lv_draw_buf_create(kUiSize.width, kUiSize.height, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    if (!lv_display || !framebuffer) {
        log(LogLevel::error, kTag, "could not allocate the LVGL display");
        return false;
    }
    lv_display_set_color_format(lv_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_draw_buffers(lv_display, framebuffer, nullptr);
    lv_display_set_render_mode(lv_display, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(lv_display, flush);
    log(LogLevel::info, kTag, "SDL %s, renderer %s, UI %dx%d", SDL_GetRevision(),
        SDL_GetRendererName(renderer), kUiSize.width, kUiSize.height);
    // Show only after render() has presented the first complete UI frame.
    initial_show_pending = true;
    return true;
}

Size size() {
    return lv_display ? kUiSize : Size{};
}

bool scaling_supported(Scaling mode) {
    return mode == Scaling::nearest || mode == Scaling::smooth;
}

Scaling default_scaling() {
    return Scaling::smooth;
}

void set_scaling(Scaling mode) {
    std::lock_guard lock(layer_mutex);
    scaling_wanted = scaling_supported(mode) ? mode : Scaling::nearest;
}

bool show_game(std::span<const int16_t, 4> rect, int width, int height, PixelFormat format) {
    if (width <= 0 || height <= 0 || format == PixelFormat::shades) return false;
    std::lock_guard lock(layer_mutex);
    const size_t pixels = static_cast<size_t>(width) * height;
    if (slots[0].size() < pixels) {
        // As on the firmware, slots grow only while nothing produces.
        if (producing.load()) return false;
        for (auto& slot : slots) slot.assign(pixels, 0);
    }
    ready.fetch_and(~kFresh);
    layer_wanted = {true,
                    {static_cast<float>(rect[0]), static_cast<float>(rect[1]), static_cast<float>(rect[2]),
                     static_cast<float>(rect[3])},
                    width,
                    height,
                    scaling_wanted,
                    format == PixelFormat::rgb565};
    layer_changed = true;
    producing = true;
    SDL_Event event{};
    event.type = frame_event;
    SDL_PushEvent(&event);
    return true;
}

void hide_game() {
    producing = false;
    {
        std::lock_guard lock(layer_mutex);
        layer_wanted.shown = false;
        layer_changed = true;
    }
    SDL_Event event{};
    event.type = frame_event;
    SDL_PushEvent(&event);
}

// SDL takes the frames in color.
bool game_shades_supported() {
    return false;
}

void set_game_shades(std::span<const uint16_t, 4>) {}

uint16_t* game_frame_buffer() {
    return producing.load(std::memory_order_acquire) ? slots[writer].data() : nullptr;
}

bool game_frame_due() {
    return true;
}

void publish_game_frame(uint64_t) {
    writer = static_cast<int>(ready.exchange(writer | kFresh, std::memory_order_acq_rel) & 3);
    SDL_Event event{};
    event.type = frame_event;
    SDL_PushEvent(&event);
}

// SDL presents without telling when the frame reaches the screen.
int64_t game_frames_shown() {
    return -1;
}

void set_color_table(const uint16_t* table) {
    color_table.store(table, std::memory_order_release);
}

// SDL scales the frames.
void lend_emulator_core(bool) {}

// The window's brightness is the monitor's.
bool brightness_supported() {
    return false;
}

void set_brightness(int) {}

bool lock(uint32_t) {
    // All host LVGL calls run on the main thread, so no mutex is needed.
    return lv_display && SDL_IsMainThread();
}

void unlock() {}

}  // namespace display

namespace touch {

bool start() {
    return window != nullptr;
}

int read(std::span<Point> points) {
    touch_pending = false;
    if (!mouse_down || points.empty()) {
        return 0;
    }
    float x = 0;
    float y = 0;
    // SDL accounts for window coordinates, display pixel density and scaling.
    if (!SDL_RenderCoordinatesFromWindow(renderer, mouse_x, mouse_y, &x, &y)) {
        return -1;
    }
    if (x < 0 || y < 0 || x >= kUiSize.width || y >= kUiSize.height) {
        return 0;
    }
    points[0] = {x, y};
    return 1;
}

}  // namespace touch

namespace keys {

uint16_t read() {
    const uint16_t buttons = held_buttons | pressed_buttons;
    pressed_buttons = 0;
    return buttons;
}

}  // namespace keys

std::span<const std::byte> asset_pack() {
    if (!assets) {
        const char* base = SDL_GetBasePath();
        if (!base) {
            sdl_error("locate asset pack");
            return {};
        }
        std::string path = std::string(base) + "assets.bin";
        assets = static_cast<std::byte*>(SDL_LoadFile(path.c_str(), &assets_size));
        if (!assets) {
            sdl_error("load assets.bin next to the executable");
        }
    }
    return {assets, assets_size};
}

namespace host {

bool poll_events(uint32_t max_wait_ms) {
    uint64_t now = SDL_GetTicks();
    uint64_t render_wait = next_render_ms > now ? next_render_ms - now : 0;
    uint32_t wait = static_cast<uint32_t>(std::min<uint64_t>(max_wait_ms, render_wait));
    if (touch_pending) {
        // Leave later transitions in SDL's queue until the app has observed
        // this one. In particular, a queued button-up must not erase a click.
        SDL_Delay(wait);
        SDL_PumpEvents();
        return !quit;
    }
    SDL_Event event;
    if (SDL_WaitEventTimeout(&event, static_cast<int>(wait))) {
        do {
            handle_event(event);
        } while (!touch_pending && !quit && SDL_PollEvent(&event));
    }
    return !quit;
}

bool render() {
    if (failed) {
        return false;
    }
    if (!update_game()) {
        return false;
    }
    uint32_t delay = lv_timer_handler();
    next_render_ms = SDL_GetTicks() + delay;
    if (failed) {
        return false;
    }
    if (dirty) {
        if (!draw_frame()) return false;
        if (!SDL_RenderPresent(renderer)) return sdl_error("present frame");
        dirty = false;
        if (initial_show_pending) {
            if (!SDL_ShowWindow(window)) {
                return sdl_error("show window");
            }
            initial_show_pending = false;
        }
    }
    return true;
}

bool draw_frame() {
    if (failed) return false;
    if (!SDL_RenderClear(renderer) || !SDL_RenderTexture(renderer, texture, nullptr, nullptr)) {
        return sdl_error("render UI");
    }
    if (game_shown && game_texture && (!SDL_RenderFillRect(renderer, &game_area) ||
                         !SDL_RenderTexture(renderer, game_texture, nullptr, &game_destination))) {
        return sdl_error("render game");
    }
    return true;
}

void show_game_error() {
    if (!SDL_SetWindowTitle(window, "esp-gbx - emulation stopped (see console)")) {
        log(LogLevel::error, kTag, "set error title: %s", SDL_GetError());
    }
}

void shutdown() {
    // app::shutdown() has already deleted all page objects and timers.
    if (lv_display) {
        lv_display_delete(lv_display);
        lv_display = nullptr;
    }
    if (framebuffer) {
        lv_draw_buf_destroy(framebuffer);
        framebuffer = nullptr;
    }
    if (lvgl_started) {
        lv_deinit();
        lvgl_started = false;
    }
    SDL_DestroyTexture(texture);
    SDL_DestroyTexture(game_texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    texture = nullptr;
    game_texture = nullptr;
    std::vector<uint32_t>().swap(game_pixels);
    game_width = game_height = 0;
    game_area = game_destination = {};
    game_shown = false;
    game_scaling = display::Scaling::nearest;
    producing = false;
    scaling_wanted = display::Scaling::nearest;
    layer_wanted = {};
    layer_changed = false;
    for (auto& slot : slots) std::vector<uint16_t>().swap(slot);
    ready = 1;
    writer = 2;
    reader = 0;
    renderer = nullptr;
    window = nullptr;
    SDL_free(assets);
    assets = nullptr;
    assets_size = 0;
    SDL_Quit();
    dirty = failed = quit = mouse_down = touch_pending = initial_show_pending = false;
    next_render_ms = 0;
    held_buttons = pressed_buttons = 0;
}

}  // namespace host
}  // namespace gbx::platform
