#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "gbx/common/scheduler.h"

namespace gbx::gba {

// The GBA's time unit is its CPU cycle, 16 * 1024 * 1024 per second (GBATEK
// "LCD Dimensions and Timings").
inline constexpr Tick kCyclesPerSecond = Tick{1} << 24;

// The S-3511's clock and control register, what a cartridge save keeps
// (GBATEK "GBA Cart Real-Time Clock (RTC)", which refers to "DS Real-Time
// Clock (RTC)" for the registers). The registers hold what was written, in
// BCD, within their bits. The hour counts 0-23 whatever the mode: 12-hour
// mode changes only how it is read and written.
struct RtcState {
    // Year, month, day, day of week, hour, minute, second.
    std::array<uint8_t, 7> datetime{0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00};
    // 82h after the battery was connected: 24-hour mode and the power-off
    // flag, which tells the program that the time was lost.
    uint8_t control = 0x82;
    // Cycles since the last second, below kCyclesPerSecond.
    uint32_t subsecond = 0;
};

// The chip behind the cartridge's GPIO port: SCK on bit 0, SIO on bit 1 and
// CS on bit 2 (GBATEK "GBA Cart I/O Port (GPIO)", Connection Examples).
//
// A transfer raises CS, sends a command byte and then sends or receives its
// parameter bytes, LSB first; dropping CS ends it. The chip takes SIO on
// the rising edge of SCK and drives it after the falling edge. The command
// byte is 0110 ccc r with its first bit on the wire as bit 7, which is the
// same sequence GBATEK's LSB-first "Fwd" codes give; a byte without the
// fixed 0110 is ignored until CS drops. ccc: 0 force reset, 1 control, 2
// date and time, 3 time, 4, 5 and 7 the alarm and free registers, which
// always read FFh, and 6 force IRQ.
//
// Interrupts are not emulated: GBATEK leaves open which edge of the
// inverted /IRQ output reaches the CPU and how long a forced pulse lasts,
// so the control register's interrupt bits and force IRQ change nothing
// but the bits' values.
//
// The clock counts emulated time and only when asked, as gb::Rtc does: it
// never schedules an event and never reads host time.
class Rtc {
public:
    // A chip in `state`, counting on from `now`, with no transfer under way.
    void reset(const RtcState& state, Tick now);
    RtcState state(Tick now) { sync(now); return state_; }
    // Writes back a state taken at another time, counting on from now.
    void set(const RtcState& state, Tick now) { state_ = state; origin_ = now; }
    // Counts an interval that passed outside emulation, such as power off.
    void advance(Tick elapsed, Tick now);
    // The pins as the GPIO drives them, SCK, SIO and CS in bits 0-2.
    void drive(uint8_t pins, Tick now);
    // SIO as the chip drives it, in bit 1; 0 while it does not.
    uint8_t sio() const { return output_; }
    // A transfer under way: CS high.
    bool busy() const { return phase_ != Phase::idle; }
    // Whether the program set the clock or the control register since the
    // last call, which a cartridge save keeps.
    bool take_written() { const bool w = written_; written_ = false; return w; }

private:
    friend struct SnapshotAccess;
    enum class Phase : uint8_t { idle, command, write, read, ignore };
    RtcState state_{};
    Tick origin_ = 0;  // Time the clock was last brought up to.
    Phase phase_ = Phase::idle;
    uint8_t pins_ = 0;      // The levels last driven.
    uint8_t command_ = 0;   // ccc of the transfer's command.
    uint8_t shift_ = 0;     // Bits of the byte being received.
    uint8_t bits_ = 0;      // Bits of the current byte sent or received.
    uint8_t index_ = 0;     // Parameter byte.
    uint8_t output_ = 0;
    bool written_ = false;
    std::array<uint8_t, 7> buffer_{};  // Parameters being read.
    void sync(Tick now);
    void count(uint64_t seconds);
    void begin(uint8_t command, Tick now);
    void receive(uint8_t value, Tick now);
    uint8_t read_hour() const;
};

// Whether the game with this code (ROM header 0ACh-0AFh) has the GPIO port
// and the RTC, from the list in gba_rtc.cpp.
bool cartridge_has_rtc(std::span<const uint8_t, 4> code);

}  // namespace gbx::gba
