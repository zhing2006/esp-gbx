#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "gbx/systems.h"

namespace gbx::app {

// The platform must be ready before init(). Keep its display and asset pack
// alive until shutdown() returns. Only one caller drives the app lifecycle.
bool init();

// Nonblocking; samples input at the same cadence on all platforms.
void update();

// Maximum time the event loop can sleep before the next input sample is due.
uint32_t update_delay_ms();
void shutdown();

// Firmware entry point: initializes the app and drives it on the calling task.
void run();

enum class Backend : uint8_t { interpreter, jit };
enum class GameState : uint8_t { no_rom, loading, run, paused, error };

// Loads the ROM at a card path, such as "roms/gb/Tetris.gb", and runs it with
// `backend`, replacing the running game. Returns at once: the state is
// LOADING until the game runs or fails. After init(). `cgb` puts the
// cartridge in a GBC, see gb::run_mode().
void start_game(const char* path, Backend backend, bool cgb = false);
GameState game_state();
// For the GBA games started after it: the BIOS file, a path as start_game()
// takes it, and the ROM cache's budget in bytes. Empty and 0 take the
// card's bios/gba_bios.bin and the default budget. After init().
void set_gba_options(const char* bios, size_t rom_cache_bytes);

// For the serial console's commands, see docs/debug-design.md.
// Keys held from the console, on top of the controls: GB bits and
// platform::keys::kL, kR and kMenu.
void set_console_keys(uint16_t keys);
// Sets the speed switch to 2X or 1X, as a touch on it would.
void set_double_speed(bool on);
// Loads instant save `slot`, 1 to 4, of the running game as the SLOTS page
// would, and runs on from it.
void load_slot(int slot);
// Logs the status at the next update, then every period_ms while a game
// runs; 0 logs it once.
void log_status(uint32_t period_ms);
// Copies the next frame, BGR555, into `out`, which takes kMaxFramePixels: a
// GB game's 160x144 or a GBA game's 240x160, as `width` and `height` say;
// a build without the GBA has only the GB's. False on timeout.
inline constexpr size_t kMaxFramePixels = GBX_HAS_GBA ? 240 * 160 : 160 * 144;
bool capture_frame(uint16_t* out, uint32_t timeout_ms, int& width, int& height);
// Copies what the menu canvas shows, RGB565 pixels of the menu grid, into
// `out`, whether or not a game frame covers it. Sets width and height also
// when `out` is too small, which returns false, as nothing drawn yet does.
bool capture_menu(std::span<uint16_t> out, int& width, int& height);
// Holds back status lines while the caller writes lines that must stay
// whole, such as a shot; due lines print after the release. One caller at a
// time, which releases from the same task.
void hold_status(bool held);

#ifdef GBX_PROFILER
// Key recordings of profiling builds, see docs/debug-design.md. Recording
// loads the ROM at `path`, in a GBC when `cgb`, and keeps the keys of each
// frame until stop_recording() writes them to `file` on the card; frames are
// timed from the mark on. Replaying loads the recording's ROM where it ran
// and presses its keys. False when the recording cannot be read.
void record_game(const char* path, Backend backend, const char* file, bool cgb = false);
void mark_recording();
void stop_recording();
bool replay_game(const char* file, Backend backend);
// Times the GBA frames from `skip` to `skip + frames` after the next slot
// load, or with `absolute` from frame `skip` of the running game, and logs
// the CPU core's cycles by part, see Game::bench().
void bench_slot(uint32_t skip, uint32_t frames, bool absolute);
#endif

}  // namespace gbx::app
