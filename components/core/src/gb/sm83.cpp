#include "gbx/gb/sm83.h"
#include "gbx/gb/memory.h"

#include <type_traits>

namespace gbx::gb {
namespace {

constexpr uint8_t kZ = 0x80;
constexpr uint8_t kN = 0x40;
constexpr uint8_t kH = 0x20;
constexpr uint8_t kC = 0x10;

int signed_byte(uint8_t value) {
    return value < 0x80 ? value : static_cast<int>(value) - 0x100;
}

// Decode the instruction families documented in Pan Docs, CPU Instruction Set.
// A small switch per family keeps flag logic and bus sequencing in one place;
// there are no generated per-opcode functions. Two bus specializations retain
// the flat test contract and inline the mapped fast path. Before specialization
// the P4 release decoder measured 8156 text bytes (task-2-1 baseline report).
// Timings follow the simple M-cycle diagrams in gbctr.pdf, chapter 6. The
// observation boundary is before the next opcode fetch, as in SingleStepTests.
// On the mapped bus, `Mcycle` is the length of an M-cycle in ticks, 4 at the
// CGB's double speed, see sm83_run_block().
template<class BusType, unsigned Mcycle = 8>
class Instruction {
public:
    Instruction(Sm83State& state, BusType& bus) : s_(state), bus_(bus) {}

    StepResult run() {
        const bool enable_ime = s_.ei_pending;
        s_.ei_pending = false;
        const uint8_t opcode = read(s_.pc);
        if constexpr (kMapped) {
            if (s_.halt_bug) s_.halt_bug = false;
            else ++s_.pc;
        } else ++s_.pc;
        decode(opcode);
        // EI takes effect after the following instruction; DI cancels it.
        // In particular, EI;EI must not defer the first EI for another opcode.
        // Source: Pan Docs, Interrupts / IME; gbctr.pdf, DI and EI.
        if (enable_ime && opcode != 0xf3) {
            s_.ime = true;
            if constexpr (kMapped) ++bus_.interrupt_generation;
        }
        return {mcycles_, s_.mode, opcode};
    }

private:
    static constexpr bool kMapped = std::is_same_v<BusType, Memory>;

    uint8_t read(uint16_t address) {
        ++mcycles_;
        if constexpr (kMapped) return bus_.read(address, Mcycle);
        else return bus_.read(address);
    }

    void write(uint16_t address, uint8_t value) {
        ++mcycles_;
        if constexpr (kMapped) bus_.write(address, value, Mcycle);
        else bus_.write(address, value);
    }

    void idle() {
        ++mcycles_;
        if constexpr (kMapped) bus_.idle(Mcycle);
        else bus_.idle();
    }

    uint8_t fetch() {
        const uint8_t value = read(s_.pc);
        ++s_.pc;
        return value;
    }

    uint16_t immediate16() {
        const uint8_t low = fetch();
        const uint8_t high = fetch();
        return static_cast<uint16_t>(low | (high << 8));
    }

    uint8_t reg8(unsigned index) {
        switch (index) {
        case 0: return s_.b;
        case 1: return s_.c;
        case 2: return s_.d;
        case 3: return s_.e;
        case 4: return s_.h;
        case 5: return s_.l;
        case 6: return read(s_.hl());
        default: return s_.a;
        }
    }

    void set_reg8(unsigned index, uint8_t value) {
        switch (index) {
        case 0: s_.b = value; break;
        case 1: s_.c = value; break;
        case 2: s_.d = value; break;
        case 3: s_.e = value; break;
        case 4: s_.h = value; break;
        case 5: s_.l = value; break;
        case 6: write(s_.hl(), value); break;
        default: s_.a = value; break;
        }
    }

    uint16_t reg16(unsigned index) const {
        switch (index) {
        case 0: return s_.bc();
        case 1: return s_.de();
        case 2: return s_.hl();
        default: return s_.sp;
        }
    }

    void set_reg16(unsigned index, uint16_t value) {
        switch (index) {
        case 0: s_.set_bc(value); break;
        case 1: s_.set_de(value); break;
        case 2: s_.set_hl(value); break;
        default: s_.sp = value; break;
        }
    }

    bool condition(unsigned index) const {
        const uint8_t flag = index < 2 ? kZ : kC;
        return ((s_.f & flag) != 0) == ((index & 1) != 0);
    }

    void push(uint16_t value) {
        idle();
        --s_.sp;
        write(s_.sp, static_cast<uint8_t>(value >> 8));
        --s_.sp;
        write(s_.sp, static_cast<uint8_t>(value));
    }

    uint16_t pop() {
        const uint8_t low = read(s_.sp);
        ++s_.sp;
        const uint8_t high = read(s_.sp);
        ++s_.sp;
        return static_cast<uint16_t>(low | (high << 8));
    }

    void ret() {
        const uint16_t target = pop();
        idle();
        s_.pc = target;
    }

    void relative_jump(bool taken) {
        const int offset = signed_byte(fetch());
        if (taken) {
            idle();
            s_.pc = static_cast<uint16_t>(s_.pc + offset);
        }
    }

    void jump(bool taken) {
        const uint16_t target = immediate16();
        if (taken) {
            idle();
            s_.pc = target;
        }
    }

    void call(bool taken) {
        const uint16_t target = immediate16();
        if (taken) {
            push(s_.pc);
            s_.pc = target;
        }
    }

    void inc_dec(unsigned index, bool decrement) {
        const uint8_t old = reg8(index);
        const uint8_t value = static_cast<uint8_t>(old + (decrement ? -1 : 1));
        const bool half = decrement ? (old & 15) == 0 : (old & 15) == 15;
        s_.f = (s_.f & kC) | (value == 0 ? kZ : 0) |
               (decrement ? kN : 0) | (half ? kH : 0);
        set_reg8(index, value);
    }

    void alu(unsigned operation, uint8_t value) {
        const unsigned a = s_.a;
        const unsigned carry = (s_.f & kC) != 0;
        if (operation < 4 || operation == 7) {
            const unsigned input_carry = (operation == 1 || operation == 3) ? carry : 0;
            const bool subtract = operation == 2 || operation == 3 || operation == 7;
            const unsigned operand = value + input_carry;
            const uint8_t result = static_cast<uint8_t>(subtract ? a - operand : a + operand);
            const bool half = subtract ? (a & 15) < (value & 15u) + input_carry
                                       : (a & 15) + (value & 15u) + input_carry > 15;
            const bool overflow = subtract ? a < operand : a + operand > 255;
            s_.f = (result == 0 ? kZ : 0) | (subtract ? kN : 0) |
                   (half ? kH : 0) | (overflow ? kC : 0);
            if (operation != 7) {
                s_.a = result;
            }
            return;
        }
        switch (operation) {
        case 4: s_.a &= value; break;
        case 5: s_.a ^= value; break;
        case 6: s_.a |= value; break;
        }
        s_.f = (s_.a == 0 ? kZ : 0) | (operation == 4 ? kH : 0);
    }

    uint8_t rotate(unsigned operation, uint8_t value) {
        const unsigned input_carry = (s_.f & kC) != 0;
        bool carry = false;
        uint8_t result = 0;
        switch (operation) {
        case 0:  // RLC
            carry = (value & 0x80) != 0;
            result = static_cast<uint8_t>((value << 1) | (value >> 7));
            break;
        case 1:  // RRC
            carry = (value & 1) != 0;
            result = static_cast<uint8_t>((value >> 1) | (value << 7));
            break;
        case 2:  // RL
            carry = (value & 0x80) != 0;
            result = static_cast<uint8_t>((value << 1) | input_carry);
            break;
        case 3:  // RR
            carry = (value & 1) != 0;
            result = static_cast<uint8_t>((value >> 1) | (input_carry << 7));
            break;
        case 4:  // SLA
            carry = (value & 0x80) != 0;
            result = static_cast<uint8_t>(value << 1);
            break;
        case 5:  // SRA
            carry = (value & 1) != 0;
            result = static_cast<uint8_t>((value >> 1) | (value & 0x80));
            break;
        case 6:  // SWAP
            result = static_cast<uint8_t>((value << 4) | (value >> 4));
            break;
        case 7:  // SRL
            carry = (value & 1) != 0;
            result = value >> 1;
            break;
        }
        s_.f = (result == 0 ? kZ : 0) | (carry ? kC : 0);
        return result;
    }

    void daa() {
        // Adjust from the original A and flags, including combinations that
        // are not the result of valid BCD arithmetic. See gbz80(7), DAA, and
        // SingleStepTests 27.json (gbctr.pdf leaves the adjustment as TODO).
        unsigned correction = 0;
        bool carry = (s_.f & kC) != 0;
        if (s_.f & kN) {
            if (s_.f & kH) correction |= 0x06;
            if (carry) correction |= 0x60;
            s_.a = static_cast<uint8_t>(s_.a - correction);
        } else {
            if ((s_.f & kH) || (s_.a & 15) > 9) correction |= 0x06;
            if (carry || s_.a > 0x99) {
                correction |= 0x60;
                carry = true;
            }
            s_.a = static_cast<uint8_t>(s_.a + correction);
        }
        s_.f = (s_.f & kN) | (s_.a == 0 ? kZ : 0) | (carry ? kC : 0);
    }

    void add_hl(uint16_t value) {
        const unsigned hl = s_.hl();
        const unsigned result = hl + value;
        idle();
        s_.f = (s_.f & kZ) | ((hl & 0xfff) + (value & 0xfff) > 0xfff ? kH : 0) |
               (result > 0xffff ? kC : 0);
        s_.set_hl(static_cast<uint16_t>(result));
    }

    void add_sp(bool to_hl) {
        const uint8_t offset = fetch();
        const uint16_t sp = s_.sp;
        const uint16_t result = static_cast<uint16_t>(sp + signed_byte(offset));
        idle();
        // H and C come from the unsigned low-byte addition even for a negative
        // displacement. See gbctr.pdf, ADD SP,e and LD HL,SP+e.
        s_.f = ((sp & 15) + (offset & 15) > 15 ? kH : 0) |
               ((sp & 255) + offset > 255 ? kC : 0);
        if (to_hl) {
            s_.set_hl(result);
        } else {
            idle();
            s_.sp = result;
        }
    }

    void cb() {
        const uint8_t opcode = fetch();
        const unsigned group = opcode >> 6;
        const unsigned bit = (opcode >> 3) & 7;
        const unsigned reg = opcode & 7;
        const uint8_t value = reg8(reg);
        switch (group) {
        case 0: set_reg8(reg, rotate(bit, value)); break;
        case 1:
            s_.f = (s_.f & kC) | kH | ((value & (1u << bit)) == 0 ? kZ : 0);
            break;
        case 2: set_reg8(reg, static_cast<uint8_t>(value & ~(1u << bit))); break;
        case 3: set_reg8(reg, static_cast<uint8_t>(value | (1u << bit))); break;
        }
    }

    void block0(unsigned y, unsigned z) {
        const unsigned pair = y >> 1;
        const bool odd = (y & 1) != 0;
        switch (z) {
        case 0:
            switch (y) {
            case 0: break;  // NOP
            case 1: {
                const uint16_t address = immediate16();
                write(address, static_cast<uint8_t>(s_.sp));
                write(static_cast<uint16_t>(address + 1), static_cast<uint8_t>(s_.sp >> 8));
                break;
            }
            case 2:
                // Normal STOP entry skips its padding byte. The no-interrupt,
                // no-wake case is documented by Pan Docs, Reducing Power
                // Consumption / STOP. The suite's generator only emits two
                // idle samples here and omits the padding skip; its +1 PC
                // expectation is not a model of normal STOP entry. The source
                // and affected cases are recorded in docs/plans/gb/task-1-1.md.
                if constexpr (kMapped) sm83_stop(s_, bus_);
                else { ++s_.pc; s_.mode = CpuMode::stopped; }
                break;
            case 3: relative_jump(true); break;
            default: relative_jump(condition(y - 4)); break;
            }
            break;
        case 1:
            if (odd) add_hl(reg16(pair));
            else set_reg16(pair, immediate16());
            break;
        case 2: {
            const uint16_t address = pair < 2 ? reg16(pair) : s_.hl();
            if (odd) s_.a = read(address);
            else write(address, s_.a);
            if (pair >= 2) {
                s_.set_hl(static_cast<uint16_t>(address + (pair == 2 ? 1 : -1)));
            }
            break;
        }
        case 3:
            idle();
            set_reg16(pair, static_cast<uint16_t>(reg16(pair) + (odd ? -1 : 1)));
            break;
        case 4: inc_dec(y, false); break;
        case 5: inc_dec(y, true); break;
        case 6: set_reg8(y, fetch()); break;
        case 7:
            if (y < 4) {
                s_.a = rotate(y, s_.a);
                s_.f &= kC;  // Accumulator rotations always clear Z.
            } else {
                switch (y) {
                case 4: daa(); break;
                case 5: s_.a ^= 0xff; s_.f |= kN | kH; break;
                case 6: s_.f = (s_.f & kZ) | kC; break;
                case 7: s_.f = (s_.f & kZ) | ((s_.f & kC) ? 0 : kC); break;
                }
            }
            break;
        }
    }

    void block3(unsigned y, unsigned z) {
        const unsigned pair = y >> 1;
        const bool odd = (y & 1) != 0;
        switch (z) {
        case 0:
            if (y < 4) {
                idle();  // Conditional RET checks even when not taken.
                if (condition(y)) ret();
            } else {
                switch (y) {
                case 4: {
                    const uint16_t address = 0xff00u | fetch();
                    write(address, s_.a);
                    break;
                }
                case 5: add_sp(false); break;
                case 6: s_.a = read(0xff00u | fetch()); break;
                case 7: add_sp(true); break;
                }
            }
            break;
        case 1:
            if (!odd) {
                const uint16_t value = pop();
                if (pair == 3) s_.set_af(value);
                else set_reg16(pair, value);
            } else {
                switch (pair) {
                case 0: ret(); break;
                case 1:
                    ret(); s_.ime = true; s_.ei_pending = false;
                    if constexpr (kMapped) ++bus_.interrupt_generation;
                    break;
                case 2: s_.pc = s_.hl(); break;
                case 3: idle(); s_.sp = s_.hl(); break;
                }
            }
            break;
        case 2:
            if (y < 4) {
                jump(condition(y));
            } else {
                switch (y) {
                case 4: write(0xff00u | s_.c, s_.a); break;
                case 5: {
                    const uint16_t address = immediate16();
                    write(address, s_.a);
                    break;
                }
                case 6: s_.a = read(0xff00u | s_.c); break;
                case 7: s_.a = read(immediate16()); break;
                }
            }
            break;
        case 3:
            switch (y) {
            case 0: jump(true); break;
            case 1: cb(); break;
            case 6: s_.ime = false; s_.ei_pending = false; break;
            case 7: s_.ei_pending = true; break;
            default: s_.mode = CpuMode::locked; break;
            }
            break;
        case 4:
            if (y < 4) call(condition(y));
            else s_.mode = CpuMode::locked;
            break;
        case 5:
            if (!odd) push(pair == 3 ? s_.af() : reg16(pair));
            else if (pair == 0) call(true);
            else s_.mode = CpuMode::locked;
            break;
        case 6: alu(y, fetch()); break;
        case 7: push(s_.pc); s_.pc = static_cast<uint16_t>(y * 8); break;
        }
    }

    void decode(uint8_t opcode) {
        const unsigned y = (opcode >> 3) & 7;
        const unsigned z = opcode & 7;
        switch (opcode >> 6) {
        case 0: block0(y, z); break;
        case 1:
            if (opcode == 0x76) {
                if constexpr (kMapped) sm83_halt(s_, bus_);
                else s_.mode = CpuMode::halted;
            } else set_reg8(y, reg8(z));
            break;
        case 2: alu(y, reg8(z)); break;
        case 3: block3(y, z); break;
        }
        // All 256 base bytes are decoded. The 11 holes enter a persistent lock,
        // never std::unreachable() or NOP. Source: Pan Docs, CPU Instruction Set.
    }

    Sm83State& s_;
    BusType& bus_;
    uint8_t mcycles_ = 0;
};

}  // namespace

void sm83_halt(Sm83State& state, Memory& memory) {
    if (!state.ime && memory.pending_interrupts()) state.halt_bug = true;
    else { state.mode = CpuMode::halted; memory.cpu_waiting = true; }
}
namespace {
// The STOP chart with a KEY1 switch armed, which only a CGB runs. A held
// button comes first and keeps the speed. Otherwise the switch halts the CPU
// through its pause, see Memory::switch_speed(). With an interrupt pending
// and IME set, the CPU glitches in no defined way: it locks, so that a run
// ends as suspended rather than going on in a made-up state.
[[gnu::noinline]] void stop_armed(Sm83State& state, Memory& memory) {
    const bool pending = memory.pending_interrupts();
    const bool button = memory.button_selected();
    if (!button && pending && state.ime) { state.mode = CpuMode::locked; return; }
    if (!pending) { ++state.pc; state.mode = CpuMode::halted; memory.cpu_waiting = true; }
    if (!button) memory.switch_speed(!pending);
}
}  // namespace

void sm83_stop(Sm83State& state, Memory& memory) {
    // Pan Docs, Reducing Power Consumption: the STOP chart. Apart from its
    // KEY1 branches, so that the GB's code keeps its size.
    if (memory.speed_switch_armed()) { stop_armed(state, memory); return; }
    const bool pending = memory.pending_interrupts();
    if (!pending) ++state.pc;
    if (memory.button_selected()) {
        if (!pending) { state.mode = CpuMode::halted; memory.cpu_waiting = true; }
    } else {
        memory.stop_clock();
        state.mode = CpuMode::stopped; memory.cpu_waiting = true;
    }
}

StepResult sm83_step(Sm83State& state, const Bus& bus) {
    if (state.mode != CpuMode::running) {
        return {0, state.mode};
    }
    return Instruction(state, bus).run();
}

namespace {
template<unsigned Mcycle>
[[gnu::always_inline]] inline Sm83BlockResult run_block(Sm83State& state, Memory& memory, uint64_t until,
                                                        unsigned limit, bool stop_on_marker) {
    // Keep the counters local; write the returned aggregate only on exit.
    uint32_t instructions = 0, mcycles = 0;
    uint8_t opcode = 0;
    bool suspended = false;
    const auto generation = memory.clock.generation;
    const auto interrupts = memory.interrupt_generation;
    while (instructions < limit && memory.clock.now < until) {
        if (state.mode != CpuMode::running) { suspended = true; break; }
        memory.instruction_pc = state.pc;
        const auto step = Instruction<Memory, Mcycle>(state, memory).run();
        ++instructions;
        mcycles += step.mcycles;
        opcode = step.opcode;
        if ((stop_on_marker && step.opcode == 0x40) || memory.clock.generation != generation ||
            memory.interrupt_generation != interrupts) break;
    }
    return {instructions, mcycles, opcode, suspended};
}
}  // namespace

Sm83BlockResult sm83_run_block(Sm83State& state, Memory& memory, uint64_t until,
                              unsigned limit, bool stop_on_marker) {
    return run_block<8>(state, memory, until, limit, stop_on_marker);
}
Sm83BlockResult sm83_run_double_speed(Sm83State& state, Memory& memory, uint64_t until,
                                      unsigned limit, bool stop_on_marker) {
    return run_block<4>(state, memory, until, limit, stop_on_marker);
}

StepResult sm83_step(Sm83State& state, Memory& memory) {
    const auto result = memory.double_speed() ? sm83_run_double_speed(state, memory, kNever, 1)
                                              : sm83_run_block(state, memory, kNever, 1);
    return {static_cast<uint8_t>(result.mcycles), state.mode, result.opcode};
}

uint8_t sm83_wait(const Sm83State& state, const Bus& bus) {
    switch (state.mode) {
    case CpuMode::running: return 0;
    case CpuMode::stopped: bus.hold(); return 0;
    case CpuMode::halted:
    case CpuMode::locked: bus.idle(); return 1;
    }
    return 0;
}

}  // namespace gbx::gb
