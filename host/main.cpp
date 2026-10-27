#include "gbx/app.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#if GBX_UI_LVGL
#include "gbx/host.h"
#include "gbx/platform.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

// A path argument in UTF-8, as the file functions take paths. Windows
// passes main() its arguments in the ANSI code page, which cannot hold every
// name, so there the path is read again from the wide-character command
// line. `index` is the argument's position on the original command line.
std::string path_argument([[maybe_unused]] char** argv, int index) {
#ifdef _WIN32
    std::string path;
    int count = 0;
    wchar_t** args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (args && count > index) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, args[index], -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            path.resize(static_cast<size_t>(n));
            WideCharToMultiByte(CP_UTF8, 0, args[index], -1, path.data(), n, nullptr, nullptr);
            path.pop_back();
        }
    }
    LocalFree(args);
    return path;
#else
    return argv[index];
#endif
}

}  // namespace
#endif

int main(int argc, char** argv) {
#if GBX_UI_LVGL
    // --cgb puts the cartridge in a GBC, as PALETTE's COLOR does for a game
    // the menus start, see gb::run_mode(); a game for the GBC only runs on
    // one anyway. A GBA game reads the BIOS at --bios, and its ROM cache
    // holds --rom-cache KiB; the default keeps any ROM whole.
    bool cgb = false, help = false, invalid = false;
    int rom = 0, bios = 0;
    size_t rom_cache = size_t{32} << 20;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help") {
            help = true;
        } else if (arg == "--cgb") {
            cgb = true;
        } else if (arg == "--bios" && i + 1 < argc) {
            bios = ++i;
        } else if (arg == "--rom-cache" && i + 1 < argc) {
            char* end = nullptr;
            const unsigned long kib = std::strtoul(argv[++i], &end, 10);
            invalid |= *end || kib < 64 || kib > 32 * 1024;
            rom_cache = static_cast<size_t>(kib) << 10;
        } else if (!rom && !arg.starts_with("-")) {
            rom = i;
        } else {
            invalid = true;
        }
    }
    if (help || invalid) {
        std::printf("Usage: %s [--cgb] [--bios FILE] [--rom-cache KIB] [ROM]\n"
                    "Without a ROM the menus open. --cgb runs a GB ROM on a GBC, in CGB mode or,\n"
                    "for a monochrome game, in compatibility mode; a game for the GBC only runs\n"
                    "on one anyway. A GBA ROM needs the BIOS, bios/gba_bios.bin unless --bios\n"
                    "names another, and pages through a ROM cache of --rom-cache KiB (64 to\n"
                    "32768, default 32768). Keys: arrows = D-pad, Z = A, X = B, Enter = Start,\n"
                    "right Shift = Select, A = L, S = R, Esc = MENU\n",
                    argv[0]);
        return invalid ? 1 : 0;
    }
#else
    if (argc > 1) {
        std::fprintf(stderr, "This build has no GUI. Run without arguments.\n");
        return 1;
    }
    (void)argv;
#endif
    bool ok = gbx::app::init();
    int exit_code = 0;
#if GBX_UI_LVGL
    using gbx::app::GameState;
    // The game given on the command line: a ROM that cannot load ends the
    // program; one that stops while running leaves the window open and the
    // program ends with an error when it closes.
    enum class Watch { none, loading, running } watch = Watch::none;
    if (ok) gbx::app::set_gba_options(bios ? path_argument(argv, bios).c_str() : "", rom_cache);
    if (ok && rom) {
        // The PC build has only the interpreter. Loading runs on the
        // emulation thread; questions about the save show in the window.
        gbx::app::start_game(path_argument(argv, rom).c_str(), gbx::app::Backend::interpreter, cgb);
        watch = Watch::loading;
    }
    while (ok) {
        if (!gbx::platform::host::poll_events(gbx::app::update_delay_ms())) break;
        const GameState state = gbx::app::game_state();
        if (watch == Watch::loading) {
            if (state == GameState::run) watch = Watch::running;
            else if (state == GameState::no_rom) watch = Watch::none;
            else if (state == GameState::error) ok = false;
        } else if (watch == Watch::running) {
            if (state == GameState::no_rom) {
                watch = Watch::none;
            } else if (state == GameState::error) {
                gbx::platform::host::show_game_error();
                exit_code = 1;
                watch = Watch::none;
            }
        }
        if (!ok) break;
        gbx::app::update();
        ok = gbx::platform::host::render();
    }
#endif
    gbx::app::shutdown();
#if GBX_UI_LVGL
    gbx::platform::host::shutdown();
#endif
    return ok ? exit_code : 1;
}
