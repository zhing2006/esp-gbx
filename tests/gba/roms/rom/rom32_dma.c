// 32 MiB ROM read by DMA. DMA0 reads internal memory only; DMA1-3 read any
// memory (GBATEK "GBA DMA Transfers", DMAxSAD). Each of them copies ROM into
// IWRAM in 16- and 32-bit units with an incrementing source: across the
// 16 MiB boundary, near the end of the ROM, and in the upper half. GBATEK
// does not say how a decrementing or fixed source behaves in the cartridge,
// so those are not used. Pattern: rom32.h.

#include "rom32.h"

static u32 buffer[64];

static void dma(u32 ch, u32 src, u32 count, u16 control) {
  REG_DMASAD(ch) = src;
  REG_DMADAD(ch) = (u32)buffer;
  REG_DMACNT_L(ch) = count;
  REG_DMACNT_H(ch) = DMA_ENABLE | control;
  while (REG_DMACNT_H(ch) & DMA_ENABLE) {
  }
}

static u32 check16(u32 o, u32 count) {
  const u16* b = (const u16*)buffer;
  for (u32 i = 0; i < count; i++) {
    if (b[i] != expected16(o + 2 * i)) return 0;
  }
  return 1;
}

static u32 check32(u32 o, u32 count) {
  for (u32 i = 0; i < count; i++) {
    if (buffer[i] != expected32(o + 4 * i)) return 0;
  }
  return 1;
}

int main(void) {
  REG_IME = 0;
  for (u32 ch = 1; ch <= 3; ch++) {
    for (u32 r = 0; r < 3; r++) {
      u32 base = regions[r];
      u32 n = (ch - 1) * 3 + r;  // 0..8
      // 1-9: 16-bit, incrementing across the 16 MiB boundary.
      dma(ch, base + 0x00FFFFC0, 64, DMA_SRC_INC);
      CHECK(1 + n, check16(0x00FFFFC0, 64));
      // 10-18: 32-bit, incrementing, near the end of the ROM (below the
      // EEPROM window of the 0C000000h mirror, see rom32.h).
      dma(ch, base + 0x01FFFE80, 32, DMA_SRC_INC | DMA_32);
      CHECK(10 + n, check32(0x01FFFE80, 32));
      // 19-27: 16-bit, in the upper half.
      dma(ch, base + 0x01234566, 40, DMA_SRC_INC);
      CHECK(19 + n, check16(0x01234566, 40));
    }
  }
  return 0;
}
