#pragma once

#include <cstdint>
#include <span>

namespace gbx::gb {
// The console and how it runs the cartridge: a DMG-C, which is GB mode, or a
// CPU CGB E in CGB mode or in its DMG compatibility mode, which Pan Docs
// calls Non-CGB Mode. The caller chooses the mode before loading. A CGB runs
// a cartridge whose CGB flag has bit 7 set in CGB mode and the others in
// compatibility mode (Pan Docs, The Cartridge Header, 0143 — CGB flag), see
// cgb_mode().
enum class Mode : uint8_t { dmg, cgb, compat };

// The mode a CGB runs `rom` in.
inline Mode cgb_mode(std::span<const uint8_t> rom) {
    return rom.size() > 0x143 && (rom[0x143] & 0x80) ? Mode::cgb : Mode::compat;
}
// The mode the app runs `rom` in, with the cartridge in a CGB when `cgb` is
// set and in a DMG-C otherwise. A cartridge for the CGB only, flag 0xC0,
// always goes in a CGB: the hardware ignores bit 6 (Pan Docs, 0143), so on a
// DMG such a game can only show that it needs a CGB.
inline Mode run_mode(std::span<const uint8_t> rom, bool cgb) {
    const bool cgb_only = rom.size() > 0x143 && (rom[0x143] & 0xc0) == 0xc0;
    return cgb || cgb_only ? cgb_mode(rom) : Mode::dmg;
}
}  // namespace gbx::gb
