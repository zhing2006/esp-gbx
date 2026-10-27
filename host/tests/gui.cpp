#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include <SDL3/SDL.h>
#include "gbx/app.h"
#include "gbx/display.h"
#include "gbx/files.h"
#include "gbx/host.h"
#include "gbx/platform.h"
#include "gbx/ui/pack.h"
#include "lvgl.h"
#include "../../components/app/src/settings.h"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s (%s)\n", message, SDL_GetError());
        // The game's tasks are still running; exit() would wait for them.
        std::_Exit(1);
    }
}

void run_for(uint32_t ms) {
    uint64_t end = SDL_GetTicks() + ms;
    do {
        require(gbx::platform::host::poll_events(gbx::app::update_delay_ms()), "window remains open");
        gbx::app::update();
        require(gbx::platform::host::render(), "render succeeds");
    } while (SDL_GetTicks() < end);
}

// Delivers events without app::update(), which would take the keys before
// the test reads them.
void pump_events(uint32_t ms) {
    uint64_t end = SDL_GetTicks() + ms;
    do {
        require(gbx::platform::host::poll_events(1), "window remains open");
    } while (SDL_GetTicks() < end);
}

const gbx::ui::pack_format::Control& control(const gbx::ui::Pack& pack, std::string_view id) {
    for (const auto& item : pack.controls()) {
        if (gbx::ui::field(item.id) == id) {
            return item;
        }
    }
    require(false, "control exists in the pack");
    std::abort();
}

// Expected pixels come from the shipped source assets, not a captured golden frame.
// Opaque pixels must be copied exactly; alpha edges are left to LVGL's blend rules.
void expect_image(const gbx::ui::Pack& pack, const char* name) {
    auto image = pack.image(name);
    require(image.has_value(), name);
    auto* buffer = lv_display_get_buf_active(lv_display_get_default());
    size_t checked = 0;
    for (int y = 0; y < image->height; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(buffer->data + (image->y + y) * buffer->header.stride);
        for (int x = 0; x < image->width; ++x) {
            if (!image->alpha || image->alpha[y * image->width + x] == 255) {
                require(row[image->x + x] == image->rgb[y * (image->stride / 2) + x], name);
                ++checked;
            }
        }
    }
    require(checked > 0, "sprite has opaque pixels");
}

void mouse(SDL_Window* window, SDL_EventType type, float x, float y, bool settle = true) {
    float wx = 0;
    float wy = 0;
    require(SDL_RenderCoordinatesToWindow(SDL_GetRenderer(window), x, y, &wx, &wy), "map logical input");
    SDL_Event event{};
    event.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) {
        event.motion.windowID = SDL_GetWindowID(window);
        event.motion.x = wx;
        event.motion.y = wy;
    } else {
        event.button.windowID = SDL_GetWindowID(window);
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.x = wx;
        event.button.y = wy;
    }
    require(SDL_PushEvent(&event), "push mouse event");
    if (settle) {
        run_for(70);
    }
}

void window_event(SDL_Window* window, SDL_EventType type) {
    SDL_Event event{};
    event.type = type;
    event.window.windowID = SDL_GetWindowID(window);
    require(SDL_PushEvent(&event), "push window event");
}

void key(SDL_Window* window, SDL_Scancode code, bool down, bool repeat = false) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.scancode = code;
    event.key.down = down;
    event.key.repeat = repeat;
    require(SDL_PushEvent(&event), "push keyboard event");
}

void press_key(SDL_Window* window, SDL_Scancode code) {
    key(window, code, true);
    run_for(30);
    key(window, code, false);
    run_for(30);
}

std::string settings_text() {
    char path[64];
    gbx::app::settings_path(path, sizeof path);
    std::array<uint8_t, 1024> text;
    const auto n = gbx::platform::files::read_start(path, text);
    require(n > 0 && n <= static_cast<int64_t>(text.size()), "read saved settings");
    return {reinterpret_cast<const char*>(text.data()), static_cast<size_t>(n)};
}

void wait_for_setting(std::string_view value) {
    const auto until = SDL_GetTicks() + 2000;
    while (SDL_GetTicks() < until) {
        run_for(10);
        if (settings_text().find(value) != std::string::npos) return;
    }
    require(false, "settings writer persists the requested value");
}

void check_menu(const gbx::ui::Pack& pack);

void check_settings_input(SDL_Window* window, const gbx::ui::Pack& pack) {
    press_key(window, SDL_SCANCODE_X);  // the preceding A-button tests opened ROMS
    for (int i = 0; i < 3; ++i) press_key(window, SDL_SCANCODE_UP);
    press_key(window, SDL_SCANCODE_DOWN);
    press_key(window, SDL_SCANCODE_Z);  // HOME -> SETTINGS, on the row left last
    for (int i = 0; i < 5; ++i) press_key(window, SDL_SCANCODE_UP);
    press_key(window, SDL_SCANCODE_RIGHT);  // volume only
    check_menu(pack);  // includes the narrow, partially redrawn volume region
    press_key(window, SDL_SCANCODE_X);
    wait_for_setting("volume=70\n");
    require(settings_text().find("scaling=") == std::string::npos,
            "editing another setting leaves the scaling default unpinned");

    press_key(window, SDL_SCANCODE_Z);
    for (int i = 0; i < 5; ++i) press_key(window, SDL_SCANCODE_DOWN);
    press_key(window, SDL_SCANCODE_RIGHT);  // explicitly save the default SMOOTH choice
    press_key(window, SDL_SCANCODE_X);
    wait_for_setting("scaling=smooth\n");

    press_key(window, SDL_SCANCODE_Z);
    const auto& area = pack.layout().display;
    const int scale = pack.menu().scale;
    // NEAREST's pill on the sixth row; the same hit map is used by all boards.
    const float x = static_cast<float>(area[0] + 140 * scale);
    const float y = static_cast<float>(area[1] + 128 * scale);
    mouse(window, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y);
    mouse(window, SDL_EVENT_MOUSE_BUTTON_UP, x, y);
    press_key(window, SDL_SCANCODE_X);
    wait_for_setting("scaling=nearest\n");
    press_key(window, SDL_SCANCODE_UP);  // restore HOME's ROMS selection
}

void check_keyboard(SDL_Window* window) {
    // GB keys in their bit order, then L, R and MENU in bits 8 to 10.
    constexpr SDL_Scancode keys[]{SDL_SCANCODE_RIGHT, SDL_SCANCODE_LEFT,   SDL_SCANCODE_UP,     SDL_SCANCODE_DOWN,
                                  SDL_SCANCODE_Z,     SDL_SCANCODE_X,      SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RETURN,
                                  SDL_SCANCODE_A,     SDL_SCANCODE_S,      SDL_SCANCODE_ESCAPE};
    for (unsigned i = 0; i < std::size(keys); ++i) {
        key(window, keys[i], true);
        pump_events(20);
        require(gbx::platform::keys::read() == (1u << i), "GB key mapping");
        require(gbx::platform::keys::read() == (1u << i), "held key survives sampling");
        key(window, keys[i], false);
        pump_events(20);
        require(gbx::platform::keys::read() == 0, "key release");
    }
    key(window, SDL_SCANCODE_Z, true);
    key(window, SDL_SCANCODE_Z, false);
    pump_events(20);
    require(gbx::platform::keys::read() == 16, "short key tap survives event batch");
    require(gbx::platform::keys::read() == 0, "short key tap releases after sampling");

    key(window, SDL_SCANCODE_RIGHT, true);
    key(window, SDL_SCANCODE_Z, true);
    key(window, SDL_SCANCODE_Z, true, true);
    pump_events(20);
    require(gbx::platform::keys::read() == 17, "simultaneous direction and action");
    window_event(window, SDL_EVENT_WINDOW_FOCUS_LOST);
    pump_events(20);
    require(gbx::platform::keys::read() == 0, "focus loss releases keyboard");
}

// Without a game the menus fill the game window. The middle of their title
// bar, between the title and the status, has the theme's header color from
// the pack, as RGB565.
void check_menu(const gbx::ui::Pack& pack) {
    const auto& menu = pack.menu();
    const auto header = pack.menu_colors()[static_cast<int>(gbx::ui::pack_format::MenuColor::header)];
    const auto color = static_cast<uint16_t>(((header.r * 31 + 127) / 255) << 11 | ((header.g * 63 + 127) / 255) << 5 |
                                             (header.b * 31 + 127) / 255);
    auto* buffer = lv_display_get_buf_active(lv_display_get_default());
    const auto& area = pack.layout().display;
    const int x = area[0] + area[2] / 2;
    for (int y = area[1]; y < area[1] + 16 * menu.scale; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(buffer->data + y * buffer->header.stride);
        require(row[x] == color && row[x + 1] == color, "game window shows the menu's title bar");
    }
    // The menu's integer expansion must replicate each logical pixel in X
    // and Y. This is a spatial invariant, not a captured golden image.
    for (int y0 = area[1]; y0 < area[1] + area[3]; y0 += menu.scale) {
        const auto* top = reinterpret_cast<const uint16_t*>(buffer->data + y0 * buffer->header.stride);
        for (int x0 = area[0]; x0 < area[0] + area[2]; x0 += menu.scale) {
            for (int dy = 0; dy < menu.scale; ++dy) {
                const auto* row = reinterpret_cast<const uint16_t*>(buffer->data + (y0 + dy) * buffer->header.stride);
                for (int dx = 0; dx < menu.scale; ++dx)
                    require(row[x0 + dx] == top[x0], "menu pixels repeat exactly inside each scaled block");
            }
        }
    }
}

void check_game_layer(SDL_Window* window, const gbx::ui::Pack& pack, int width, int height, bool smooth = false) {
    auto* renderer = SDL_GetRenderer(window);
    auto* target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, width, height);
    require(target && SDL_SetRenderTarget(renderer, target), "create readback target");
    require(SDL_SetRenderLogicalPresentation(renderer, 720, 1280, SDL_LOGICAL_PRESENTATION_LETTERBOX), "scale readback target");
    require(gbx::platform::host::draw_frame(), "draw actual game composition");
    auto* image = SDL_RenderReadPixels(renderer, nullptr);
    require(image, "read composition before present");

    const auto& area = pack.layout().display;
    // GB frames take the largest whole scale that fits the window, centered,
    // with black on every side.
    const int scale = std::min(area[2] / 160, area[3] / 144);
    require(scale >= 1 && scale * 160 < area[2] && scale * 144 < area[3], "game window has room for borders");
    const double left = area[0] + (area[2] - scale * 160) / 2.0, right = left + scale * 160;
    const double top = area[1] + (area[3] - scale * 144) / 2.0, bottom = top + scale * 144;
    const double center_x = area[0] + area[2] / 2.0;
    const double center_y = area[1] + area[3] / 2.0;
    size_t game_checked = 0, black_checked = 0, mixed_checked = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const double lx = (x + 0.5) * 720 / width;
            const double ly = (y + 0.5) * 1280 / height;
            if (lx <= area[0] + 2 || lx >= area[0] + area[2] - 2 ||
                ly <= area[1] + 2 || ly >= area[1] + area[3] - 2) continue;
            Uint8 r, g, b, a;
            require(SDL_ReadSurfacePixel(image, x, y, &r, &g, &b, &a), "read composed pixel");
            if (lx < left - 2 || lx > right + 2 || ly < top - 2 || ly > bottom + 2) {
                require(r == 0 && g == 0 && b == 0, "unused game area is black");
                ++black_checked;
            } else if (lx > left + 2 && lx < right - 2 && ly > top + 2 && ly < bottom - 2) {
                const bool mixed = (r != 0 && r != 255) || (g != 0 && g != 255) || (b != 0 && b != 255);
                if (smooth) mixed_checked += mixed;
                else require(!mixed, "nearest sampling never blends quadrant edges");
                if (std::abs(lx - center_x) < 2 || std::abs(ly - center_y) < 2) continue;
                const bool right_half = lx > center_x, bottom_half = ly > center_y;
                require(r == ((!right_half && !bottom_half) || (right_half && bottom_half) ? 255 : 0) &&
                        g == (right_half ? 255 : 0) && b == (bottom_half ? 255 : 0),
                        "game keeps its aspect, position and BGR555 colors");
                ++game_checked;
            }
        }
    }
    require(game_checked > 1000 && black_checked > 100, "readback covers game and its borders");
    if (smooth && width == 720) require(mixed_checked > 0, "smooth sampling blends quadrant edges");
    SDL_DestroySurface(image);
    require(SDL_SetRenderTarget(renderer, nullptr), "restore window target");
    SDL_DestroyTexture(target);
}

}  // namespace

int main() {
    // Never write test choices over a user's card settings.
    namespace fs = std::filesystem;
    std::error_code error;
    const auto previous = fs::current_path(error);
    require(!error, "read working directory");
    const auto card = fs::temp_directory_path(error) / ("gbx-gui-" + std::to_string(SDL_GetPerformanceCounter()));
    require(!error && fs::create_directory(card, error) && !error, "create isolated test card");
    fs::current_path(card, error);
    require(!error, "select isolated test card");
    // Repeated startup and teardown also exercises timer and image-buffer lifetimes.
    for (int cycle = 0; cycle < 2; ++cycle) {
        char settings_path[64];
        gbx::app::settings_path(settings_path, sizeof settings_path);
        constexpr std::string_view old_settings = "volume=60\nbrightness=100\ncpu=int\npalette=gray\nlast_rom=\n";
        require(gbx::platform::files::make_parents(settings_path) == gbx::platform::files::Error::none &&
                    gbx::platform::files::write(settings_path,
                        {reinterpret_cast<const uint8_t*>(old_settings.data()), old_settings.size()}) ==
                        gbx::platform::files::Error::none,
                "seed an old settings file without scaling");
        require(gbx::app::init(), "initialize app");
        gbx::ui::Pack pack;
        require(pack.open(gbx::platform::asset_pack()), "load packaged assets");
        int count = 0;
        SDL_Window** windows = SDL_GetWindows(&count);
        require(windows && count == 1, "one desktop window");
        SDL_Window* window = windows[0];
        SDL_free(windows);
        run_for(70);
        require(!(SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN), "first frame shows the window");
        check_menu(pack);
        expect_image(pack, "ctl.a.normal");
        expect_image(pack, "ctl.speed.1x");

        // Request an incompatible ratio, so this also tests the size constraint.
        // The smaller window exercises coordinate conversion without a HiDPI display.
        require(SDL_SetWindowSize(window, 400, 640), "resize window");
        require(SDL_SyncWindow(window), "wait for resize");
        run_for(70);
        int width = 0;
        int height = 0;
        require(SDL_GetWindowSize(window, &width, &height), "read window size");
        require(std::abs(width * 16 - height * 9) <= 16, "window keeps 9:16 aspect");
        if (std::string_view(SDL_GetCurrentVideoDriver()) != "dummy") {
            require(SDL_MaximizeWindow(window), "maximize window");
            run_for(250);
            require(SDL_SyncWindow(window), "wait for fitted window");
            require(SDL_GetWindowSize(window, &width, &height), "read fitted window size");
            require(std::abs(width * 16 - height * 9) <= 16, "maximization keeps 9:16 aspect");
        }

        const auto& a = control(pack, "a");
        mouse(window, SDL_EVENT_MOUSE_BUTTON_DOWN, a.cx, a.cy);
        expect_image(pack, "ctl.a.pressed");

        const auto& dpad = control(pack, "dpad");
        mouse(window, SDL_EVENT_MOUSE_MOTION, dpad.cx, dpad.cy - (dpad.a + dpad.dead) / 2);
        expect_image(pack, "ctl.a.normal");
        expect_image(pack, "ctl.dpad.up");

        window_event(window, SDL_EVENT_WINDOW_FOCUS_LOST);
        run_for(70);
        expect_image(pack, "ctl.dpad.normal");
        mouse(window, SDL_EVENT_MOUSE_MOTION, a.cx, a.cy);
        expect_image(pack, "ctl.a.normal");

        // Release outside the content must not leave a button held.
        mouse(window, SDL_EVENT_MOUSE_BUTTON_DOWN, a.cx, a.cy);
        mouse(window, SDL_EVENT_MOUSE_BUTTON_UP, -20, -20);
        expect_image(pack, "ctl.a.normal");

        mouse(window, SDL_EVENT_MOUSE_BUTTON_DOWN, a.cx, a.cy);
        require(SDL_HideWindow(window), "hide window");
        run_for(70);
        expect_image(pack, "ctl.a.normal");
        require(SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN, "updates do not reopen a hidden window");
        require(SDL_ShowWindow(window), "show window again");
        run_for(70);

        const auto& speed = control(pack, "speed");
        mouse(window, SDL_EVENT_MOUSE_BUTTON_DOWN, speed.cx, speed.cy);
        expect_image(pack, "ctl.speed.2x");
        run_for(100);
        expect_image(pack, "ctl.speed.2x");
        mouse(window, SDL_EVENT_MOUSE_BUTTON_UP, speed.cx, speed.cy);
        expect_image(pack, "ctl.speed.2x");
        mouse(window, SDL_EVENT_MOUSE_BUTTON_DOWN, speed.cx, speed.cy);
        expect_image(pack, "ctl.speed.1x");
        mouse(window, SDL_EVENT_MOUSE_BUTTON_UP, speed.cx, speed.cy);
        mouse(window, SDL_EVENT_MOUSE_BUTTON_DOWN, speed.cx, speed.cy, false);
        mouse(window, SDL_EVENT_MOUSE_BUTTON_UP, speed.cx, speed.cy, false);
        run_for(100);
        expect_image(pack, "ctl.speed.2x");

        check_settings_input(window, pack);
        check_keyboard(window);
        // A synthetic four-color frame checks composition independently of the
        // emulator. Expected colors come from BGR555, not captured ROM output.
        std::array<uint16_t, 160 * 144> pixels{};
        constexpr uint16_t colors[]{0x001f, 0x03e0, 0x7c00, 0x7fff};
        for (int y = 0; y < 144; ++y)
            for (int x = 0; x < 160; ++x) pixels[y * 160 + x] = colors[(y >= 72) * 2 + (x >= 80)];
        for (bool smooth : {true, false}) {
            gbx::platform::display::set_scaling(smooth ? gbx::platform::display::Scaling::smooth
                                                      : gbx::platform::display::Scaling::nearest);
            require(gbx::platform::display::show_game(pack.layout().display, 160, 144), "show game layer");
            uint16_t* frame = gbx::platform::display::game_frame_buffer();
            require(frame != nullptr, "game layer takes frames");
            std::copy(pixels.begin(), pixels.end(), frame);
            gbx::platform::display::publish_game_frame(1);
            run_for(20);
            check_menu(pack);
            check_game_layer(window, pack, 720, 1280, smooth);
            check_game_layer(window, pack, 360, 640, smooth);
            SDL_Event reset{};
            reset.type = SDL_EVENT_RENDER_DEVICE_RESET;
            require(SDL_PushEvent(&reset), "push renderer reset");
            run_for(20);
            check_menu(pack);
            check_game_layer(window, pack, 720, 1280, smooth);
        }
        gbx::platform::host::show_game_error();
        run_for(20);
        require(std::string_view(SDL_GetWindowTitle(window)).find("emulation stopped") != std::string_view::npos,
                "execution error is visible in the window title");
        check_game_layer(window, pack, 360, 640);

        window_event(window, SDL_EVENT_WINDOW_CLOSE_REQUESTED);
        require(!gbx::platform::host::poll_events(0), "close request exits the loop");
        gbx::app::shutdown();
        gbx::platform::host::shutdown();
    }
    fs::current_path(previous, error);
    require(!error, "restore working directory");
    fs::remove_all(card, error);
    require(!error, "remove isolated test card");
    std::puts("PASS: host UI lifecycle, source pixels, settings, mouse and keyboard input, game layer and renderer reset");
    return 0;
}
