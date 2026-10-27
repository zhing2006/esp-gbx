// The interrupt line from IE, IF and IME to the CPU, and HALT.
//
// IE & IF reach the CPU through two stages clocked by the system and IME
// through one; the CPU's own stage takes them when a CPU bus cycle begins.
// Wait states, a DMA the CPU waits for and HALT hold the CPU's clock, so
// they hold its stage too (alyosha Interactions/ReadMe.txt); a change that
// lasts a cycle passes through (alyosha irq/IE). The CPU takes the
// interrupt after an instruction if its stage was up when the
// instruction's last bus cycle began.
//
// The stages agree with nba-hw-test irq/irq-delay (IME), alyosha irq/IE,
// irq/BL_IRQ, irq/halt_pc, ppu/start_up_vbl_irq and Interactions.
//
// Clocking the CPU's stage at every access would cost the fast paths, so
// it is clocked only while the signals change (irq_watch_), and then only
// at the accesses from the next time its input may change on: up to then
// it takes what it holds. rearm() sets the deadline there, and the slow
// path calls irq_edge().

#include <algorithm>

#include "gbx/gba/memory.h"

namespace gbx::gba {
namespace {

// HALT ends this many cycles after IE & IF goes up, and lasts at least
// kHaltMinimum cycles (nba-hw-test haltcnt, alyosha irq/halt_pc).
constexpr Tick kHaltWake = 4, kHaltMinimum = 2;
// IE & IF pass two stages clocked by the system and IME one before the
// CPU's own stage takes them.
constexpr Tick kRequestDelay = 3, kMasterDelay = 2;

}  // namespace

void Memory::IrqHistory::push(Tick when, bool value) {
    if (last() == value) return;
    if (count == kSize) {
        before = values & 1;
        for (unsigned i = 1; i < kSize; ++i) time[i - 1] = time[i];
        values >>= 1;
        --count;
    }
    time[count] = when;
    values = static_cast<uint8_t>((values & ((1u << count) - 1)) | (value ? 1u << count : 0));
    ++count;
}

bool Memory::IrqHistory::at(Tick when) const {
    for (unsigned i = count; i-- > 0;)
        if (time[i] <= when) return (values >> i) & 1;
    return before;
}

void Memory::request_irq(uint16_t bits) {
    if (irq_recent_time_ != clock.now) {
        irq_recent_time_ = clock.now;
        irq_recent_ = 0;
    }
    irq_recent_ |= bits;
    irq_flags_ |= bits;
    mirror_if();
    update_irq();
}

// IME or IE & IF may have changed at clock.now; the caller rearms.
void Memory::update_irq() {
    const bool request = (ie() & irq_flags_ & 0x3fff) != 0, master = ime();
#ifdef GBX_GBA_RELAXED_TIMING
    // Relaxed: the line follows at once, without the stages' delay, and
    // the CPU takes the interrupt at the next instruction boundary; in
    // HALT, when it wakes (run_halt()'s caller).
    if (request && !irq_request_.last()) irq_request_rise_ = clock.now;
    irq_request_.reset(request);
    irq_line = request && master;
    if (irq_line && !(cpu->cpsr & kFlagI) && !halted) request_stop();
    return;
#endif
    const Tick time = irq_time(clock.now);
    if (request != irq_request_.last()) {
        irq_request_.push(time, request);
        if (request) irq_request_rise_ = clock.now;
        irq_next_ = irq_watch_ ? std::min(irq_next_, time + kRequestDelay) : time + kRequestDelay;
        irq_watch_ = true;
#ifdef GBX_PROFILER
        ++arm7_cycle_counts[26];
#endif
    }
    if (master != irq_master_.last()) {
        irq_master_.push(time, master);
        irq_next_ = irq_watch_ ? std::min(irq_next_, time + kMasterDelay) : time + kMasterDelay;
        irq_watch_ = true;
#ifdef GBX_PROFILER
        ++arm7_cycle_counts[25];
#endif
    }
}

// A CPU bus cycle begins at the line's time `time`, see irq_time(): its
// stage takes what the stages before it hold, the signals as they were
// kRequestDelay or kMasterDelay cycles earlier.
void Memory::irq_edge(Tick time) {
    const bool request = irq_request_.at(time - kRequestDelay), master = irq_master_.at(time - kMasterDelay);
    irq_line = request && master;
    if (irq_line && !(cpu->cpsr & kFlagI)) request_stop();
    // The next change to reach the CPU's stage; settled when none is left.
    Tick next = kNever;
    for (unsigned i = irq_request_.count; i-- > 0 && irq_request_.time[i] + kRequestDelay > time;)
        next = irq_request_.time[i] + kRequestDelay;
    for (unsigned i = irq_master_.count; i-- > 0 && irq_master_.time[i] + kMasterDelay > time;)
        next = std::min(next, irq_master_.time[i] + kMasterDelay);
    irq_next_ = next;
    irq_watch_ = next != kNever;
}

void Memory::reset_irq() {
    irq_flags_ = 0;
    mirror_if();
    irq_recent_ = 0;
    irq_recent_time_ = kNever;
    irq_request_.reset(false);
    irq_master_.reset(false);
    irq_request_rise_ = 0;
    irq_watch_ = false;
    irq_next_ = 0;
    halt_entry_ = kNever;
    irq_stalled_ = 0;
    irq_stall_from_ = kNever;
    halt_in_access_ = false;
    halt_tail_ = 0;
}

// HALT: the devices run until IE & IF wakes the CPU or the target comes.
// It stops the CPU's clock within the write to HALTCNT, the last bus cycle
// of its instruction.
void Memory::run_halt() {
    clock.advance(now());
    if (!wait_halt(true)) {
        rearm();
        return;
    }
    halted = false;
    rearm();
}

bool Memory::wait_halt(bool until_target) {
    if (halt_entry_ == kNever) halt_entry_ = clock.now;
    while (!wake_pending()) {
        if (until_target && clock.now >= target) return false;
        const Tick next = until_target ? std::min(clock.next, target) : clock.next;
        if (next == kNever) {
            // Nothing can wake the CPU: it stays until the target.
            halt_cycles += target - clock.now;
            clock.advance(target);
            halt_entry_ = kNever;
            return true;
        }
        halt_cycles += next - clock.now;
        clock.advance(next);
        if (dma_pending_) run_dma(kNever);
    }
    const Tick wake = std::max(halt_entry_ + kHaltMinimum, irq_request_rise_ + kHaltWake);
    if (wake > clock.now) {
        halt_cycles += wake - clock.now;
        clock.advance(wake);
        if (dma_pending_) run_dma(kNever);
    }
    halt_entry_ = kNever;
    return true;
}

}  // namespace gbx::gba
