#include "gbx/audio.h"
#include "gbx/display.h"

namespace gbx::platform::display {

bool start() {
    return false;
}

Size size() {
    return {0, 0};
}

bool lock(uint32_t) {
    return false;
}

void unlock() {}

bool scaling_supported(Scaling mode) {
    return mode == Scaling::nearest;
}

Scaling default_scaling() {
    return Scaling::nearest;
}

void set_scaling(Scaling) {}

bool show_game(std::span<const int16_t, 4>, int, int, PixelFormat) {
    return false;
}

void hide_game() {}

uint16_t* game_frame_buffer() {
    return nullptr;
}

void publish_game_frame(uint64_t) {}
void set_color_table(const uint16_t*) {}

int64_t game_frames_shown() {
    return -1;
}

// No frames are drawn.
void lend_emulator_core(bool) {}

bool brightness_supported() {
    return false;
}

void set_brightness(int) {}

}  // namespace gbx::platform::display

namespace gbx::platform::touch {

bool start() {
    return false;
}

int read(std::span<Point>) {
    return 0;
}

}  // namespace gbx::platform::touch

namespace gbx::platform::keys {

uint16_t read() {
    return 0;
}

}  // namespace gbx::platform::keys

namespace gbx::platform::audio {

bool open(uint32_t) {
    return false;
}

void close() {}

size_t write(std::span<const int16_t>, uint32_t) {
    return 0;
}

size_t buffered() {
    return 0;
}

uint32_t underruns() {
    return 0;
}

void set_volume(int) {}

void set_muted(bool) {}

void update() {}

void pause() {}

void resume() {}

}  // namespace gbx::platform::audio

namespace gbx::platform {

std::span<const std::byte> asset_pack() {
    return {};
}

}  // namespace gbx::platform
