// The cartridge's GPIO port and the S-3511 RTC behind it (GBATEK "GBA Cart
// I/O Port (GPIO)", "GBA Cart Real-Time Clock (RTC)" and "DS Real-Time
// Clock (RTC)"), and the list of games that have them.
//
// The port's registers sit in the ROM's address range, 80000C4h-80000C9h.
// While the control register makes them readable, page 0 of the ROM cache
// maps to a copy of the ROM's first page with the registers' values over
// those six bytes (RomCache::set_overlay()), so the interpreter's and the
// JIT's ROM paths and DMA read them with no check of their own. The ROM
// chip sees no wait-state region, so the registers also answer at
// A0000C4h and C0000C4h, which map to the same page. Writes to the ROM
// range reach write_region() in any case, where only games with the port
// look at them.

#include <cstring>

#include "gbx/gba/memory.h"
#include "gbx/platform.h"

namespace gbx::gba {
namespace {

// Games with the port and the RTC, by game code. The RTC's presence comes
// from cartridge hardware documentation, the codes from the "Serial" field
// of No-Intro's records (datomatic.no-intro.org, Nintendo - Game Boy
// Advance, record numbers below). Sources of the RTC:
//   [G] Game Boy hardware database, cartridge PCB pages
//       (gbhwdb.gekkio.fi/cartridges/AGB-<code>-<revision>/): "RTC (U3)
//       S-3511A, Seiko Instruments Inc.".
//   [C] ConsoleMods wiki, "GBA: Real Time Clock (RTC) Fix", Games Affected:
//       Pokemon Ruby, Sapphire and Emerald, Boktai 1, 2 and 3.
//   [N] NintendoWiki, "Real-Time Clock" (niwanetwork.org): Pokemon Ruby,
//       Sapphire and Emerald, and Sennen Kazoku, which cannot be played
//       when the internal battery is not functioning. It does not name
//       the chip.
//   [T] GBATEK, "GBA Cart Real-Time Clock (RTC)": the S3511 is "used in
//       Boktai"; RTC Games: Boktai series, Rockman EXE 4.5 Real Operation.
// The English European releases of Pokemon hold the USA ROM, whose code
// ends in E (No-Intro "USA, Europe").
constexpr char kRtcGames[][5] = {
    // Pokemon Ruby: J #0744 #2907, E #0891 #1223 #1981, D #1089 #1959,
    // F #1097 #1955, I #1087 #1892, S #1093 #1741. [G] [C] [N]
    "AXVJ", "AXVE", "AXVD", "AXVF", "AXVI", "AXVS",
    // Pokemon Sapphire: J #0741 #2906, E #0882 #1218 #1986, D #1075 #1960,
    // F #1094 #1954, I #1088 #1893, S #1092 #1742. [G] [C] [N]
    "AXPJ", "AXPE", "AXPD", "AXPF", "AXPI", "AXPS",
    // Pokemon Emerald: J #1629, E #1961, D #2159, F #2161, I #2181,
    // S #2164. [G] [C] [N]
    "BPEJ", "BPEE", "BPED", "BPEF", "BPEI", "BPES",
    // Bokura no Taiyou / Boktai: J #1085, E #1143, P #1463. [G] [C] [T]
    "U3IJ", "U3IE", "U3IP",
    // Zoku Bokura no Taiyou / Boktai 2: J #1565 #2365, E #1695, P #1988.
    // [G] [C] [T]
    "U32J", "U32E", "U32P",
    // Shin Bokura no Taiyou: Gyakushuu no Sabata: J #2044. [G] [C]
    "U33J",
    // Rockman EXE 4.5 Real Operation: J #1584. [G] [T]
    "BR4J",
    // Sennen Kazoku: J #1908 #2860. [N]
    "BKAJ",
    // The RTC test ROM of task 1-2, tests/gba/roms/rtc/rtc.c, by its own code.
    "0RTC",
};

constexpr uint8_t kDatetimeMasks[7] = {0xff, 0x1f, 0x3f, 0x07, 0x3f, 0x7f, 0x7f};
// Parameter bytes by command: force reset, control, date and time, time,
// then the alarm and free registers, which read FFh, and force IRQ.
constexpr uint8_t kParameters[8] = {0, 1, 7, 3, 0, 0, 0, 0};
// Writable bits of the control register: 1, 3, 5 and 6 (GBATEK "GBA Cart
// Real-Time Clock (RTC)", Control Register). Bit 7 is the power-off flag.
constexpr uint8_t kControlBits = 0x6a;
constexpr uint8_t kPowerOff = 0x80, k24Hour = 0x40;

unsigned decimal(uint8_t bcd) { return (bcd >> 4) * 10 + (bcd & 15); }
uint8_t bcd(unsigned value) { return static_cast<uint8_t>((value / 10) << 4 | value % 10); }

// Days of a month, 1-12, of year 2000 + year: every fourth year is a leap
// year, as 2000-2099 are by the Gregorian rule.
unsigned month_days(unsigned month, unsigned year) {
    static constexpr uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 31;
    return kDays[month - 1] + (month == 2 && year % 4 == 0);
}

}  // namespace

bool cartridge_has_rtc(std::span<const uint8_t, 4> code) {
    for (const auto& game : kRtcGames)
        if (std::memcmp(game, code.data(), 4) == 0) return true;
    return false;
}

// --- The S-3511 ------------------------------------------------------------------

void Rtc::reset(const RtcState& state, Tick now) {
    set(state, now);
    phase_ = Phase::idle;
    pins_ = shift_ = bits_ = index_ = output_ = 0;
}

void Rtc::sync(Tick now) {
    const Tick total = state_.subsecond + (now - origin_);
    state_.subsecond = static_cast<uint32_t>(total & (kCyclesPerSecond - 1));
    origin_ = now;
    if (total >= kCyclesPerSecond) count(total >> 24);
}

void Rtc::advance(Tick elapsed, Tick now) {
    sync(now);
    const Tick total = state_.subsecond + elapsed;
    state_.subsecond = static_cast<uint32_t>(total & (kCyclesPerSecond - 1));
    count(total >> 24);
}

// GBATEK gives the registers' ranges but not what the chip does with a
// value outside them. Here a register beyond its last value goes to its
// first at its next count and carries, as from the last value: one
// second at a time until the time of day is valid, then whole intervals at
// once, as power-off compensation needs. The date counts a day at a time
// over at most one 100-year cycle of years 00-99, whose 36525 days bring a
// valid date back to itself.
void Rtc::count(uint64_t seconds) {
    auto& t = state_.datetime;
    uint64_t days = 0;
    unsigned second = decimal(t[6]), minute = decimal(t[5]), hour = decimal(t[4]);
    for (; seconds && (second >= 60 || minute >= 60 || hour >= 24); --seconds) {
        if (++second < 60) continue;
        second = 0;
        if (++minute < 60) continue;
        minute = 0;
        if (++hour < 24) continue;
        hour = 0;
        ++days;
    }
    if (seconds) {
        const uint64_t total = second + 60 * (minute + 60 * hour) + seconds;
        days += total / 86400;
        const auto rest = static_cast<unsigned>(total % 86400);
        second = rest % 60;
        minute = rest / 60 % 60;
        hour = rest / 3600;
    }
    t[6] = bcd(second);
    t[5] = bcd(minute);
    t[4] = bcd(hour);
    if (!days) return;
    // The day of week counts 0-6; 7 goes to 0 at its first count.
    t[3] = static_cast<uint8_t>(t[3] > 6 ? (days - 1) % 7 : (t[3] + days) % 7);
    unsigned year = decimal(t[0]), month = decimal(t[1]), day = decimal(t[2]);
    const bool valid = year < 100 && month >= 1 && month <= 12 && day >= 1 && day <= month_days(month, year);
    if (valid) days %= 36525;
    for (; days; --days) {
        if (++day <= month_days(month, year)) continue;
        day = 1;
        if (++month <= 12) continue;
        month = 1;
        if (++year >= 100) year = 0;
    }
    t[0] = bcd(year);
    t[1] = bcd(month);
    t[2] = bcd(day);
}

// GBATEK, DS RTC Hour Register (bit 6 there is bit 7 on the GBA): in
// 24-hour mode 00h-23h with the PM flag read only, set for 12h-23h; in
// 12-hour mode 00h-11h with the PM flag, 12 o'clock being 00h.
uint8_t Rtc::read_hour() const {
    const unsigned hour = decimal(state_.datetime[4]);
    const uint8_t pm = hour >= 12 ? 0x80 : 0;
    if (state_.control & k24Hour) return state_.datetime[4] | pm;
    return bcd(hour >= 12 ? hour - 12 : hour) | pm;
}

void Rtc::drive(uint8_t pins, Tick now) {
    const uint8_t old = pins_;
    pins_ = pins;
    if (!(pins & 4)) {
        phase_ = Phase::idle;
        output_ = 0;
        return;
    }
    if (!(old & 4)) {
        // CS rises: a command byte follows.
        phase_ = Phase::command;
        shift_ = bits_ = 0;
        output_ = 0;
        return;
    }
    const bool sio = pins & 2;
    if ((old & 1) && !(pins & 1)) {
        // Falling edge: the chip drives the next bit it sends.
        if (phase_ == Phase::read) {
            const uint8_t byte = index_ < kParameters[command_] ? buffer_[index_] : 0xff;
            output_ = (byte >> bits_) & 1 ? 2 : 0;
        }
        return;
    }
    if ((old & 1) || !(pins & 1)) return;
    // Rising edge: the chip takes SIO, or the CPU took the bit sent.
    switch (phase_) {
    case Phase::command:
        shift_ = static_cast<uint8_t>(shift_ << 1 | sio);
        if (++bits_ == 8) begin(shift_, now);
        break;
    case Phase::write:
        shift_ |= static_cast<uint8_t>(sio << bits_);
        if (++bits_ == 8) {
            if (index_ < kParameters[command_]) receive(shift_, now);
            ++index_;
            shift_ = bits_ = 0;
        }
        break;
    case Phase::read:
        if (++bits_ == 8) {
            ++index_;
            bits_ = 0;
        }
        break;
    default: break;
    }
}

// A command byte received. Force reset and force IRQ act on any access,
// read or write (GBATEK, Force Reset/Irq Registers).
void Rtc::begin(uint8_t command, Tick now) {
    bits_ = index_ = shift_ = 0;
    if ((command & 0xf0) != 0x60) {
        phase_ = Phase::ignore;
        return;
    }
    command_ = (command >> 1) & 7;
    const bool read = command & 1;
    phase_ = read ? Phase::read : Phase::write;
    sync(now);
    auto& s = state_;
    switch (command_) {
    case 0:
        // All registers 00h but day and month, 01h.
        s.datetime = {0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00};
        s.control = 0;
        written_ = true;
        break;
    case 1:
        if (read) {
            buffer_[0] = s.control;
            s.control &= ~kPowerOff;  // Cleared by the read.
        }
        break;
    case 2:
        if (read) {
            buffer_ = s.datetime;
            buffer_[4] = read_hour();
        }
        break;
    case 3:
        if (read) buffer_ = {read_hour(), s.datetime[5], s.datetime[6]};
        break;
    default: break;
    }
}

// A parameter byte written, which takes effect at once.
void Rtc::receive(uint8_t value, Tick now) {
    sync(now);
    written_ = true;
    auto& s = state_;
    if (command_ == 1) {
        s.control = static_cast<uint8_t>((s.control & kPowerOff) | (value & kControlBits));
        return;
    }
    const unsigned field = index_ + (command_ == 3 ? 4 : 0);
    // The PM flag counts only in 12-hour mode.
    if (field == 4 && !(s.control & k24Hour) && (value & 0x80)) value = bcd(decimal(value & 0x3f) + 12);
    s.datetime[field] = value & kDatetimeMasks[field];
}

// --- The port ----------------------------------------------------------------------

// A write in 80000C4h-80000C9h: data, direction and control by halfword. A
// word writes both of its halfwords. A byte puts its value on both halves of
// the 16-bit bus, which the port takes as a halfword; GBATEK reports STRB
// as ignored, but asks whether that holds only in DS mode.
void Memory::write_gpio(uint32_t address, unsigned width, uint32_t value) {
    const uint32_t offset = address & 0x1fffffe;
    if (width == 2) {
        write_gpio16(offset & ~2u, static_cast<uint16_t>(value));
        write_gpio16((offset & ~2u) + 2, static_cast<uint16_t>(value >> 16));
    } else {
        write_gpio16(offset, static_cast<uint16_t>(width ? value : (value & 0xff) * 0x0101u));
    }
}

void Memory::write_gpio16(uint32_t offset, uint16_t value) {
    switch (offset) {
    case 0xc4: gpio_.data = value & 15; break;
    case 0xc6: gpio_.direction = value & 15; break;
    case 0xc8: gpio_.control = value & 1; break;
    default: return;
    }
    rtc.drive(gpio_.data & gpio_.direction & 7, clock.now);
    update_gpio();
}

// The readable registers into the copy of page 0, or the ROM's own page
// while they are write-only. Pins set as outputs read what was written,
// the inputs what the chip drives.
void Memory::update_gpio() {
    if (!(gpio_.control & 1)) {
        rom->set_overlay(nullptr);
        return;
    }
    const uint8_t data = (gpio_.data & gpio_.direction) | (rtc.sio() & ~gpio_.direction);
    const uint8_t registers[6] = {data, 0, gpio_.direction, 0, gpio_.control, 0};
    std::memcpy(gpio_page_ + 0xc4, registers, sizeof registers);
    rom->set_overlay(gpio_page_);
}

// For a game with the port: a copy of the ROM's first page to read the
// registers through. False without the memory for it.
bool Memory::load_gpio() {
    // Page 0 as the ROM has it, not this memory's copy from a run before.
    if (gpio_page_) rom->end_overlay(gpio_page_);
    uint8_t code[4];
    for (unsigned i = 0; i < 4; ++i) code[i] = rom->byte(0xac + i);
    if (!cartridge_has_rtc(code)) {
        platform::free_external(gpio_page_);
        gpio_page_ = nullptr;
        return true;
    }
    if (!gpio_page_) gpio_page_ = static_cast<uint8_t*>(platform::allocate_external(RomCache::kPageBytes));
    if (!gpio_page_) return false;
    std::memcpy(gpio_page_, rom->map(0), RomCache::kPageBytes);
    rtc.reset({}, clock.now);
    return true;
}

}  // namespace gbx::gba
