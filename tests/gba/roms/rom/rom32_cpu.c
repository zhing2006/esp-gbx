// 32 MiB ROM read by the CPU: byte, halfword and word loads in ARM and in
// Thumb state at offsets below and above 16 MiB in the three ROM mirrors,
// and code executed from 01F00000h through each mirror. DMA reads are in
// rom32_dma.c. Pattern and offsets: rom32.h.

#include "rom32.h"

extern u32 high_arm(void);
extern u32 high_thumb(void);

static u32 check_arm(u32 base) {
  for (u32 i = 0; i < OFFSET_COUNT; i++) {
    u32 o = offsets[i];
    if (!testable(base, o)) continue;
    volatile u8* p8 = (volatile u8*)(base + o);
    volatile u16* p16 = (volatile u16*)(base + o);
    volatile u32* p32 = (volatile u32*)(base + o);
    if (*p8 != expected8(o) || p8[1] != expected8(o + 1) || p8[3] != expected8(o + 3)) return 0;
    if (*p16 != expected16(o) || p16[1] != expected16(o + 2)) return 0;
    if (*p32 != expected32(o) || p32[1] != expected32(o + 4)) return 0;
  }
  return 1;
}

__attribute__((target("thumb"), noinline)) static u32 load_thumb(u32 addr, u32 width) {
  if (width == 1) return *(volatile u8*)addr;
  if (width == 2) return *(volatile u16*)addr;
  return *(volatile u32*)addr;
}

static u32 check_thumb(u32 base) {
  for (u32 i = 0; i < OFFSET_COUNT; i++) {
    u32 o = offsets[i];
    if (!testable(base, o)) continue;
    if (load_thumb(base + o + 1, 1) != expected8(o + 1)) return 0;
    if (load_thumb(base + o + 2, 2) != expected16(o + 2)) return 0;
    if (load_thumb(base + o + 4, 4) != expected32(o + 4)) return 0;
  }
  return 1;
}

int main(void) {
  for (u32 r = 0; r < 3; r++) {
    // 1-3: ARM loads, 4-6: Thumb loads, in each mirror.
    CHECK(1 + r, check_arm(regions[r]));
    CHECK(4 + r, check_thumb(regions[r]));
  }
  for (u32 r = 0; r < 3; r++) {
    u32 offset = (u32)high_arm - 0x08000000;
    u32 (*arm)(void) = (u32 (*)(void))(regions[r] + offset);
    u32 (*thumb)(void) = (u32 (*)(void))(regions[r] + ((u32)high_thumb - 0x08000000));
    // 7-9: ARM code above 16 MiB runs and sees its own address.
    CHECK(7 + r, arm() == regions[r] + offset + 0x12345678);
    // 10-12: Thumb code above 16 MiB runs.
    CHECK(10 + r, thumb() == ((regions[r] + ((u32)high_thumb & ~1u) - 0x08000000 + 4) + 0x0FEDCBA0));
  }
  return 0;
}
