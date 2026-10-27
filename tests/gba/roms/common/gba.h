// Register addresses and helpers shared by the GBX test ROMs. Addresses and
// bit layouts follow GBATEK "GBA I/O Map" and the sections cited next to each
// group.
#pragma once

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t s16;
typedef int32_t s32;

#define REG8(a) (*(volatile u8*)(a))
#define REG16(a) (*(volatile u16*)(a))
#define REG32(a) (*(volatile u32*)(a))

#define IWRAM_CODE __attribute__((section(".iwram"), target("arm"), noinline))
#define EWRAM_BSS __attribute__((section(".ewram")))

// GBATEK "LCD I/O Display Control", "LCD I/O Interrupts and Status"
#define REG_DISPCNT REG16(0x04000000)
#define REG_DISPSTAT REG16(0x04000004)
#define REG_VCOUNT REG16(0x04000006)

// GBATEK "GBA Timers"
#define REG_TMCNT_L(n) REG16(0x04000100 + 4 * (n))
#define REG_TMCNT_H(n) REG16(0x04000102 + 4 * (n))

// GBATEK "GBA DMA Transfers"
#define REG_DMASAD(n) REG32(0x040000B0 + 12 * (n))
#define REG_DMADAD(n) REG32(0x040000B4 + 12 * (n))
#define REG_DMACNT_L(n) REG16(0x040000B8 + 12 * (n))
#define REG_DMACNT_H(n) REG16(0x040000BA + 12 * (n))
#define DMA_DST_INC (0 << 5)
#define DMA_DST_FIXED (2 << 5)
#define DMA_DST_RELOAD (3 << 5)
#define DMA_SRC_INC (0 << 7)
#define DMA_SRC_FIXED (2 << 7)
#define DMA_REPEAT (1 << 9)
#define DMA_32 (1 << 10)
#define DMA_HBLANK (2 << 12)
#define DMA_ENABLE (1 << 15)

// GBATEK "GBA Interrupt Control", "GBA System Control"
#define REG_IE REG16(0x04000200)
#define REG_IF REG16(0x04000202)
#define REG_WAITCNT REG16(0x04000204)
#define REG_IME REG16(0x04000208)
#define IRQ_HANDLER REG32(0x03007FFC)
#define IRQ_VBLANK (1 << 0)
#define IRQ_HBLANK (1 << 1)

// Fails the test with number n when cond is false.
#define CHECK(n, cond) \
  do { \
    if (!(cond)) return (n); \
  } while (0)
