#pragma once

#include <array>
#include <cstdint>

#include "gbx/gba/bus.h"

namespace gbx::gba {

// Program status register bits and modes (GBATEK "ARM CPU Flags & Condition
// Field").
inline constexpr uint32_t kFlagN = 1u << 31, kFlagZ = 1u << 30, kFlagC = 1u << 29, kFlagV = 1u << 28;
inline constexpr uint32_t kFlagI = 1u << 7, kFlagF = 1u << 6, kFlagT = 1u << 5;
inline constexpr uint32_t kModeMask = 0x1f;
inline constexpr uint32_t kModeUser = 0x10, kModeFiq = 0x11, kModeIrq = 0x12, kModeSupervisor = 0x13,
                          kModeAbort = 0x17, kModeUndefined = 0x1b, kModeSystem = 0x1f;

// Instruction-boundary state, shared by the interpreter and the JIT. r holds
// the registers of the current mode, so that an instruction indexes them
// directly; a mode change moves the banked ones, see arm7_switch_mode().
//
// R15 is the address of the instruction in pipeline[1] plus 4 in ARM state
// and 2 in Thumb state, that is, the executing instruction's address plus 8
// or 4 (GBATEK "ARM CPU Register Set"). pipeline[0] executes next; both are
// already fetched, halfwords in Thumb state. sequential is the SEQ the CPU
// drove in its last cycle: whether the next code fetch is an S cycle (ARM7TDMI
// TRM, DDI 0029G, 6.1).
struct Arm7State {
    std::array<uint32_t, 16> r{};
    uint32_t cpsr = kModeSupervisor | kFlagI | kFlagF;
    std::array<uint32_t, 2> pipeline{};
    bool sequential = false;
    // The banked registers that are not in r. r8[0] is R8-R12 of every mode
    // but FIQ, r8[1] FIQ's; r13 is R13-R14 of User and System, FIQ, SVC,
    // ABT, IRQ and UND, in that order. The entries of the current mode are
    // stale.
    std::array<std::array<uint32_t, 5>, 2> r8{};
    std::array<std::array<uint32_t, 2>, 6> r13{};
    // SPSR of FIQ, SVC, ABT, IRQ and UND.
    std::array<uint32_t, 5> spsr{};
};

// Executes the instruction in pipeline[0] with all of its bus cycles, from
// any state, and returns at the next instruction boundary. Single-step tests
// call it, and so does the JIT when it falls back to the interpreter.
void arm7_step(Arm7State& state, const Bus& bus);

// IRQ exception entry in place of the next instruction: its prefetch, R14
// and SPSR of IRQ mode, and the refill from 18h (GBATEK "ARM CPU
// Exceptions", TRM 6.12). The caller checks the I flag. Milestone 2
// connects it to the interrupt controller.
void arm7_interrupt(Arm7State& state, const Bus& bus);

// Moves the banked registers from mode `from` to mode `to`; cpsr is not
// changed. A mode value that GBATEK does not list uses the registers of User
// mode and has no SPSR: no source gives the hardware's banking for it.
void arm7_switch_mode(Arm7State& state, uint32_t from, uint32_t to);

// The register file as GBATEK "ARM CPU Register Set" lists it and the
// single-step tests store it: R0-R15 of User and System mode, R8-R14 of FIQ,
// R13-R14 of SVC, ABT, IRQ and UND, CPSR, and the SPSRs in the order of
// Arm7State.
struct Arm7Registers {
    std::array<uint32_t, 16> r{};
    std::array<uint32_t, 7> fiq{};
    std::array<uint32_t, 2> svc{}, abt{}, irq{}, und{};
    uint32_t cpsr = 0;
    std::array<uint32_t, 5> spsr{};
};
void arm7_set_registers(Arm7State& state, const Arm7Registers& registers);
Arm7Registers arm7_registers(const Arm7State& state);

}  // namespace gbx::gba
