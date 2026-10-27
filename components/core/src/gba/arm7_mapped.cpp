// The interpreter on the GBA's memory map: Executor with MappedBus, the
// loop that handles the stops the memory requests at instruction
// boundaries, and the single steps of the JIT's dispatcher. With the JIT
// runtime it forms the GBA's execution core in internal RAM
// (components/core/linker.lf). It is the only file that instantiates
// Executor<MappedBus>: the linker keeps one object's copy of a template's
// functions, and another object in flash would take them out of internal
// RAM.

#include "arm7_spin.h"
#include "executor.h"
#include "gbx/gba/system.h"
#include "mapped_bus.h"

namespace gbx::gba {

// A wait loop in the interpreter. Two passes in a row that start in the
// same state, with nothing but the loop's own instructions between them,
// are followed by passes alike. Nothing else ran when the epoch stayed and
// the branch back was the loop's own both times: any other way back to the
// loop writes R15 by a backward branch from elsewhere, which starts over,
// or otherwise, which moves the epoch (spin_epoch). The state is the CPU's
// registers and pipeline, the prefetch buffer's timing relative to now
// (only its head while it is stopped) and the gamepak's next address,
// which is all a pass of a loop that only reads RAM depends on. Those that
// would end with cycles left are skipped at once, the buffer's times moving
// with the clock; the last one runs, so that an event comes due at the
// same boundary as without skipping.
void Memory::spin_loop(uint32_t head, bool thumb) {
    auto& s = spin_;
    if (s.verdict == 0) {
        const auto read = [this, thumb](uint32_t address) -> uint32_t {
            const unsigned region = address >> 24;
            if (region != 0 && region != 2 && region != 3 && (region < 8 || region > 13)) return ~0u;
            return peek(address, thumb ? 1 : 2);
        };
        const unsigned count = arm7_spin_loop(head, thumb, read);
        s.verdict = count && head + (count - 1) * (thumb ? 2 : 4) == s.branch ? 1 : 2;
        if (s.verdict == 2) return;
    }
    // The state replaces the one kept, compared on the way.
    uint32_t differs = !s.kept;
    const auto keep = [&s, &differs](unsigned i, uint32_t value) {
        differs |= s.state[i] ^ value;
        s.state[i] = value;
    };
    for (unsigned i = 0; i < 16; ++i) keep(i, cpu->r[i]);
    keep(16, cpu->cpsr);
    keep(17, cpu->pipeline[0]);
    keep(18, cpu->pipeline[1]);
    keep(19, cpu->sequential);
    // A stopped buffer's times and flags are not read before it starts
    // again, which sets them; with relaxed timing it never starts.
    const auto& p = prefetch_;
    keep(20, p.head);
    if (p.head) {
        const uint32_t time = static_cast<uint32_t>(now());
        keep(21, p.base - time);
        keep(22, p.start - time);
        keep(23, p.paused | p.cycles << 8 | uint32_t{p.running} << 16 | uint32_t{p.stalled} << 24);
    } else {
        keep(21, 0);
        keep(22, 0);
        keep(23, 0);
    }
    keep(24, gamepak_next_);
    const int32_t cycles = s.left - left;
    const bool same = !differs;
    if (same && cycles > 0 && cycles == s.cycles && left > cycles) {
        const int32_t skipped = (left - 1) / cycles * cycles;
        left -= skipped;
        prefetch_.base += static_cast<uint32_t>(skipped);
        prefetch_.start += static_cast<uint32_t>(skipped);
    }
    s.cycles = same ? cycles : 0;
    s.kept = true;
    s.left = left;
}

void arm7_run_mapped(Arm7State& cpu, Memory& m) {
    const MappedBus bus{m};
    Executor<MappedBus> executor(cpu, bus);
    for (;;) {
        // A loop for each state, which runs on while it lasts, so that the
        // common path through each is straight: every taken branch costs
        // the core a slot in its 16-entry BTB (docs/esp32p4-performance.md).
        if (cpu.cpsr & kFlagT) {
            while (m.left > 0 && (cpu.cpsr & kFlagT)) [[likely]] Executor<MappedBus>::run_thumb(cpu, bus);
        } else {
            while (m.left > 0 && !(cpu.cpsr & kFlagT)) [[likely]] Executor<MappedBus>::run_arm(cpu, bus);
        }
        if (m.left > 0) continue;  // The state changed.
        if (!m.stop) {
            // Events are due, or the target came: dispatched here, at the
            // time the next fetch would. When the countdown stays out (the
            // bus is checked after a DMA), the instruction's accesses
            // catch up themselves.
            m.catch_up_boundary();
            if (!m.stop) {
                executor.step();
                continue;
            }
        }
        m.stop = false;
        if (m.halted) {
            m.run_halt();
            if (m.halted) return;  // The target came first.
        }
        if (m.irq_line && (!(cpu.cpsr & kFlagI) || m.irq_unmasked)) {
            executor.interrupt();
        }
        m.irq_unmasked = false;
        if (m.now() >= m.target) {
            m.stop = true;
            return;
        }
        // The interrupt's entry took cycles since the clock last caught up.
        m.catch_up_boundary();
    }
}

// Executor keeps a reference to its bus. A step runs one instruction: the
// epoch moves first, so that it skips no passes of a wait loop.
void arm7_step_mapped(Arm7State& cpu, Memory& m) {
    const MappedBus bus{m};
    Executor<MappedBus> executor(cpu, bus);
    ++m.spin_epoch;
    executor.step();
}

void arm7_interrupt_mapped(Arm7State& cpu, Memory& m) {
    const MappedBus bus{m};
    Executor<MappedBus> executor(cpu, bus);
    executor.interrupt();
}

}  // namespace gbx::gba
