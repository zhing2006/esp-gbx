#pragma once

#include <array>
#include <cstdint>

#include "gbx/common/scheduler.h"

namespace gbx::gb {
// Complete MBC3 clock state. The counters hold what their registers hold,
// including out-of-range values a program wrote (Pan Docs, MBC3; rtc3test
// tests.md, Range tests).
struct RtcState {
    uint8_t seconds = 0, minutes = 0, hours = 0;  // 6, 6 and 5 bits
    uint16_t days = 0;                            // 9 bits
    bool halt = false, carry = false;
    // Time since the last second, in ticks; below kTicksPerSecond. Ticks,
    // rather than 32768 Hz cycles, keep a saved state exact to the tick.
    uint32_t subsecond = 0;
    // S, M, H, DL and DH as the program last latched them.
    std::array<uint8_t, 5> latched{};
};

// The clock runs on emulated time and counts only when asked: every access
// first brings the counters up to the access time. It never schedules an
// event and never reads host time.
class Rtc {
public:
    void reset(const RtcState& state, Tick now) { set(state, now); last_latch_ = 0xff; }
    RtcState state(Tick now) { sync(now); return state_; }
    // Writes back a state taken at another time, counting on from now.
    void set(const RtcState& state, Tick now) { state_ = state; origin_ = now; }
    // Counts an interval that passed outside emulation, such as power off.
    // A halted clock does not count it.
    void advance(Tick elapsed, Tick now);
    // Register 0..4 is select value 08..0C. Unused bits read as zero.
    uint8_t read(unsigned reg) const { return state_.latched[reg]; }
    void write(unsigned reg, uint8_t value, Tick now);
    // Writing 00 and then 01 copies the counters into the latched registers.
    void latch(uint8_t value, Tick now);
private:
    friend struct SnapshotAccess;
    RtcState state_{};
    Tick origin_ = 0;  // Time the counters were last brought up to.
    uint8_t last_latch_ = 0xff;
    void sync(Tick now);
    void count(uint64_t seconds);
};
}  // namespace gbx::gb
