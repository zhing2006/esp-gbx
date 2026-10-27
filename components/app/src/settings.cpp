#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "gbx/executable.h"
#include "gbx/platform.h"

namespace gbx::app {
namespace {

std::string_view trim(std::string_view t) {
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r')) t.remove_suffix(1);
    return t;
}

// A whole number from `low` to `high` in steps of `step`.
bool parse_level(std::string_view t, int low, int high, int step, int& out) {
    if (t.empty() || t.size() > 3) return false;
    int value = 0;
    for (char c : t) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    if (value < low || value > high || value % step) return false;
    out = value;
    return true;
}

}  // namespace

Settings default_settings() {
    Settings s;
    s.backend = platform::executable_available() ? Backend::jit : Backend::interpreter;
    s.scaling = platform::display::default_scaling();
    return s;
}

void settings_path(char* out, size_t size) {
    std::snprintf(out, size, "config/%s.ini", platform::board_id());
}

void parse_settings(std::string_view text, Settings& s) {
    while (!text.empty()) {
        const size_t end = std::min(text.find('\n'), text.size());
        const std::string_view line = trim(text.substr(0, end));
        text.remove_prefix(std::min(end + 1, text.size()));
        const size_t equals = line.find('=');
        if (line.empty() || line.front() == '#' || equals == std::string_view::npos) continue;
        const std::string_view key = trim(line.substr(0, equals));
        const std::string_view value = trim(line.substr(equals + 1));
        if (key == "volume") {
            parse_level(value, 0, 100, 10, s.volume);
        } else if (key == "brightness") {
            parse_level(value, 10, 100, 10, s.brightness);
        } else if (key == "cpu") {
            if (value == "int") s.backend = Backend::interpreter;
            else if (value == "jit" && platform::executable_available()) s.backend = Backend::jit;
        } else if (key == "scaling") {
            if (value == "nearest" || value == "smooth") {
                const auto mode = value == "smooth" ? platform::display::Scaling::smooth
                                                     : platform::display::Scaling::nearest;
                s.scaling = platform::display::scaling_supported(mode) ? mode : platform::display::Scaling::nearest;
                s.scaling_explicit = true;
            }
        } else if (key == "palette") {
            // The app checks the name against the pack's palettes.
            if (!value.empty() && value.size() < sizeof s.palette) {
                std::memcpy(s.palette, value.data(), value.size());
                s.palette[value.size()] = '\0';
            }
        } else if (key == "color_fix") {
            if (value == "on" || value == "off") s.color_fix = value == "on";
        } else if (key == "last_rom") {
            if (value.size() < sizeof s.last_rom) {
                std::memcpy(s.last_rom, value.data(), value.size());
                s.last_rom[value.size()] = '\0';
            }
        }
    }
}

size_t format_settings(const Settings& s, char* out, size_t size) {
    const char* scaling = !s.scaling_explicit ? ""
                          : s.scaling == platform::display::Scaling::smooth ? "scaling=smooth\n"
                                                                           : "scaling=nearest\n";
    const int n = std::snprintf(out, size, "volume=%d\nbrightness=%d\ncpu=%s\npalette=%s\ncolor_fix=%s\nlast_rom=%s\n%s",
                                s.volume, s.brightness, s.backend == Backend::jit ? "jit" : "int", s.palette,
                                s.color_fix ? "on" : "off", s.last_rom, scaling);
    return n > 0 && static_cast<size_t>(n) < size ? static_cast<size_t>(n) : 0;
}

}  // namespace gbx::app
