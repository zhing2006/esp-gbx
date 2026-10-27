#pragma once

// The out-of-line members of BasicScheduler, for the one file of each
// system that instantiates its scheduler: common/scheduler.cpp for GB's,
// gba/scheduler.cpp for the GBA's. Each copy goes with its system's hot
// code, see components/core/linker.lf.

#include <cassert>

#include "gbx/common/scheduler.h"

namespace gbx {
template<unsigned Slots>
void BasicScheduler<Slots>::reset() {
    now = 0; next = kNever; generation = 0; next_slot_ = 0; stall_ = 0;
    events_.fill({});
}
template<unsigned Slots>
void BasicScheduler<Slots>::dispatch(Tick target) {
    assert(target >= now);
    for (;;) {
        while (next != kNever && next <= target) {
            now = next;
            auto& event = events_[next_slot_];
            const auto pending = event;
            // Remove the dispatched deadline before the callback installs
            // its successor, which then updates the minimum without a rescan.
            event = {};
            update_next();
            ++generation;
            pending.callback(pending.context);
        }
        // A pause events asked for moves the target, see stall(); then the
        // events within it run too.
        if (!stall_) break;
        target += stall_; stall_ = 0;
    }
    now = target;
}
}  // namespace gbx
