#pragma once

#include <cstddef>
#include <string_view>

#include "gbx/app.h"
#include "gbx/display.h"
#include "gbx/files.h"

namespace gbx::app {

// The emulator's settings, kept on the card in config/<board>.ini: each board
// has its own file, since boards differ in what they support, such as the
// PC's lack of backlight control and JIT. The file is text, one key=value per
// line. Keys it does not know are skipped and a missing or bad value keeps
// its default, so firmware of another version reads the same file.
struct Settings {
    int volume = 60;       // 0 to 100 in steps of 10
    int brightness = 100;  // 10 to 100 in steps of 10
    Backend backend = Backend::interpreter;
    platform::display::Scaling scaling = platform::display::Scaling::nearest;
    bool scaling_explicit = false;  // omit the key until the user chooses a mode
    // The id of a menu palette: a GB mode palette, or "color", which puts
    // cartridges in a GBC, see gb::run_mode().
    char palette[8] = "gray";
    bool color_fix = true;  // the LCD color correction of color games
    char last_rom[platform::files::kMaxPath] = {};  // the card path of the ROM last started
};

// The defaults for this platform: the JIT where it can run, and its sampling mode.
Settings default_settings();
// "config/sila_p4.ini" and so on.
void settings_path(char* out, size_t size);
// Reads `text` over the defaults; unsupported scaling falls back to nearest.
void parse_settings(std::string_view text, Settings& settings);
// Writes the file's text; returns its length, or 0 when it does not fit.
size_t format_settings(const Settings& settings, char* out, size_t size);

}  // namespace gbx::app
