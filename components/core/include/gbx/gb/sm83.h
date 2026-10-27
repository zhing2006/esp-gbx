#pragma once

#include <cstdint>

#include "gbx/gb/bus.h"

namespace gbx::gb {

enum class CpuMode : uint8_t { running, halted, stopped, locked };

// Plain instruction-boundary state, shared by the interpreter and JIT. Keep
// byte registers explicit: pair access uses shifts rather than endian-dependent
// unions or aliasing. F's low nibble must be zero when restoring a CPU state.
// This is not a boot-ROM state or an on-disk save format.
struct Sm83State {
    uint8_t a = 0, f = 0;
    uint8_t b = 0, c = 0;
    uint8_t d = 0, e = 0;
    uint8_t h = 0, l = 0;
    uint16_t sp = 0, pc = 0;
    bool ime = false;
    bool ei_pending = false;
    bool halt_bug = false;  // Suppress the next opcode fetch's PC increment.
    CpuMode mode = CpuMode::running;

    uint16_t af() const { return static_cast<uint16_t>((a << 8) | f); }
    uint16_t bc() const { return static_cast<uint16_t>((b << 8) | c); }
    uint16_t de() const { return static_cast<uint16_t>((d << 8) | e); }
    uint16_t hl() const { return static_cast<uint16_t>((h << 8) | l); }
    void set_af(uint16_t value) { a = value >> 8; f = value & 0xf0; }
    void set_bc(uint16_t value) { b = value >> 8; c = static_cast<uint8_t>(value); }
    void set_de(uint16_t value) { d = value >> 8; e = static_cast<uint8_t>(value); }
    void set_hl(uint16_t value) { h = value >> 8; l = static_cast<uint8_t>(value); }
};

struct StepResult {
    uint8_t mcycles;
    CpuMode mode;
    uint8_t opcode = 0;  // Actual fetched byte, including event-time changes.
};

// Executes one complete instruction, including its fetch, and returns even
// after HALT, STOP or an illegal opcode. Already suspended CPUs return zero
// cycles without touching the bus. All bus cycles are issued in order.
//
// The flat bus has no interrupt/IO devices and uses ordinary HALT/STOP entry.
// Mapped execution handles HALT bug and DMG STOP entry; System owns interrupt
// dispatch and waits between instructions.
StepResult sm83_step(Sm83State& state, const Bus& bus);
class Memory;
// Shared mapped CPU-control operations; not instruction fallback. JIT callers
// synchronize PC before STOP, the only operation here that changes a register.
void sm83_halt(Sm83State& state, Memory& memory);
void sm83_stop(Sm83State& state, Memory& memory);
StepResult sm83_step(Sm83State& state, Memory& memory);

struct Sm83BlockResult {
    uint32_t instructions = 0;
    uint32_t mcycles = 0;
    uint8_t opcode = 0;  // Last completed instruction, not a pre-fetch peek.
    bool suspended = false;  // Suspension stopped a still-admissible next step.
};

// Same decoder as step(), with one outer call for several instructions. Stops
// at a limit, suspension, marker or the first boundary after a device event.
// An instruction crossing the time limit always completes its bus sequence.
// At normal speed; sm83_run_double_speed() is the same at the CGB's double
// speed. Each has its own copy of the decoder, so that the M-cycle's length
// stays a constant in every inlined access. The speed changes only at STOP,
// whose pause ends a block.
Sm83BlockResult sm83_run_block(Sm83State& state, Memory& memory, uint64_t until,
                              unsigned limit, bool stop_on_marker = false);
Sm83BlockResult sm83_run_double_speed(Sm83State& state, Memory& memory, uint64_t until,
                                      unsigned limit, bool stop_on_marker = false);

// Observes one waiting interval without executing an instruction. HALT/lock
// consume one idle M-cycle. STOP holds the bus without advancing its clock.
// Returns elapsed M-cycles (zero also for a running CPU, which does nothing).
// There is no test-specific fixed number of wait samples in the interpreter.
uint8_t sm83_wait(const Sm83State& state, const Bus& bus);

}  // namespace gbx::gb
