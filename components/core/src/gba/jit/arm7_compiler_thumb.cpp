// Thumb instructions, with the decoding and semantics of src/gba/executor.h.

#include "../executor.h"
#include "arm7_compiler.h"

namespace gbx::gba::arm7_compiler {

void Compiler::thumb(uint32_t op) {
    switch (op >> 11) {
    case 0x00: case 0x01: case 0x02: {  // THUMB.1: LSL, LSR, ASR by an immediate.
        const unsigned rd = op & 7, rs = (op >> 3) & 7;
        // Shifted straight into Rd unless C comes from the Rs it overwrites.
        const bool direct = rd != rs;
        if (direct) {
            kill(true, false, false);
            clobber(host(rd));
        }
        Carry carry = kKeepCarry;
        const Value b = shift_immediate(op >> 11, in(host(rs)), (op >> 6) & 31, true, carry,
                                        direct ? host(rd) : R::a1);
        alu(13, fixed(0), b, carry, true, host(rd));
        sequential_ = 1;
        break;
    }
    case 0x03: {  // THUMB.2: ADD and SUB, a register or a 3-bit immediate.
        const Value b = op & (1u << 10) ? fixed((op >> 6) & 7) : in(host((op >> 6) & 7));
        alu(op & (1u << 9) ? 2 : 4, in(host((op >> 3) & 7)), b, kKeepCarry, true, host(op & 7));
        sequential_ = 1;
        break;
    }
    case 0x04: case 0x05: case 0x06: case 0x07: {  // THUMB.3: MOV, CMP, ADD and SUB with an 8-bit immediate.
        static constexpr uint8_t kOpcodes[4]{13, 10, 4, 2};
        const unsigned rd = (op >> 8) & 7, opcode = kOpcodes[(op >> 11) & 3];
        alu(opcode, in(host(rd)), fixed(op & 0xff), kKeepCarry, true, opcode == 10 ? R::zero : host(rd));
        sequential_ = 1;
        break;
    }
    case 0x08:
        if (op & (1u << 10)) high_register(op);
        else thumb_alu(op);
        break;
    case 0x09: {  // THUMB.6: LDR PC-relative, from the word-aligned PC.
        const uint32_t address = (r15_ & ~2u) + (op & 0xff) * 4;
        if (m_literal(address, (op >> 8) & 7)) break;
        li(R::a2, address);
        hint(15);
        load(Load::word);
        finish_load((op >> 8) & 7);
        break;
    }
    case 0x0a: case 0x0b: {  // THUMB.7 and THUMB.8: register offset.
        static constexpr Load kLoads[8]{Load::word, Load::word, Load::word, Load::signed_byte,
                                        Load::word, Load::half, Load::byte, Load::signed_half};
        static constexpr uint32_t kWidths[3]{kWord, kHalf, kByte};
        const unsigned rd = op & 7, kind = (op >> 9) & 7;
        emit(rv::add(R::a2, host((op >> 3) & 7), host((op >> 6) & 7)));
        hint((op >> 3) & 7);
        if (kind < 3) {
            store(kWidths[kind], in(host(rd)));
            sequential_ = 0;
        } else {
            load(kLoads[kind]);
            finish_load(rd);
        }
        break;
    }
    case 0x0c: case 0x0d: case 0x0e: case 0x0f: {  // THUMB.9: immediate offset, words and bytes.
        const unsigned rd = op & 7;
        const bool byte = op & (1u << 12);
        binary(Op::add, R::a2, in(host((op >> 3) & 7)), fixed(((op >> 6) & 31) * (byte ? 1 : 4)));
        hint((op >> 3) & 7);
        if (op & (1u << 11)) {
            load(byte ? Load::byte : Load::word);
            finish_load(rd);
        } else {
            store(byte ? kByte : kWord, in(host(rd)));
            sequential_ = 0;
        }
        break;
    }
    case 0x10: case 0x11: {  // THUMB.10: LDRH and STRH with an immediate offset.
        const unsigned rd = op & 7;
        binary(Op::add, R::a2, in(host((op >> 3) & 7)), fixed(((op >> 6) & 31) * 2));
        hint((op >> 3) & 7);
        if (op & (1u << 11)) {
            load(Load::half);
            finish_load(rd);
        } else {
            store(kHalf, in(host(rd)));
            sequential_ = 0;
        }
        break;
    }
    case 0x12: case 0x13: {  // THUMB.11: SP-relative.
        const unsigned rd = (op >> 8) & 7;
        binary(Op::add, R::a2, in(host(13)), fixed((op & 0xff) * 4));
        hint(13);
        if (op & (1u << 11)) {
            load(Load::word);
            finish_load(rd);
        } else {
            store(kWord, in(host(rd)));
            sequential_ = 0;
        }
        break;
    }
    case 0x14: case 0x15: {  // THUMB.12: ADD Rd, PC or SP, the PC word-aligned.
        const unsigned rd = (op >> 8) & 7;
        if (op & (1u << 11)) {
            clobber(host(rd));
            binary(Op::add, host(rd), in(host(13)), fixed((op & 0xff) * 4));
        } else {
            set_guest(rd, fixed((r15_ & ~2u) + (op & 0xff) * 4));
        }
        sequential_ = 1;
        break;
    }
    case 0x16: case 0x17:
        switch ((op >> 8) & 0xf) {
        case 0x0: {  // THUMB.13: ADD SP, signed 9-bit offset.
            const auto offset = static_cast<int32_t>((op & 0x7f) * 4);
            clobber(host(13));
            emit(rv::addi(host(13), host(13), op & 0x80 ? -offset : offset));
            sequential_ = 1;
            break;
        }
        case 0x4: case 0x5:  // THUMB.14: PUSH, with LR when bit 8 is set.
            block_transfer(13, (op & 0xff) | ((op & 0x100) << 6), true, false, true, false, false);
            break;
        case 0xc: case 0xd:  // POP, with PC when bit 8 is set.
            block_transfer(13, (op & 0xff) | ((op & 0x100) << 7), false, true, true, true, false);
            break;
        default:  // Reserved, and later ARMs' BKPT and others.
            exception(kModeUndefined, 0x04, true);
            break;
        }
        break;
    case 0x18: case 0x19: {  // THUMB.15: STMIA and LDMIA, writing back unless a load includes the base.
        const unsigned rb = (op >> 8) & 7;
        const bool load = op & (1u << 11);
        block_transfer(rb, op & 0xff, false, true, !(load && (op & (1u << rb))), load, false);
        break;
    }
    case 0x1a: case 0x1b: {  // THUMB.16 and THUMB.17: conditional branches and SWI.
        const unsigned cond = (op >> 8) & 0xf;
        if (cond == 0xf) {
            exception(kModeSupervisor, 0x08, false);
            break;
        }
        const uint32_t target = r15_ + static_cast<uint32_t>(static_cast<int8_t>(op & 0xff) * 2);
        // Condition 1110 branches always, see Executor::thumb().
        if (cond == 0xe) {
            refill_static(target, true);
            break;
        }
        // Both paths leave the block, each computing the pending flags.
        flush_charge();
        std::array<unsigned, 2> taken{};
        const unsigned count = when(cond, taken);
        const Flags flags = flags_;
        // A wait loop leaves: its next start measures afresh.
        if (spin_) emit(rv::sw(R::zero, R::sp, kFrameSpinSite));
        leave(r15_ + 2, false, true, Arm7JitExit::control_flow);
        flags_ = flags;
        end_ = false;
        for (unsigned i = 0; i < count; ++i) bind(taken[i]);
        refill_static(target, true);
        break;
    }
    case 0x1c:  // THUMB.18: B.
        refill_static(r15_ + static_cast<uint32_t>(static_cast<int32_t>(op << 21) >> 20), true);
        break;
    case 0x1d: exception(kModeUndefined, 0x04, true); break;  // ARMv5's BLX suffix.
    case 0x1e:  // THUMB.19: BL, first half: LR = PC + offset << 12.
        set_guest(14, fixed(r15_ + static_cast<uint32_t>(static_cast<int32_t>(op << 21) >> 9)));
        sequential_ = 1;
        break;
    default:  // BL, second half: PC = LR + offset << 1, LR = next instruction | 1 (TRM 6.3).
        binary(Op::add, R::a3, in(host(14)), fixed((op & 0x7ff) * 2));
        set_guest(14, fixed((r15_ - 2) | 1));
        refill_dynamic(R::a3, true);
        break;
    }
}

// THUMB.4: each an ARM data processing operation with S. Shifts by a
// register take an internal cycle, MUL m of them, by Rd's value.
void Compiler::thumb_alu(uint32_t op) {
    const unsigned rd = op & 7, rs = (op >> 3) & 7, operation = (op >> 6) & 15;
    const Value d = in(host(rd)), m = in(host(rs));
    sequential_ = 1;
    switch (operation) {
    case 0x2: case 0x3: case 0x4: case 0x7: {  // LSL, LSR, ASR and ROR, in place.
        static constexpr uint8_t kTypes[8]{0, 0, 0, 1, 2, 0, 0, 3};
        idle();
        kill(true, false, false);
        clobber(host(rd));
        shift_register(kTypes[operation], d, m, true, host(rd));
        alu(13, fixed(0), d, {kCarryDone, R::zero, 0}, true, host(rd));
        sequential_ = 0;
        break;
    }
    case 0x8: case 0xa: case 0xb: alu(operation, d, m, kKeepCarry, true, R::zero); break;  // TST, CMP, CMN
    case 0x9: alu(2, fixed(0), m, kKeepCarry, true, host(rd)); break;                     // NEG
    case 0xd: thumb_multiply(rd, rs); break;
    default: {
        // AND, EOR, ADC, SBC, ORR, BIC and MVN: their ARM opcodes.
        static constexpr uint8_t kOpcodes[16]{0, 1, 0, 0, 0, 5, 6, 0, 0, 0, 0, 0, 12, 0, 14, 15};
        alu(kOpcodes[operation], d, m, kKeepCarry, true, host(rd));
        break;
    }
    }
}

// MUL Rd, Rs: Rd = Rs * Rd, C pending on Rs and a copy of Rd, the multiplier.
void Compiler::thumb_multiply(unsigned rd, unsigned rs) {
    const R multiplier = hold();
    mv(multiplier, host(rd));
    const R multiplicand = rs == rd ? multiplier : host(rs);
    multiply_cycles(in(multiplier), true, 0);
    idle_loop();
    kill(true, true, false);
    clobber(host(rd));
    flags_.c = kCMultiply;
    flags_.c_first = multiplicand;
    flags_.c_second = static_cast<uint8_t>(number(multiplier));
    emit(rv::mul(host(rd), multiplicand, multiplier));
    flags_.nz = kNzRegister;
    flags_.nz_reg = host(rd);
    sequential_ = 0;
}

// THUMB.5: ADD, CMP and MOV on any registers, and BX. Only CMP sets flags;
// ADD and MOV to R15 branch in Thumb state.
void Compiler::high_register(uint32_t op) {
    const unsigned rd = (op & 7) | ((op >> 4) & 8);
    const Value source = guest((op >> 3) & 15, r15_);
    sequential_ = 1;
    switch ((op >> 8) & 3) {
    case 0:
        if (rd == 15) {
            if (source.fixed) {
                refill_static(r15_ + source.constant, true);
                return;
            }
            binary(Op::add, R::a3, fixed(r15_), source);
            refill_dynamic(R::a3, true);
            return;
        }
        clobber(host(rd));
        binary(Op::add, host(rd), in(host(rd)), source);
        return;
    case 1: alu(10, guest(rd, r15_), source, kKeepCarry, true, R::zero); return;
    case 2:
        if (rd == 15) branch_to(source);
        else set_guest(rd, source);
        return;
    default: exchange(source); return;
    }
}

}  // namespace gbx::gba::arm7_compiler
