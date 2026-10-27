#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string_view>

#include "../../components/app/src/settings.h"

namespace {

using gbx::app::Settings;
using gbx::platform::display::Scaling;

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

std::string_view format(const Settings& settings, char (&text)[1024]) {
    const size_t size = gbx::app::format_settings(settings, text, sizeof text);
    require(size > 0, "settings fit in the file buffer");
    return {text, size};
}

}  // namespace

int main() {
    using namespace gbx::app;
    const auto expected_default = gbx::platform::display::scaling_supported(Scaling::smooth)
                                      ? Scaling::smooth : Scaling::nearest;
    require(gbx::platform::display::default_scaling() == expected_default,
            "smooth is the default when supported, otherwise nearest");
    Settings old = default_settings();
    require(old.scaling == gbx::platform::display::default_scaling() && !old.scaling_explicit,
            "new settings follow the platform default without pinning it");
    // Supply a known default independently of the current platform.
    old.scaling = Scaling::smooth;
    parse_settings("volume=30\nbrightness=80\ncpu=int\npalette=green\nlast_rom=roms/test.gb\n", old);
    require(old.scaling == Scaling::smooth && !old.scaling_explicit,
            "an old settings file preserves the supplied platform default");
    char text[1024];
    old.volume = 40;
    const auto saved = format(old, text);
    require(saved.find("scaling=") == std::string_view::npos, "unrelated saves do not pin scaling");
    require(saved.find("volume=40\n") != std::string_view::npos &&
                saved.find("brightness=80\n") != std::string_view::npos &&
                saved.find("cpu=int\n") != std::string_view::npos &&
                saved.find("palette=green\n") != std::string_view::npos &&
                saved.find("last_rom=roms/test.gb\n") != std::string_view::npos,
            "existing settings survive formatting");
    Settings updated = default_settings();
    updated.scaling = Scaling::nearest;
    parse_settings(saved, updated);
    require(updated.scaling == Scaling::nearest && !updated.scaling_explicit,
            "an unpinned file follows a changed default");

    for (const auto mode : {Scaling::nearest, Scaling::smooth}) {
        const auto effective = gbx::platform::display::scaling_supported(mode) ? mode : Scaling::nearest;
        Settings explicit_choice = default_settings();
        parse_settings(mode == Scaling::smooth ? "scaling=smooth\n" : "scaling=nearest\n", explicit_choice);
        require(explicit_choice.scaling == effective && explicit_choice.scaling_explicit,
                "saved scaling is used when supported, otherwise it falls back to nearest");
        explicit_choice.brightness = 70;
        const auto choice_text = format(explicit_choice, text);
        require(choice_text.find(effective == Scaling::smooth ? "scaling=smooth\n" : "scaling=nearest\n") !=
                    std::string_view::npos,
                "saving records the supported mode, including a nearest fallback");
        Settings reloaded = default_settings();
        reloaded.scaling = effective == Scaling::smooth ? Scaling::nearest : Scaling::smooth;
        parse_settings(choice_text, reloaded);
        require(reloaded.scaling == effective && reloaded.scaling_explicit,
                "a new default never overrides a supported persisted user choice");
    }
    std::puts("PASS: settings compatibility, default inheritance and explicit scaling persistence");
}
