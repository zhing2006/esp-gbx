// SRAM test without an ID string.
//
// jsmolka's save/sram.gba carries "SRAM_V", so here the save type can only
// be told from the first access: a byte write to E000000h. The test then
// writes and reads back all 32 KiB with byte accesses.
//
// GBATEK "GBA Cart Backup SRAM/FRAM": 32 KiB at E000000h-E007FFFh, 8-bit
// data bus, 8 waitstates (WAITCNT bits 0-1 = 3), accessed by LDRB/STRB, and
// read by code running in WRAM.

#include "../common/gba.h"

#define SRAM ((volatile u8*)0x0E000000)
#define SIZE 0x8000

static u8 pattern(u32 addr, u32 seed) {
  return (u8)(addr * 7 + (addr >> 8) * 13 + seed);
}

static IWRAM_CODE u32 read8(u32 addr) {
  return SRAM[addr];
}

static IWRAM_CODE u32 verify(u32 seed) {
  for (u32 a = 0; a < SIZE; a++) {
    if (SRAM[a] != pattern(a, seed)) return a + 1;
  }
  return 0;
}

int main(void) {
  REG_IME = 0;
  REG_WAITCNT = (REG_WAITCNT & ~3) | 3;

  // The first access: a byte write.
  SRAM[0] = 0x5A;

  // 1: the byte reads back.
  CHECK(1, read8(0) == 0x5A);

  // 2-3: every byte of the 32 KiB holds its own value, twice over.
  for (u32 a = 0; a < SIZE; a++) SRAM[a] = pattern(a, 0);
  CHECK(2, verify(0) == 0);
  for (u32 a = 0; a < SIZE; a++) SRAM[a] = pattern(a, 0x55);
  CHECK(3, verify(0x55) == 0);
  return 0;
}
