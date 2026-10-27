// Shared by the 32 MiB ROM tests. tools/gba_test_roms.py fills the ROM with
// halfwords that depend on their address: the halfword at byte offset o is
// ((o >> 1) + (o >> 17) * 0x3B) & 0xFFFF. The term in o >> 17 makes copies
// of any power-of-two size from 128 KiB up differ, so a ROM mapping that
// drops address bit 24 reads wrong values above 16 MiB.
//
// The ROM is mirrored in the three waitstate regions 08000000h, 0A000000h
// and 0C000000h (GBATEK "GBA Memory Map", "GBA Cartridge ROM"). Test
// offsets stay clear of the program at the start of the ROM and of the code
// at 01F00000h (see gba.ld). Each test reads up to 8 bytes from its offset;
// the last one ends at the last byte of the ROM. In the 0C000000h mirror,
// offsets from 01FFFF00h up fall in DFFFF00h-DFFFFFFh, where a 32 MiB
// cartridge with EEPROM has its EEPROM (GBATEK "GBA Cart Backup EEPROM");
// they are skipped there.
#pragma once

#include "../common/gba.h"

static inline u16 expected16(u32 offset) {
  return (u16)((offset >> 1) + (offset >> 17) * 0x3B);
}

static inline u8 expected8(u32 offset) {
  u16 h = expected16(offset & ~1u);
  return (offset & 1) ? (u8)(h >> 8) : (u8)h;
}

static inline u32 expected32(u32 offset) {
  return expected16(offset) | ((u32)expected16(offset + 2) << 16);
}

static const u32 offsets[] = {
    0x00100000, 0x00ABCDE0, 0x00FFFFF8, 0x00FFFFFC, 0x01000000, 0x01000004,
    0x01020000, 0x01234560, 0x017FFFF8, 0x01800000, 0x01EFFFF8, 0x01FFFFF8,
};
#define OFFSET_COUNT (sizeof(offsets) / sizeof(offsets[0]))
static const u32 regions[] = {0x08000000, 0x0A000000, 0x0C000000};

static inline u32 testable(u32 base, u32 offset) {
  return base != 0x0C000000 || offset < 0x01FFFF00 - 8;
}
