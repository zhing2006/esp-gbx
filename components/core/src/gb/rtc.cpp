#include "gbx/gb/rtc.h"

namespace gbx::gb {
namespace {
// Valid bits of S, M, H, DL and DH (Pan Docs, MBC3; rtc3test Valid bits).
constexpr uint8_t kMasks[]{0x3f, 0x3f, 0x1f, 0xff, 0xc1};
}
void Rtc::sync(Tick now) {
    if (!state_.halt) {
        const Tick total = state_.subsecond + (now - origin_);
        state_.subsecond = static_cast<uint32_t>(total % kTicksPerSecond);
        count(total / kTicksPerSecond);
    }
    origin_ = now;
}
void Rtc::advance(Tick elapsed, Tick now) {
    sync(now);
    if (state_.halt) return;
    const Tick total = state_.subsecond + elapsed;
    state_.subsecond = static_cast<uint32_t>(total % kTicksPerSecond);
    count(total / kTicksPerSecond);
}
// A counter at its last valid value carries into the next one. A value
// beyond it counts on to the top of its bits and wraps to zero without a
// carry (rtc3test tests.md: Invalid value tick, Invalid rollovers, High
// minutes and High hours). The day counter sets the sticky carry flag when it
// wraps from 511.
void Rtc::count(uint64_t seconds) {
    auto& s = state_;
    const auto tick = [&s] {
        if (s.seconds != 59) { s.seconds = (s.seconds + 1) & 63; return; }
        s.seconds = 0;
        if (s.minutes != 59) { s.minutes = (s.minutes + 1) & 63; return; }
        s.minutes = 0;
        if (s.hours != 23) { s.hours = (s.hours + 1) & 31; return; }
        s.hours = 0;
        if (s.days == 511) s.carry = true;
        s.days = (s.days + 1) & 511;
    };
    // Out-of-range values tick one second at a time until they wrap, then
    // whole intervals are counted at once, as power-off compensation needs.
    for (; seconds && (s.seconds >= 60 || s.minutes >= 60 || s.hours >= 24); --seconds) tick();
    if (!seconds) return;
    uint64_t total = s.seconds + 60 * (s.minutes + 60 * (s.hours + uint64_t{24} * s.days)) + seconds;
    s.seconds = total % 60; total /= 60;
    s.minutes = total % 60; total /= 60;
    s.hours = total % 24; total /= 24;
    if (total >= 512) s.carry = true;
    s.days = total % 512;
}
// rtc3test tests.md, Sub-second writes: writing the seconds restarts the
// current second; the other registers leave it running, and a halt pauses
// it until the clock runs again.
void Rtc::write(unsigned reg, uint8_t value, Tick now) {
    sync(now);
    value &= kMasks[reg];
    auto& s = state_;
    switch (reg) {
    case 0: s.seconds = value; s.subsecond = 0; break;
    case 1: s.minutes = value; break;
    case 2: s.hours = value; break;
    case 3: s.days = (s.days & 256) | value; break;
    default:
        s.days = (s.days & 255) | (value & 1) << 8;
        s.halt = value & 0x40; s.carry = value & 0x80;
        break;
    }
}
void Rtc::latch(uint8_t value, Tick now) {
    if (last_latch_ == 0 && value == 1) {
        sync(now);
        const auto& s = state_;
        state_.latched = {s.seconds, s.minutes, s.hours, static_cast<uint8_t>(s.days),
            static_cast<uint8_t>((s.days >> 8) | (s.halt ? 0x40 : 0) | (s.carry ? 0x80 : 0))};
    }
    last_latch_ = value;
}
}  // namespace gbx::gb
