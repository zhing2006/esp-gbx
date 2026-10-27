#pragma once

#include <cstdint>
#include <span>

// Desktop entry-point support, not part of the shared app or core interface.
// All functions run on the main thread, after app::init().
namespace gbx::platform::host {

// Waits up to max_wait_ms or the next LVGL deadline, then processes events.
// Mouse button edges are kept until sampled, even if down/up arrive together.
// Returns false on a window close or quit request.
bool poll_events(uint32_t max_wait_ms);

// Runs LVGL and presents any changed content. Returns false on a display error.
bool render();

// Draws the current textures without presenting. Shared by the normal
// presentation path and readback checks, which must read before present.
bool draw_frame();

// Keeps the window and last frame available after an execution error.
void show_game_error();

// Releases the host display and asset pack. Call after app::shutdown(), also
// when app::init() fails partway through startup.
void shutdown();

}  // namespace gbx::platform::host
