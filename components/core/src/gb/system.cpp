#include "gbx/gb/system.h"

#include <algorithm>
#include <bit>
#include <new>

#include "gbx/gb/header.h"

namespace gbx::gb {
namespace {
[[gnu::always_inline]] inline JitResult interpreter_block(Sm83State& cpu, Memory& memory, Tick until, unsigned limit,
                                                     bool marker) {
    const auto generation = memory.clock.generation;
    const auto block = memory.double_speed() ? sm83_run_double_speed(cpu, memory, until, limit, marker)
                                             : sm83_run_block(cpu, memory, until, limit, marker);
    auto reason = JitExit::limit;
    if (marker && block.instructions && block.opcode == 0x40) reason = JitExit::breakpoint;
    else if (memory.clock.generation != generation) reason = JitExit::event;
    else if (block.suspended) reason = JitExit::suspended;
    return {block.instructions, block.mcycles, reason, JitFallback::none};
}
}  // namespace
bool System::load_rom(std::span<const uint8_t> rom, bool stop_on_marker, Mode mode) {
    const auto previous_backend = backend_;
    if (!memory.initialize() || !memory.load(rom, mode)) return false;
    if (marker_ != stop_on_marker) { jit_.reset(); backend_ = Backend::interpreter; }
    marker_ = stop_on_marker;
    reset();
    return select_backend(previous_backend);
}
void System::reset() {
    memory.reset();
    cpu = {};
    // Pan Docs, Power Up Sequence: CPU registers.
    const auto rom = memory.rom();
    switch (memory.mode()) {
    case Mode::dmg:
        cpu.a = 1;
        // H/C depend on the cartridge header checksum.
        cpu.f = rom.empty() || rom[0x14d] ? 0xb0 : 0x80;
        cpu.set_bc(0x0013); cpu.set_de(0x00d8); cpu.set_hl(0x014d);
        break;
    case Mode::cgb:
        cpu.a = 0x11; cpu.f = 0x80;
        cpu.set_bc(0x0000); cpu.set_de(0xff56); cpu.set_hl(0x000d);
        break;
    case Mode::compat:
        // B holds the title checksum for a Nintendo cartridge, and HL where
        // the boot ROM left off writing the logo's tile map, see Pan Docs.
        cpu.a = 0x11; cpu.f = 0x80;
        cpu.b = nintendo_licensee(rom) ? title_checksum(rom) : 0;
        cpu.c = 0; cpu.set_de(0x0008);
        cpu.set_hl(cpu.b == 0x43 || cpu.b == 0x58 ? 0x991a : 0x007c);
        break;
    }
    cpu.sp = 0xfffe; cpu.pc = 0x100;
    if (jit_) { jit_->discard_continuation(); jit_->clear(); }
}
bool System::select_backend(Backend backend, const platform::ExecutableMemory* reserved) {
    if (backend == Backend::jit && !jit_) {
        jit_.reset(new (std::nothrow) Sm83Jit);
        if (!jit_ || !jit_->initialize(memory, marker_, jit_capacity_, reserved)) { jit_.reset(); return false; }
    }
    backend_ = backend;
    memory.trace(trace_, backend == Backend::jit);
    if (jit_) jit_->trace(trace_ != nullptr);
    return true;
}
void System::set_trace(TraceBuffer* sink) {
    trace_ = sink;
    memory.trace(sink, backend_ == Backend::jit);
    if (jit_) jit_->trace(sink != nullptr);
}
JitResult System::boundary(Tick until) {
    if (cpu.mode == CpuMode::locked) return {0, 0, JitExit::suspended};
    if (cpu.mode == CpuMode::stopped) {
        if (!memory.button_selected()) return {0, 0, JitExit::suspended};
        memory.resume_clock(); cpu.mode = CpuMode::running; memory.cpu_waiting = false;
    }
    if (cpu.mode == CpuMode::halted) {
        if (memory.double_speed() || memory.wake != kNever) return halt_cgb(until);
        const Tick start = memory.clock.now;
        if (!memory.pending_interrupts()) {
            // HALT samples enabled requests at M-cycle boundaries, a request
            // kHaltWake late: it wakes at the first boundary at least that
            // long after the request, where a NOP slide takes one at the
            // first boundary at or after it. AGE stat-int and mooneye
            // intr_2_mode3_timing time the same STAT request both ways, see
            // Ppu's kTransfer. Jump over idle cycles without moving device
            // events to those boundaries.
            constexpr Tick kHaltWake = 4;
            const Tick event = std::min(memory.clock.next, until);
            memory.clock.advance(event);
            const Tick wake = event + (memory.pending_interrupts() ? kHaltWake : 0);
            memory.clock.advance(start + ((wake - start + 7) & ~Tick{7}));
        }
        if (memory.pending_interrupts()) { cpu.mode = CpuMode::running; memory.cpu_waiting = false; }
        return {0, static_cast<uint32_t>((memory.clock.now - start) / 8), JitExit::event};
    }
    if (cpu.ime && memory.pending_interrupts()) {
        memory.instruction_pc = cpu.pc;
        cpu.ime = cpu.ei_pending = false;
        // EI;HALT with an already pending request pushes the HALT address.
        if (cpu.halt_bug) { --cpu.pc; cpu.halt_bug = false; }
        const auto push = [&](uint8_t value) {
            // Native probes cover generated accesses, not this shared service
            // sequence. The interpreter's mapped bus already records these.
            if (trace_ && backend_ == Backend::jit) memory.probed_write(--cpu.sp, value);
            else memory.write(--cpu.sp, value);
        };
        memory.idle(); memory.idle();
        push(cpu.pc >> 8);
        // Mooneye interrupts/ie_push: the high-byte write to IE can cancel or
        // reprioritize service, the low-byte write is too late to. IF counts
        // at the low-byte write's access edge, before it: Gambatte
        // irq_precedence/late_m0irq_vs_tima on dmg08 and cgb04c take a STAT
        // request that comes in that M-cycle over the timer's, and
        // if_and_ie_0_vector_4 a timer request the low byte clears from IF.
        const unsigned enabled = memory.high[127];
        const auto low = static_cast<uint16_t>(--cpu.sp);
        memory.idle(memory.access_ticks());
        const unsigned pending = (memory.io[0x0f] | memory.unseen_interrupts) & enabled & 31;
        if (trace_ && backend_ == Backend::jit)
            memory.record_access(memory.clock.now, low, static_cast<uint8_t>(cpu.pc), 1);
        memory.write_from_edge(low, static_cast<uint8_t>(cpu.pc));
        const unsigned bit = pending ? std::countr_zero(pending) : 0;
        cpu.pc = static_cast<uint16_t>(pending ? 0x40 + bit * 8 : 0);
        // The request leaves IF only 4 ticks into the last M-cycle, or as it
        // starts at double speed, so the same request coming again before
        // then is lost with it: Gambatte m2int_m2irq_late_retrigger and
        // irq_precedence/late_m0irq_retrigger on dmg08 and cgb04c.
        const unsigned edge = memory.double_speed() ? 0 : 4;
        memory.idle(edge);
        if (pending) {
            memory.io[0x0f] &= ~(1u << bit);
            memory.unseen_interrupts &= ~(1u << bit);
        }
        memory.idle(memory.mcycle() - edge);
        return {0, 5, JitExit::event};
    }
    return {};
}
// HALT at double speed, whose M-cycles are 4 ticks, and the HALT after a
// speed switch, which ends at its own time unless an interrupt comes first.
// That time sets the CPU's M-cycles apart from the devices' anew.
JitResult System::halt_cgb(Tick until) {
    const Tick start = memory.clock.now;
    const Tick mcycle = memory.mcycle();
    if (!memory.pending_interrupts()) {
        const Tick distance = std::min(memory.clock.next, until) - start;
        memory.clock.advance(std::min(memory.wake, start + ((distance + mcycle - 1) & ~(mcycle - 1))));
    }
    if (memory.pending_interrupts() || memory.clock.now >= memory.wake) {
        cpu.mode = CpuMode::running; memory.cpu_waiting = false; memory.wake = kNever;
    }
    return {0, static_cast<uint32_t>((memory.clock.now - start) / mcycle), JitExit::event};
}
JitResult System::step_block(Tick until, unsigned limit) {
    if (memory.clock.now >= until) return {};
    if (memory.clock.now >= memory.apu.flush_at()) memory.apu.flush();
    if (cpu.mode != CpuMode::running || (cpu.ime && memory.pending_interrupts())) return boundary(until);
    if (backend_ == Backend::jit) return jit_->run_block(cpu, until, limit);
    return interpreter_block(cpu, memory, until, limit, marker_);
}
// Select the backend once per run, without specializing the decoder or bus.
RunResult System::run(Tick target, bool frame) {
    const auto start_frame = memory.frames;
    const auto loop = [&]<bool Native>() -> RunResult {
        uint64_t instructions = 0;
        while (memory.clock.now < target && (!frame || memory.frames == start_frame)) {
            // A long run still hands sound over regularly, see Apu::flush.
            if (memory.clock.now >= memory.apu.flush_at()) memory.apu.flush();
            const Tick until = std::min(target, memory.apu.flush_at());
            if (cpu.mode != CpuMode::running || (cpu.ime && memory.pending_interrupts())) {
                if (boundary(until).exit == JitExit::suspended)
                    return {RunEnd::suspended, instructions, memory.clock.now};
                continue;
            }
            // Events, interrupt changes and suspension still end a batch early.
            const auto result = Native ? jit_->run(cpu, until, frame) :
                interpreter_block(cpu, memory, until, 64, marker_);
            instructions += result.instructions;
            if (result.fallback != JitFallback::none || !result.instructions)
                return {RunEnd::unavailable, instructions, memory.clock.now};
            if (result.exit == JitExit::breakpoint) return {RunEnd::breakpoint, instructions, memory.clock.now};
        }
        return {frame && memory.frames != start_frame ? RunEnd::frame : RunEnd::target, instructions, memory.clock.now};
    };
    const auto result = backend_ == Backend::jit ? loop.operator()<true>() : loop.operator()<false>();
    memory.apu.flush();
    return result;
}
RunResult System::run_until(Tick target) { return run(target, false); }
RunResult System::run_frame() { return run(memory.clock.now + 140448, true); }
}  // namespace gbx::gb
