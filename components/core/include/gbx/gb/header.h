#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace gbx::gb {

// The logo every cartridge carries at 0104-0133 (Pan Docs, The Cartridge
// Header). The CGB boot ROM checks only the first 0x18 bytes.
inline constexpr std::array<uint8_t, 48> kNintendoLogo{
    0xce, 0xed, 0x66, 0x66, 0xcc, 0x0d, 0x00, 0x0b, 0x03, 0x73, 0x00, 0x83, 0x00, 0x0c, 0x00, 0x0d,
    0x00, 0x08, 0x11, 0x1f, 0x88, 0x89, 0x00, 0x0e, 0xdc, 0xcc, 0x6e, 0xe6, 0xdd, 0xdd, 0xd9, 0x99,
    0xbb, 0xbb, 0x67, 0x63, 0x6e, 0x0e, 0xec, 0xcc, 0xdd, 0xdc, 0x99, 0x9f, 0xbb, 0xb9, 0x33, 0x3e};

// What the CGB boot ROM checks in the header of a monochrome cartridge (Pan
// Docs, Power Up Sequence, Compatibility palettes): whether the licensee is
// Nintendo, the old licensee code 01, or 33 with the new code "01", and the
// sum of the 16 title bytes at 0134-0143.
inline bool nintendo_licensee(std::span<const uint8_t> rom) {
    if (rom.size() <= 0x14b) return false;
    return rom[0x14b] == 0x33 ? rom[0x144] == '0' && rom[0x145] == '1' : rom[0x14b] == 0x01;
}
inline uint8_t title_checksum(std::span<const uint8_t> rom) {
    unsigned sum = 0;
    for (unsigned i = 0x134; i < 0x144 && i < rom.size(); ++i) sum += rom[i];
    return static_cast<uint8_t>(sum);
}
// The palettes the boot ROM picks for a monochrome cartridge in
// compatibility mode: BG palette 0, then OBJ palettes 0 and 1, four BGR555
// colors each, lightest index first.
std::array<uint16_t, 12> compatibility_palettes(std::span<const uint8_t> rom);

}  // namespace gbx::gb
