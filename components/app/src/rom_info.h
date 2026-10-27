#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace gbx::app {

// What a ROM file's header says, for the ROM list and the loader: Pan Docs,
// The Cartridge Header, and GBATEK, GBA Cartridge Header.
struct RomInfo {
    enum class Machine : uint8_t { none, gb, gbc, gbc_only, gba };
    // Why a ROM cannot run.
    // system: a machine the build does not run, see gbx/systems.h.
    enum class Problem : uint8_t { none, not_rom, too_large, type, size, ram, system };
    Machine machine = Machine::none;
    Problem problem = Problem::not_rom;
    uint8_t type = 0;      // 0147, the cartridge type
    uint8_t ram_code = 0;  // 0149
    char title[17] = {};
    // A GBA game's code at 0AC, and whether its cartridge has the RTC, see
    // gba::cartridge_has_rtc().
    char code[5] = {};
    bool rtc = false;
};

// The first bytes of a ROM file of `size` bytes, up to 0x150 of them.
inline constexpr size_t kRomHeaderBytes = 0x150;
// GB and GBC cartridges hold up to 8 MiB of ROM (Pan Docs, MBC5), GBA
// cartridges 32 MiB (GBATEK "GBA Memory Map").
inline constexpr int64_t kMaxRomBytes = 8 << 20;
inline constexpr int64_t kMaxGbaRomBytes = 32 << 20;
RomInfo parse_rom(std::span<const uint8_t> header, int64_t size);

// The title in the 16 bytes from 0134 to 0143, as shown to the user: printable
// ASCII without trailing spaces. CGB games that put a manufacturer code in
// 013F-0142 have an 11 byte title.
void rom_title(std::span<const uint8_t, 16> bytes, char (&out)[17]);

// A GBA game's title in the 12 bytes from 0A0 (GBATEK "GBA Cartridge
// Header"), as rom_title() shows a GB title.
void gba_title(std::span<const uint8_t, 12> bytes, char (&out)[17]);

// The footer line for a ROM that can run, such as "POKEMON YELLOW  MBC5  RAM 32K"
// or "POKEMON EMER  BPEE  RTC".
void describe_rom(const RomInfo& info, char* out, size_t size);
// Why a ROM cannot run, as a sentence for the ROM list and CAN'T LOAD.
void describe_problem(const RomInfo& info, char* out, size_t size);

}  // namespace gbx::app
