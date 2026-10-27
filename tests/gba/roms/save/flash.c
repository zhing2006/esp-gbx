// 64 KiB Flash test without an ID string.
//
// jsmolka's save/flash64.gba and flash128.gba carry "FLASH_V" strings, so
// here the save type can only be told from the first access: the Flash
// command sequence at E005555h. Without FLASH1M_V the Flash is 64 KiB
// (GBATEK "GBA Cart Backup IDs").
//
// Commands, device IDs and the Atmel sector write from GBATEK "GBA Cart
// Backup Flash ROM". Any of the four 64 KiB devices is accepted; reads and
// status polling run from IWRAM, and accesses use 8 waitstates.

#include "../common/gba.h"

#define FLASH ((volatile u8*)0x0E000000)
#define SIZE 0x10000

static IWRAM_CODE u32 read8(u32 addr) {
  return FLASH[addr];
}

static void command(u8 cmd) {
  FLASH[0x5555] = 0xAA;
  FLASH[0x2AAA] = 0x55;
  FLASH[0x5555] = cmd;
}

// Timer 0 and 1 cascaded at 262144 Hz (GBATEK "GBA Timers").
static void timer_start(void) {
  REG_TMCNT_H(0) = 0;
  REG_TMCNT_H(1) = 0;
  REG_TMCNT_L(0) = 0;
  REG_TMCNT_L(1) = 0;
  REG_TMCNT_H(1) = 0x84;
  REG_TMCNT_H(0) = 0x81;
}

static u32 timer_read(void) {
  u32 hi = REG_TMCNT_L(1);
  u32 lo = REG_TMCNT_L(0);
  u32 hi2 = REG_TMCNT_L(1);
  if (hi2 != hi) lo = REG_TMCNT_L(0);
  return (hi2 << 16) | lo;
}

// Waits until addr reads value; the timeout is the longest one in the device
// table, 2000 ms for a chip erase on Macronix.
static IWRAM_CODE u32 wait_for(u32 addr, u8 value) {
  timer_start();
  while (timer_read() < 524288) {
    if (FLASH[addr] == value) return 1;
  }
  return 0;
}

static IWRAM_CODE u32 all_equal(u32 start, u32 end, u8 value) {
  for (u32 a = start; a < end; a++) {
    if (FLASH[a] != value) return 0;
  }
  return 1;
}

static u8 pattern(u32 addr) {
  return (u8)(addr * 5 + (addr >> 7) + 1);
}

static u32 atmel;

// Writes 128 bytes from addr (a multiple of 80h) with pattern(). Atmel
// erases and writes a 128-byte sector in one command; the others need an
// erased sector and take one command per byte.
static u32 program(u32 addr) {
  if (atmel) {
    command(0xA0);
    for (u32 i = 0; i < 128; i++) FLASH[addr + i] = pattern(addr + i);
    return wait_for(addr + 127, pattern(addr + 127));
  }
  for (u32 i = 0; i < 128; i++) {
    command(0xA0);
    FLASH[addr + i] = pattern(addr + i);
    if (!wait_for(addr + i, pattern(addr + i))) return 0;
  }
  return 1;
}

static IWRAM_CODE u32 holds_pattern(u32 addr) {
  for (u32 i = 0; i < 128; i++) {
    if (FLASH[addr + i] != pattern(addr + i)) return 0;
  }
  return 1;
}

int main(void) {
  REG_IME = 0;
  REG_WAITCNT = (REG_WAITCNT & ~3) | 3;

  // 1: the ID of a 64 KiB device.
  command(0x90);
  u32 id = read8(0) | (read8(1) << 8);
  command(0xF0);
  atmel = id == 0x3D1F;
  CHECK(1, id == 0xD4BF || id == 0x1CC2 || id == 0x1B32 || atmel);

  // 2: after leaving ID mode the array reads normally again: a chip erase
  // leaves FFh everywhere.
  command(0x80);
  command(0x10);
  CHECK(2, wait_for(0, 0xFF) && all_equal(0, SIZE, 0xFF));

  // 3-4: programmed bytes read back, in the first and the last sector.
  CHECK(3, program(0x0000) && holds_pattern(0x0000));
  CHECK(4, program(0xFF80) && holds_pattern(0xFF80));

  // 5: a byte write without the write command changes nothing.
  FLASH[0x0100] = 0x00;
  CHECK(5, read8(0x0100) == 0xFF);

  // 6-7: erasing sector 0 (4 KiB; on Atmel, rewriting its first 128 bytes
  // with FFh) leaves the last sector alone.
  if (atmel) {
    command(0xA0);
    for (u32 i = 0; i < 128; i++) FLASH[i] = 0xFF;
    CHECK(6, wait_for(127, 0xFF) && all_equal(0, 128, 0xFF));
  } else {
    command(0x80);
    FLASH[0x5555] = 0xAA;
    FLASH[0x2AAA] = 0x55;
    FLASH[0x0000] = 0x30;
    CHECK(6, wait_for(0, 0xFF) && all_equal(0, 0x1000, 0xFF));
  }
  CHECK(7, holds_pattern(0xFF80));
  return 0;
}
