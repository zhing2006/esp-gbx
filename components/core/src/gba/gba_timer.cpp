// The four timers (GBATEK "GBA Timers"), computed from the time: a counter
// is read from when it last started counting, and only the overflows that
// request an interrupt or make a sound FIFO ask for DMA are events. The
// samples the overflows take from a FIFO in between are counted when
// something next looks at the FIFO, see catch_up_fifos().
//
// A timer that counts by its prescaler ticks when the prescaler, which runs
// from power-on, crosses a multiple of 1, 64, 256 or 1024 cycles (alyosha
// timer/readme.txt), so its overflows follow at a fixed period once it
// started. A cascaded timer counts the overflows of the timer before it,
// so its overflows come every so many of those: each running timer's
// overflows are a progression, first + k * period, until a write to it or
// to a timer it counts from replans them.

#include "gbx/gba/memory.h"

namespace gbx::gba {
namespace {

constexpr uint8_t kPrescalerShift[4] = {0, 6, 8, 10};
// A write that starts a timer loads the reload value a cycle later, and
// the timer ticks from the cycle after; one that stops it lets the tick of
// its own cycle happen (alyosha irq/readme.txt). Reads see the counter of
// the cycle before (kReadLag): with these, nba-hw-test timer/start-stop and
// timer/reload agree, including a reload written in the cycle of an
// overflow, which that overflow does not use yet.
constexpr Tick kStartDelay = 1, kStopDelay = 0;

bool cascaded(unsigned index, uint16_t control) { return index && (control & 4); }

// Dividends and divisors here fit 32 bits but for very long runs and
// cascades: a 64-bit division is a libgcc call, many times slower.
uint64_t quotient(uint64_t n, uint64_t d) {
    return (n | d) >> 32 ? n / d : static_cast<uint32_t>(n) / static_cast<uint32_t>(d);
}
uint64_t remainder(uint64_t n, uint64_t d) {
    return (n | d) >> 32 ? n % d : static_cast<uint32_t>(n) % static_cast<uint32_t>(d);
}

}  // namespace

// Overflows of a timer in (from, to].
uint64_t Memory::overflows(unsigned index, Tick from, Tick to) const {
    const auto& t = timers_[index];
    if (t.first == kNever) return 0;
    if (to > t.end) to = t.end;
    if (to < t.first || to <= from) return 0;
    const uint64_t upto = quotient(to - t.first, t.period) + 1;
    const uint64_t before = from < t.first ? 0 : quotient(from - t.first, t.period) + 1;
    return upto - before;
}

// The next overflow strictly after `after`, or kNever.
Tick Memory::next_overflow(unsigned index, Tick after) const {
    const auto& t = timers_[index];
    if (t.first == kNever) return kNever;
    Tick when = t.first;
    if (after >= when) when += (quotient(after - when, t.period) + 1) * t.period;
    return when <= t.end ? when : kNever;
}

uint16_t Memory::timer_counter(unsigned index, Tick time) const {
    const auto& t = timers_[index];
    if (t.since == kNever) return t.counter;
    if (time < t.since) return t.held;
    if (time > t.end) time = t.end;
    uint64_t ticks;
    if (cascaded(index, t.control)) {
        ticks = overflows(index - 1, t.since, time);
    } else {
        const unsigned shift = kPrescalerShift[t.control & 3];
        ticks = ((time + prescaler_phase_) >> shift) - ((t.since + prescaler_phase_) >> shift);
    }
    uint64_t value = t.counter + ticks;
    if (value > 0xffff) value = t.reload + remainder(value - 0x10000, 0x10000 - t.reload);
    return static_cast<uint16_t>(value);
}

// The overflow progressions of the timers from `from` on, after their
// counters were brought to clock.now.
void Memory::plan_timers(unsigned from) {
    fifo_quiet_until_ = 0;
    for (unsigned i = from; i < 4; ++i) {
        auto& t = timers_[i];
        t.first = kNever;
        if (t.since == kNever) continue;
        const uint64_t needed = 0x10000 - t.counter;
        if (cascaded(i, t.control)) {
            const auto& previous = timers_[i - 1];
            const Tick next = next_overflow(i - 1, t.since);
            if (next == kNever) continue;
            const Tick first = next + (needed - 1) * previous.period;
            if (previous.end != kNever && first > previous.end) continue;
            t.first = first;
            t.period = (0x10000 - t.reload) * previous.period;
        } else {
            const unsigned shift = kPrescalerShift[t.control & 3];
            const Tick tick = ((((t.since + prescaler_phase_) >> shift) + 1) << shift) - prescaler_phase_;
            t.first = tick + ((needed - 1) << shift);
            t.period = static_cast<Tick>(0x10000 - t.reload) << shift;
        }
        if (t.first > t.end) t.first = kNever;
    }
    schedule_timers();
}

// A write to TMxCNT_L (the reload value, used at the next start or
// overflow) or TMxCNT_H at clock.now.
void Memory::write_timer(unsigned index, uint16_t reload, uint16_t control, bool control_written) {
    const Tick now = clock.now;
    // The FIFOs take their samples before the progressions change.
    if (index < 2) catch_up_sound(now);
    // Counters from this timer on are brought to now, as a change to this
    // one changes the progressions of those it cascades into.
    for (unsigned i = index; i < 4; ++i) {
        auto& t = timers_[i];
        t.handled = now;
        if (t.since == kNever) continue;
        const uint16_t value = timer_counter(i, now);
        if (t.end <= now) {
            t.counter = value;
            t.since = kNever;
            t.end = kNever;
        } else if (now >= t.since) {
            t.counter = t.held = value;
            t.since = now;
        }
    }
    auto& t = timers_[index];
    t.reload = reload;
    if (control_written) {
        const bool was = t.control & 0x80, is = control & 0x80;
        if (!was && is) {
            // Started: counts from the reload value after a delay, showing the
            // old counter until then (alyosha timer/timer_reset). A counter
            // at FFFFh still ticks before the reload and overflows: its
            // interrupt and cascade happen (nba-hw-test
            // timer/tick-before-reload).
            t.held = t.since == kNever ? t.counter : timer_counter(index, now);
            t.counter = reload;
            t.since = now + kStartDelay;
            t.end = kNever;
            if (t.held == 0xffff) {
                uint16_t requests = control & 0x40 ? static_cast<uint16_t>(irq::timer0 << index) : 0;
                if (index < 3 && (timers_[index + 1].control & 0x84) == 0x84 && timers_[index + 1].since != kNever) {
                    auto& next = timers_[index + 1];
                    if (++next.counter == 0) {
                        next.counter = next.reload;
                        if (next.control & 0x40) requests |= static_cast<uint16_t>(irq::timer0 << (index + 1));
                    }
                    next.held = next.counter;
                }
                raise_timer_irq(requests, now);
            }
        } else if (was && !is) {
            // Stopped: counts one more cycle (alyosha irq/readme.txt).
            t.end = now + kStopDelay;
        } else if (is && ((t.control ^ control) & 7)) {
            // The prescaler or cascade changed while running: counts on from now.
            if (t.since != kNever && now >= t.since) {
                t.counter = t.held = timer_counter(index, now);
                t.since = now;
            }
        }
        t.control = control;
    }
    plan_timers(index);
}

void Memory::schedule_timers() {
    // Only overflows with an effect are events: an interrupt, a sound FIFO
    // that asks for DMA, or a cascade into a timer that has one, which is
    // planned with that timer.
    const uint16_t sound = io16(0x82);
    const bool fifos = io_[0x84] & 0x80;
    Tick due = kNever;
    for (unsigned i = 0; i < 4; ++i) {
        auto& t = timers_[i];
        const bool irq = t.control & 0x40;
        const bool feeds = i < 2 && fifos && (((sound >> 10) & 1) == i || ((sound >> 14) & 1) == i);
        // Overflows nothing waits for are past once something does; those
        // that feed a FIFO stay to be counted (catch_up_fifos()).
        if (t.first == kNever || (!irq && !feeds)) {
            t.handled = clock.now;
            continue;
        }
        const Tick when = irq ? next_overflow(i, t.handled) : fifo_due(i);
        if (when < due) due = when;
    }
    if (timer_irq_time_ < due) due = timer_irq_time_;
    if (due == kNever) clock.cancel(kSlotTimer);
    else if (due > clock.now) clock.schedule(kSlotTimer, due, timer_event, this);
    else clock.schedule(kSlotTimer, clock.now + 1, timer_event, this);  // Due now: next cycle.
}

// The overflow of timer `index` after which a FIFO it feeds asks a DMA
// channel for more, see fifo_overflow(): with s bytes, the one that leaves
// fewer than 15, or the next one when it has fewer already. kNever when no
// channel waits to feed either FIFO.
Tick Memory::fifo_due(unsigned index) const {
    const uint16_t sound = io16(0x82);
    const auto& t = timers_[index];
    Tick due = kNever;
    for (unsigned which = 0; which < 2; ++which) {
        if (((sound >> (which ? 14 : 10)) & 1) != index || !fifo_fed(which)) continue;
        const Tick first = next_overflow(index, t.handled);
        if (first == kNever) return kNever;
        const unsigned size = fifo_[which].size;
        const Tick when = first + Tick{size >= 15 ? size - 15u : 0u} * t.period;
        if (when <= t.end && when < due) due = when;
    }
    return due;
}

// Whether a DMA channel waits to feed FIFO A (0) or B (1).
bool Memory::fifo_fed(unsigned which) const {
    const uint32_t address = which ? 0x040000a4 : 0x040000a0;
    for (unsigned c = 1; c <= 2; ++c)
        if ((io16(0xb0 + 12 * c + 10) & 0xb000) == 0xb000 && dma_[c].destination == address) return true;
    return false;
}

// The samples the FIFOs' timers took up to `to`, which no event counted.
// Events count those up to every request, so none is due here. The FIFO
// DMA's writes come here for every word: until the next overflow there is
// nothing to count. Only catch_up_sound() calls it, which keeps the
// sequencer's steps and the samples in time order; here the overflows of
// timers 0 and 1 go in time order too, timer 0's first at the same time.
void Memory::catch_up_fifos(Tick to) {
    if (!sound_on() || to < fifo_quiet_until_) return;
    unsigned fifos[2]{}, holding[2]{};
    uint64_t count[2]{};
    Tick next[2]{kNever, kNever};
    Tick quiet = kNever;
    for (unsigned i = 0; i < 2; ++i) {
        fifos[i] = fifos_of(i);
        if (!fifos[i]) continue;
        auto& t = timers_[i];
        if (to > t.handled) {
            count[i] = overflows(i, t.handled, to);
            if (count[i]) next[i] = next_overflow(i, t.handled);
            holding[i] = fifos_holding(fifos[i]);
            t.handled = to;
        }
        // Also for a timer an event handled up to `to` or later.
        const Tick after = next_overflow(i, t.handled);
        if (after < quiet) quiet = after;
    }
    const uint64_t counted[2]{count[0], count[1]};
    for (;;) {
        const bool zero = count[0] && holding[0], one = count[1] && holding[1];
        if (!zero && !one) break;
        const unsigned i = zero && (!one || next[0] <= next[1]) ? 0 : 1;
        holding[i] = fifo_samples(holding[i], next[i]);
        next[i] += timers_[i].period;
        --count[i];
    }
    for (unsigned i = 0; i < 2; ++i)
        if (counted[i]) fifo_requests(fifos[i]);
    fifo_quiet_until_ = quiet;
}

void Memory::timer_event(void* context) {
#ifdef GBX_PROFILER
    ++event_counts[1];
#endif
    auto& m = *static_cast<Memory*>(context);
    if (m.timer_irq_time_ <= m.clock.now) {
        const uint16_t requests = m.timer_irq_;
        m.timer_irq_ = 0;
        m.timer_irq_time_ = kNever;
        m.request_irq(requests);
    }
    m.handle_overflows(m.clock.now);
}

// An overflow's interrupt request is raised in IF the cycle after it
// (alyosha irq/IF).
void Memory::raise_timer_irq(uint16_t requests, Tick time) {
    if (!requests) return;
    timer_irq_ |= requests;
    timer_irq_time_ = time + 1;
}

// Overflows at `time`: their interrupts and sound FIFOs. The sound before
// `time` goes first, so that the samples follow the sequencer's steps.
void Memory::handle_overflows(Tick time) {
    if (time) catch_up_sound(time - 1);
    fifo_quiet_until_ = 0;
    const bool fifos = sound_on();
    uint16_t requests = 0;
    for (unsigned i = 0; i < 4; ++i) {
        auto& t = timers_[i];
        const uint64_t count = overflows(i, t.handled, time);
        const Tick first = count && i < 2 && fifos ? next_overflow(i, t.handled) : kNever;
        t.handled = time;
        if (!count) continue;
        if (t.control & 0x40) requests |= static_cast<uint16_t>(irq::timer0 << i);
        if (i < 2 && fifos) fifo_overflow(i, count, first, t.period);
    }
    raise_timer_irq(requests, time);
    schedule_timers();
}

}  // namespace gbx::gba
