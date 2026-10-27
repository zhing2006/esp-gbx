// EEPROM test, built four ways: EEPROM_SIZE 512 or 8192, WITH_ID 1 or 0.
//
// With WITH_ID the ROM carries the "EEPROM_Vnnn" string, so the save type
// can be told from the ROM (GBATEK "GBA Cart Backup IDs"). Without it, the
// first save access is the read request below, a DMA bitstream to D000000h
// and up, and its length gives the size: 9 bits for 512 bytes (6 address
// bits), 17 bits for 8 KiB (14 address bits).
//
// Protocol and timing from GBATEK "GBA Cart Backup EEPROM":
// - 8 waitstates in WS2 (WAITCNT = X3XXh); the chip is at DFFFF00h for any
//   ROM size;
// - read: "11", n address bits MSB first, "0" by DMA3; then 68 bits by
//   DMA3, 4 ignored bits and 64 data bits MSB first;
// - write: "10", n address bits, 64 data bits, "0" by DMA3; then LDRH reads
//   bit 0 = 0 while the chip is busy and 1 when it is ready, within 10 ms
//   (programming takes ca. 108368 cycles);
// - 512 bytes: 64 blocks of 8 bytes, 6-bit address; 8 KiB: 1024 blocks,
//   14-bit address of which the lower 10 bits are used.

#include "../common/gba.h"

#if EEPROM_SIZE == 512
#define ADDR_BITS 6
#define BLOCKS 64
#elif EEPROM_SIZE == 8192
#define ADDR_BITS 14
#define BLOCKS 1024
#else
#error EEPROM_SIZE must be 512 or 8192
#endif

#if WITH_ID
__attribute__((used, aligned(4))) const char save_id[] = "EEPROM_Vnnn";
#endif

#define EEPROM_ADDR 0x0DFFFF00
#define EEPROM REG16(EEPROM_ADDR)

static u16 stream[2 + ADDR_BITS + 64 + 1];
static u16 reply[68];

static void dma3(const volatile void* src, volatile void* dst, u32 count) {
  REG_DMASAD(3) = (u32)src;
  REG_DMADAD(3) = (u32)dst;
  REG_DMACNT_L(3) = count;
  REG_DMACNT_H(3) = DMA_ENABLE;  // 16-bit, source and destination increment
  while (REG_DMACNT_H(3) & DMA_ENABLE) {
  }
}

static u32 put_address(u32 pos, u32 block) {
  for (int bit = ADDR_BITS - 1; bit >= 0; bit--) stream[pos++] = (block >> bit) & 1;
  return pos;
}

static void eeprom_read(u32 block, u32 out[2]) {
  u32 pos = 0;
  stream[pos++] = 1;
  stream[pos++] = 1;
  pos = put_address(pos, block);
  stream[pos++] = 0;
  dma3(stream, (volatile void*)EEPROM_ADDR, pos);
  dma3((const volatile void*)EEPROM_ADDR, reply, 68);
  out[0] = out[1] = 0;
  for (u32 i = 0; i < 64; i++) {
    out[i / 32] = (out[i / 32] << 1) | (reply[4 + i] & 1);
  }
}

// Starts a write and returns the status bit read right after the DMA.
static u32 eeprom_write_start(u32 block, const u32 data[2]) {
  u32 pos = 0;
  stream[pos++] = 1;
  stream[pos++] = 0;
  pos = put_address(pos, block);
  for (u32 i = 0; i < 64; i++) stream[pos++] = (data[i / 32] >> (31 - i % 32)) & 1;
  stream[pos++] = 0;
  dma3(stream, (volatile void*)EEPROM_ADDR, pos);
  return EEPROM & 1;
}

// Timer 0 and 1 cascaded count at 16.78 MHz / 64 = 262144 Hz
// (GBATEK "GBA Timers").
static void timer_start(void) {
  REG_TMCNT_H(0) = 0;
  REG_TMCNT_H(1) = 0;
  REG_TMCNT_L(0) = 0;
  REG_TMCNT_L(1) = 0;
  REG_TMCNT_H(1) = 0x84;  // count-up, enable
  REG_TMCNT_H(0) = 0x81;  // F/64, enable
}

static u32 timer_read(void) {
  u32 hi = REG_TMCNT_L(1);
  u32 lo = REG_TMCNT_L(0);
  u32 hi2 = REG_TMCNT_L(1);
  if (hi2 != hi) lo = REG_TMCNT_L(0);
  return (hi2 << 16) | lo;
}

// Waits until the status bit is 1. Returns 0 on timeout after 10 ms.
static u32 wait_ready(void) {
  timer_start();
  while (timer_read() < 2622) {
    if (EEPROM & 1) return 1;
  }
  return 0;
}

static u32 write_block(u32 block, u32 hi, u32 lo, u32* busy) {
  const u32 data[2] = {hi, lo};
  *busy = eeprom_write_start(block, data) == 0;
  return wait_ready();
}

static u32 read_matches(u32 block, u32 hi, u32 lo) {
  u32 got[2];
  eeprom_read(block, got);
  return got[0] == hi && got[1] == lo;
}

int main(void) {
  u32 busy, data[2];
  REG_IME = 0;
  REG_WAITCNT = (REG_WAITCNT & ~0x0700) | 0x0300;  // WS2: 8,8 waitstates

  // 1: the first access is a read request (size detection); it completes.
  eeprom_read(0, data);

  // 2-4: a write reports busy, then ready, and reads back.
  CHECK(2, write_block(1, 0x01234567, 0x89ABCDEF, &busy));
  CHECK(3, busy);
  CHECK(4, read_matches(1, 0x01234567, 0x89ABCDEF));

  // 5-8: first and last block are separate, the data bit order holds.
  CHECK(5, write_block(0, 0x80000001, 0x00000000, &busy));
  CHECK(6, write_block(BLOCKS - 1, 0xFFFF0000, 0x0000FFFF, &busy));
  CHECK(7, read_matches(0, 0x80000001, 0x00000000));
  CHECK(8, read_matches(BLOCKS - 1, 0xFFFF0000, 0x0000FFFF));

  // 9-10: a write replaces the old data of its block only.
  CHECK(9, write_block(1, 0xDEADBEEF, 0x13579BDF, &busy));
  CHECK(10, read_matches(1, 0xDEADBEEF, 0x13579BDF) && read_matches(0, 0x80000001, 0x00000000));

  // 11-12: every address bit selects a different block.
  for (u32 bit = 0; (1u << bit) < BLOCKS; bit++) {
    CHECK(11, write_block(1u << bit, 0xA5A50000 | bit, ~bit, &busy));
  }
  for (u32 bit = 0; (1u << bit) < BLOCKS; bit++) {
    CHECK(12, read_matches(1u << bit, 0xA5A50000 | bit, ~bit));
  }
  return 0;
}
