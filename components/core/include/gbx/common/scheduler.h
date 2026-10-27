#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>

namespace gbx {

// Absolute 64-bit time avoids wrapping during long ROM runs. Each system
// picks its unit: GB and GBC count half a DMG dot (a normal-speed CPU
// T-cycle is two ticks, CGB double speed takes one), see kTicksPerSecond;
// the GBA counts its 16.78 MHz CPU cycles, see gba::kCyclesPerSecond.
using Tick = uint64_t;
inline constexpr Tick kTicksPerSecond = 8'388'608;
inline constexpr Tick kNever = std::numeric_limits<Tick>::max();

// Fixed slots fit the small number of independently scheduled devices: the
// DMG uses slots 0..3 for the PPU, timer, serial port and OAM DMA, and the
// GBA has its own count. Each slot scanned costs every event, so there are
// no spare ones; device tests reuse slots after resetting the clock. The
// next deadline and its slot are kept, so dispatch rescans the slots once
// per event and rescheduling the earliest event rescans only when it moves
// later. Callbacks run on the CPU owner, at their timestamp, before a
// same-time access; same-time events run in slot order. A callback must
// cancel itself or schedule strictly after the current time.
//
// The slot count is a template parameter so that the GBA's devices have
// slots without the GB scanning them: each system instantiates its own,
// GB's in scheduler.cpp, see scheduler_dispatch.h.
template<unsigned Slots>
class BasicScheduler {
public:
    using Callback = void (*)(void*);
    static constexpr unsigned kSlots = Slots;
    Tick now = 0;
    Tick next = kNever;
    uint32_t generation = 0;

    void reset();
    void schedule(unsigned slot, Tick when, Callback callback, void* context) {
        assert(slot < kSlots && when > now && callback);
        const auto previous = events_[slot].when;
        events_[slot] = {when, callback, context};
        if (when < next || (when == next && slot < next_slot_)) { next = when; next_slot_ = slot; }
        else if (slot == next_slot_ && when > previous) update_next();
    }
    void cancel(unsigned slot) {
        assert(slot < kSlots);
        events_[slot] = {};
        if (slot == next_slot_) update_next();
    }
    Tick deadline(unsigned slot) const { return events_[slot].when; }
    Callback callback(unsigned slot) const { return events_[slot].callback; }
    // Restores a saved timeline: the time and each slot's deadline, kNever
    // for none. A deadline may be due already, as a batched JIT run defers
    // some events to a limit, see Memory::deferral_limit().
    void restore(Tick time, const std::array<Tick, kSlots>& deadlines,
                 const std::array<Callback, kSlots>& callbacks, const std::array<void*, kSlots>& contexts) {
        now = time;
        for (unsigned slot = 0; slot < kSlots; ++slot)
            events_[slot] = deadlines[slot] == kNever ? Event{} : Event{deadlines[slot], callbacks[slot], contexts[slot]};
        update_next();
    }
    void advance(Tick target) {
        if (target >= next) dispatch(target);
        else now = target;
    }
    // Hold the CPU for `ticks` while the devices run on, as the CGB's VRAM
    // DMA does (Pan Docs, LCD VRAM DMA Transfers). Either counts as an event:
    // whoever keeps a window to the next event, such as the JIT, recomputes
    // it. stall() is for an event: the advance under way ends that much
    // later, so the access in progress completes after the pause. delay()
    // is for an access, between advances: the pause passes now.
    void stall(Tick ticks) { stall_ += static_cast<uint32_t>(ticks); ++generation; }
    void delay(Tick ticks) { ++generation; dispatch(now + ticks); }
private:
    struct Event { Tick when = kNever; Callback callback = nullptr; void* context = nullptr; };
    std::array<Event, kSlots> events_{};
    unsigned next_slot_ = 0;
    uint32_t stall_ = 0;  // In the padding, so the DMG's layout stays.
    void update_next() {
        Tick when = events_[0].when;
        unsigned slot = 0;
        for (unsigned i = 1; i < kSlots; ++i)
            if (events_[i].when < when) { when = events_[i].when; slot = i; }
        next = when; next_slot_ = slot;
    }
    void dispatch(Tick target);
};

using Scheduler = BasicScheduler<4>;
extern template class BasicScheduler<4>;

}  // namespace gbx
