// Cartridge RTC test: the GPIO port and the S-3511 behind it.
//
// The header's game code is 0RTC, which task 5-2 registers as a cartridge
// with RTC; no Nintendo game code starts with a digit (GBATEK "GBA
// Cartridge Header").
//
// GPIO (GBATEK "GBA Cart I/O Port (GPIO)"): data at 80000C4h, direction at
// 80000C6h (1 = out), control at 80000C8h (bit 0: 1 = registers readable);
// writes are 16-bit. The RTC uses bit 0 = SCK, bit 1 = SIO, bit 2 = CS.
//
// RTC (GBATEK "GBA Cart Real-Time Clock (RTC)" and "DS Real-Time Clock
// (RTC)"): a transfer raises CS, sends the command byte and then the
// parameter bytes, and drops CS. Each bit: SCK low with SIO set when
// writing, wait 5 us, SCK high, wait 5 us, then sample SIO when reading.
// Parameters go LSB first; the command byte is sent MSB first as 0110 xxx r
// (6xh), which is the same bit sequence as the LSB-first "Fwd" codes.
// Commands: 60h force reset, 62h/63h control, 64h/65h date and time
// (year, month, day, day of week, hour, minute, second), 66h/67h time.
// Control bit 6: 1 = 24-hour mode. After a force reset all registers are
// 00h except day and month, which are 01h, so the control register reads
// 00h (12-hour mode). Hour bit 7 is the PM flag: read only in 24-hour mode
// (set for 12h..23h), read/write in 12-hour mode, where 12 o'clock is 00h.
// All values are BCD.

#include "../common/gba.h"

#define GPIO_DATA REG16(0x080000C4)
#define GPIO_DIR REG16(0x080000C6)
#define GPIO_CTRL REG16(0x080000C8)
#define SCK 1
#define SIO 2
#define CS 4

// About 5 us at 16.78 MHz.
static void wait_5us(void) {
  for (volatile u32 i = 0; i < 12; i++) {
  }
}

static void begin(void) {
  GPIO_DIR = SCK | SIO | CS;
  GPIO_DATA = SCK;  // CS low, SCK high
  wait_5us();
  GPIO_DATA = SCK | CS;
  wait_5us();
}

static void end(void) {
  GPIO_DIR = SCK | SIO | CS;
  GPIO_DATA = SCK;
  wait_5us();
}

static void send_bit(u32 bit) {
  u32 sio = bit ? SIO : 0;
  GPIO_DATA = CS | sio;
  wait_5us();
  GPIO_DATA = CS | SCK | sio;
  wait_5us();
}

static void send_command(u8 cmd) {
  GPIO_DIR = SCK | SIO | CS;
  for (int bit = 7; bit >= 0; bit--) send_bit((cmd >> bit) & 1);
}

static void send_byte(u8 value) {
  GPIO_DIR = SCK | SIO | CS;
  for (int bit = 0; bit < 8; bit++) send_bit((value >> bit) & 1);
}

static u8 receive_byte(void) {
  GPIO_DIR = SCK | CS;
  u8 value = 0;
  for (int bit = 0; bit < 8; bit++) {
    GPIO_DATA = CS;
    wait_5us();
    GPIO_DATA = CS | SCK;
    wait_5us();
    value |= ((GPIO_DATA & SIO) ? 1 : 0) << bit;
  }
  return value;
}

static void rtc_write(u8 cmd, const u8* data, u32 count) {
  begin();
  send_command(cmd);
  for (u32 i = 0; i < count; i++) send_byte(data[i]);
  end();
}

static void rtc_read(u8 cmd, u8* data, u32 count) {
  begin();
  send_command(cmd);
  for (u32 i = 0; i < count; i++) data[i] = receive_byte();
  end();
}

// Timer 2 and 3 cascaded at 16.78 MHz / 1024 = 16384 Hz.
static void timer_start(void) {
  REG_TMCNT_H(2) = 0;
  REG_TMCNT_H(3) = 0;
  REG_TMCNT_L(2) = 0;
  REG_TMCNT_L(3) = 0;
  REG_TMCNT_H(3) = 0x84;
  REG_TMCNT_H(2) = 0x83;
}

static u32 timer_read(void) {
  u32 hi = REG_TMCNT_L(3);
  u32 lo = REG_TMCNT_L(2);
  u32 hi2 = REG_TMCNT_L(3);
  if (hi2 != hi) lo = REG_TMCNT_L(2);
  return (hi2 << 16) | lo;
}

static u32 equal(const u8* a, const u8* b, u32 n) {
  for (u32 i = 0; i < n; i++) {
    if (a[i] != b[i]) return 0;
  }
  return 1;
}

// Waits until the seconds register changes from its current value. Returns
// the timer value at the change, or 0 after 3 s.
static u32 wait_second(u8* now) {
  u8 t[3];
  rtc_read(0x67, t, 3);
  u8 start = t[2];
  while (timer_read() < 3 * 16384) {
    rtc_read(0x67, t, 3);
    if (t[2] != start) {
      u32 at = timer_read();
      rtc_read(0x65, now, 7);
      return at ? at : 1;
    }
  }
  return 0;
}

// Sets date and time, waits for the next second, and compares.
static u32 roll(const u8 from[7], const u8 to[7]) {
  u8 now[7];
  rtc_write(0x64, from, 7);
  timer_start();
  if (!wait_second(now)) return 0;
  return equal(now, to, 7);
}

int main(void) {
  u8 buf[7];
  REG_IME = 0;

  // 1-3: GPIO registers are write-only until the control bit is set; then
  // data and direction read back.
  GPIO_CTRL = 0;
  GPIO_DIR = SCK | CS;
  GPIO_DATA = SCK;
  CHECK(1, GPIO_DATA == 0 && GPIO_DIR == 0);
  GPIO_CTRL = 1;
  CHECK(2, GPIO_CTRL == 1 && GPIO_DIR == (SCK | CS));
  CHECK(3, (GPIO_DATA & (SCK | CS)) == SCK);

  // 4-5: force reset: control 00h, 2000-01-01, day 0, 00:00:00.
  rtc_write(0x60, buf, 0);
  rtc_read(0x63, buf, 1);
  CHECK(4, buf[0] == 0x00);
  rtc_read(0x65, buf, 7);
  CHECK(5, buf[0] == 0x00 && buf[1] == 0x01 && buf[2] == 0x01 && buf[3] == 0x00 && buf[4] == 0x00 &&
               buf[5] == 0x00 && buf[6] <= 0x01);

  // 6: 24-hour mode is set and reads back.
  buf[0] = 0x40;
  rtc_write(0x62, buf, 1);
  rtc_read(0x63, buf, 1);
  CHECK(6, buf[0] == 0x40);

  // 7-8: date and time read back as written; in 24-hour mode 23h reads
  // with the PM flag.
  {
    static const u8 set[7] = {0x26, 0x10, 0x23, 0x04, 0x23, 0x45, 0x10};
    rtc_write(0x64, set, 7);
    rtc_read(0x65, buf, 7);
    CHECK(7, equal(buf, set, 4) && buf[4] == (0x23 | 0x80) && buf[5] == 0x45 && (buf[6] == 0x10 || buf[6] == 0x11));
    static const u8 t[3] = {0x09, 0x30, 0x00};
    rtc_write(0x66, t, 3);
    rtc_read(0x67, buf, 3);
    CHECK(8, buf[0] == 0x09 && buf[1] == 0x30 && buf[2] <= 0x01);
  }

  // 9: BCD carry from 09:59:59 to 10:00:00.
  {
    static const u8 a[7] = {0x26, 0x10, 0x23, 0x04, 0x09, 0x59, 0x59};
    static const u8 b[7] = {0x26, 0x10, 0x23, 0x04, 0x10, 0x00, 0x00};
    CHECK(9, roll(a, b));
  }
  // 10: end of a 31-day month and of the year; day of week wraps 6 -> 0.
  {
    static const u8 a[7] = {0x25, 0x12, 0x31, 0x06, 0x23 | 0x80, 0x59, 0x59};
    static const u8 b[7] = {0x26, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00};
    CHECK(10, roll(a, b));
  }
  // 11: February 28 of a leap year (2000) goes to February 29.
  {
    static const u8 a[7] = {0x00, 0x02, 0x28, 0x01, 0x23 | 0x80, 0x59, 0x59};
    static const u8 b[7] = {0x00, 0x02, 0x29, 0x02, 0x00, 0x00, 0x00};
    CHECK(11, roll(a, b));
  }
  // 12: February 28 of a common year (2001) goes to March 1.
  {
    static const u8 a[7] = {0x01, 0x02, 0x28, 0x02, 0x23 | 0x80, 0x59, 0x59};
    static const u8 b[7] = {0x01, 0x03, 0x01, 0x03, 0x00, 0x00, 0x00};
    CHECK(12, roll(a, b));
  }
  // 13: April 30 goes to May 1.
  {
    static const u8 a[7] = {0x26, 0x04, 0x30, 0x03, 0x23 | 0x80, 0x59, 0x59};
    static const u8 b[7] = {0x26, 0x05, 0x01, 0x04, 0x00, 0x00, 0x00};
    CHECK(13, roll(a, b));
  }

  // 14-15: 12-hour mode: 11:59:59 AM -> 00:00:00 PM, 11:59:59 PM -> 00:00:00
  // AM of the next day.
  buf[0] = 0x00;
  rtc_write(0x62, buf, 1);
  {
    static const u8 a[7] = {0x26, 0x10, 0x23, 0x04, 0x11, 0x59, 0x59};
    static const u8 b[7] = {0x26, 0x10, 0x23, 0x04, 0x80, 0x00, 0x00};
    CHECK(14, roll(a, b));
  }
  {
    static const u8 a[7] = {0x26, 0x10, 0x23, 0x04, 0x11 | 0x80, 0x59, 0x59};
    static const u8 b[7] = {0x26, 0x10, 0x24, 0x05, 0x00, 0x00, 0x00};
    CHECK(15, roll(a, b));
  }

  // 16: the next second comes one second later, measured by the timer
  // (16384 ticks, within 1%).
  {
    u8 now[7];
    timer_start();
    u32 first = wait_second(now);
    u32 second = first ? wait_second(now) : 0;
    u32 delta = second - first;
    CHECK(16, first && second && delta > 16220 && delta < 16548);
  }
  return 0;
}
