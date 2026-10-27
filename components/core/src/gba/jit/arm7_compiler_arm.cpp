// ARM instructions, with the decoding and semantics of src/gba/executor.h.

#include <bit>

#include "../executor.h"
#include "arm7_compiler.h"

namespace gbx::gba::arm7_compiler {

// Conditional execution (GBATEK "ARM CPU Flags & Condition Field"): a
// passing condition runs the instruction, a failing one only its prefetch.
// Branches and instructions that change the mode end the block on their
// path; a failing condition goes on, or for a branch leaves too. The paths
// of other instructions join again and must leave the same flag state:
// flags the instruction reads or sets are computed before the condition,
// those it sets by the end of its path, and the values of registers it
// writes that pending flags read are copied to held registers first. Where
// the paths end with different SEQ, each puts the request of the next
// prefetch in a1.
void Compiler::arm(uint32_t op) {
    const unsigned cond = op >> 28;
    if (cond == 15) {  // NV never passes.
        sequential_ = 1;
        return;
    }
    if (cond == 14) {
        arm_execute(op);
        return;
    }
    const Shape shape = arm_shape(op);
    flush_charge();
    std::array<unsigned, 2> skip{};
    if (shape.branch || shape.ends) {
        const unsigned skips = when(cond ^ 1, skip);
        const Flags flags = flags_;
        arm_execute(op);
        if (!end_) leave(r15_ + 4, false, sequential_, Arm7JitExit::control_flow);
        flags_ = flags;
        busy_ = 0;
        end_ = false;
        for (unsigned i = 0; i < skips; ++i) bind(skip[i]);
        sequential_ = 1;
        if (shape.branch) {
            // A wait loop leaves: its next start measures afresh.
            if (spin_) emit(rv::sw(R::zero, R::sp, kFrameSpinSite));
            leave(r15_ + 4, false, true, Arm7JitExit::control_flow);
        }
        return;
    }
    if (shape.sets || shape.reads == kReadsAll) {
        materialize_all();
    } else {
        if (shape.reads) materialize_c();
        preserve(shape.writes);
        // Loads, stores and block transfers hold one register.
        if (!shape.sequential && !free_held()) materialize_all();
    }
    const unsigned skips = when(cond ^ 1, skip);
    // An instruction that sets flags may compute its own on its path: all
    // of them are computed at its end.
    merging_ = !shape.sets;
    arm_execute(op);
    merging_ = false;
    flush_charge();
    if (shape.sets) materialize_all();
    if (shape.sequential) {
        for (unsigned i = 0; i < skips; ++i) bind(skip[i]);
        sequential_ = 1;
        return;
    }
    li(R::a1, kFetch | kWord);
    const unsigned join = jump();
    for (unsigned i = 0; i < skips; ++i) bind(skip[i]);
    li(R::a1, kFetch | kWord | kSequential);
    bind(join);
    sequential_ = kRequested;
}

Compiler::Shape Compiler::arm_shape(uint32_t op) const {
    Shape s{false, false, false, kReadsNone, true, 0};
    const unsigned rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    const bool writeback = !(op & (1u << 24)) || (op & (1u << 21));
    const bool load = op & (1u << 20);
    switch ((op >> 25) & 7) {
    case 0:
    case 1:
        if (!(op & (1u << 25)) && (op & 0x90) == 0x90) {
            s.sequential = false;
            if (op & 0x60) {  // Halfword and signed transfers.
                s.writes = static_cast<uint16_t>((load ? 1u << rd : 0) | (writeback ? 1u << rn : 0));
                s.branch = (load && rd == 15) || (writeback && rn == 15);
            } else if (op & (1u << 24)) {  // SWP
                s.writes = static_cast<uint16_t>(1u << rd);
                s.branch = rd == 15;
            } else if (op & (1u << 23)) {  // Long multiplies: RdHi in Rn's field.
                s.writes = static_cast<uint16_t>(1u << rn | 1u << rd);
                s.branch = rn == 15 || rd == 15;
                s.sets = load;
            } else {  // MUL and MLA: Rd in Rn's field.
                s.writes = static_cast<uint16_t>(1u << rn);
                s.branch = rn == 15;
                s.sets = load;
            }
        } else if ((op & 0x01900000) == 0x01000000) {
            if ((op & 0x0ffffff0) == 0x012fff10) {  // BX
                s.branch = true;
            } else if (!(op & (1u << 21))) {  // MRS
                s.writes = static_cast<uint16_t>(1u << rd);
                s.branch = rd == 15;
                s.reads = !(op & (1u << 22)) || !bank_ ? kReadsAll : kReadsNone;
            } else if (!(op & (1u << 22))) {  // MSR to CPSR
                if ((mode_ & 0x10) && !(msr_mask(op) & 0x00ffffff)) s.sets = msr_mask(op) != 0;
                else s.ends = true;
            }
        } else {
            const unsigned opcode = (op >> 21) & 15;
            const bool set = op & (1u << 20), restore = rd == 15 && set && bank_;
            const bool register_shift = !(op & (1u << 25)) && (op & 0x10);
            s.sequential = !register_shift;
            s.sets = set && !restore;
            // ADC, SBC, RSC and RRX read C.
            s.reads = (opcode >= 5 && opcode <= 7) ||
                              (!(op & (1u << 25)) && !register_shift && ((op >> 5) & 3) == 3 && !((op >> 7) & 31))
                          ? kReadsCarry
                          : kReadsNone;
            if ((opcode & 0xc) == 8) {
                s.ends = restore;
            } else {
                s.writes = static_cast<uint16_t>(1u << rd);
                s.branch = rd == 15;
            }
        }
        break;
    case 2:
    case 3:
        if (((op >> 25) & 7) == 3 && (op & 0x10)) {  // Undefined.
            s.branch = true;
            break;
        }
        s.sequential = false;
        s.writes = static_cast<uint16_t>((load ? 1u << rd : 0) | (writeback ? 1u << rn : 0));
        s.branch = (load && rd == 15) || (writeback && rn == 15);
        s.reads = (op & (1u << 25)) && ((op >> 5) & 3) == 3 && !((op >> 7) & 31) ? kReadsCarry : kReadsNone;
        break;
    case 4: {
        const uint32_t list = op & 0xffff;
        s.sequential = false;
        s.writes = static_cast<uint16_t>((load ? list : 0) | (op & (1u << 21) ? 1u << rn : 0));
        s.branch = (load && (!list || (list & 0x8000))) || ((op & (1u << 21)) && rn == 15);
        break;
    }
    case 7:
        if (!(op & (1u << 24)) && (op & 0x10) && ((op >> 8) & 15) == 14) {  // MRC and MCR on CP14.
            s.sequential = false;
            if (load && rd == 15) s.sets = true;
            else if (load) s.writes = static_cast<uint16_t>(1u << rd);
            break;
        }
        s.branch = true;
        break;
    default:
        s.branch = true;
        break;
    }
    s.writes &= 0x7fff;
    return s;
}

void Compiler::arm_execute(uint32_t op) {
    switch ((op >> 25) & 7) {
    case 0:
        if ((op & 0x90) == 0x90) {
            if (op & 0x60) halfword_transfer(op);
            else if (op & (1u << 24)) swap(op);
            else if (op & (1u << 23)) multiply_long(op);
            else multiply(op);
        } else if ((op & 0x01900000) == 0x01000000) {
            if ((op & 0x0ffffff0) == 0x012fff10) exchange(guest(op & 15, r15_));
            else psr_transfer(op);
        } else {
            data_processing(op);
        }
        break;
    case 1:
        if ((op & 0x01900000) == 0x01000000) psr_transfer(op);
        else data_processing(op);
        break;
    case 2: single_transfer(op); break;
    case 3:
        if (op & 0x10) exception(kModeUndefined, 0x04, true);
        else single_transfer(op);
        break;
    case 4:
        block_transfer((op >> 16) & 15, op & 0xffff, op & (1u << 24), op & (1u << 23), op & (1u << 21),
                       op & (1u << 20), op & (1u << 22));
        break;
    case 5:
        if (op & (1u << 24)) set_guest(14, fixed(r15_ - 4));
        refill_static(r15_ + static_cast<uint32_t>(static_cast<int32_t>(op << 8) >> 6), false);
        break;
    case 6: exception(kModeUndefined, 0x04, true); break;  // LDC and STC: no coprocessor answers.
    default:
        if (op & (1u << 24)) exception(kModeSupervisor, 0x08, false);
        else if ((op & 0x10) && ((op >> 8) & 15) == 14) debug_channel(op);
        else exception(kModeUndefined, 0x04, true);  // CDP, MRC and MCR on other coprocessors.
        break;
    }
}

void Compiler::data_processing(uint32_t op) {
    const unsigned opcode = (op >> 21) & 15, rn = (op >> 16) & 15, rd = (op >> 12) & 15, rm = op & 15;
    const bool s = op & (1u << 20);
    const bool test = (opcode & 0xc) == 8, logic = (kLogical >> opcode) & 1;
    // With Rd = R15 and S, CPSR = SPSR of the current mode, where it has one
    // (see Executor::data_processing()).
    const bool restore = rd == 15 && s && bank_;
    const bool set = s && !restore;
    const bool register_shift = !(op & (1u << 25)) && (op & 0x10);
    // ADD with Rm shifted left by 1 to 3 is one Zba instruction. With S,
    // its flags stay pending on Rd and Rn.
    const unsigned amount = (op >> 7) & 31;
    if (rv::kZb && opcode == 4 && !(op & (1u << 25)) && !(op & 0x70) && amount && amount <= 3 && rd != 15 &&
        rn != 15 && rm != 15 && !(set && rn == rd)) {
        static constexpr rv::Instruction (*kShiftAdd[])(R, R, R){rv::sh1add, rv::sh2add, rv::sh3add};
        if (set) kill(true, true, true);
        clobber(host(rd));
        emit(kShiftAdd[amount - 1](host(rd), host(rm), host(rn)));
        if (set) {
            flags_.arith = kArithAdd;
            flags_.result = register_operand(host(rd));
            flags_.first = register_operand(host(rn));
            flags_.second = {kUnknown, R::zero, 0};
            flags_.nz = kNzArith;
            flags_.c = kCArith;
            flags_.v = true;
        }
        sequential_ = 1;
        return;
    }
    // A shift by a register takes an internal cycle, after which R15 reads
    // as PC + 12; Rs reads as PC + 8.
    if (register_shift) idle();
    const uint32_t late = register_shift ? r15_ + 4 : r15_;
    // Where the shifter puts a shifted register: MOV shifts straight into
    // Rd, unless its carry would come from the Rm it overwrites; a second
    // operand that a pending flag reads stays in a held register.
    const bool shifted = !(op & (1u << 25)) && (register_shift || ((op & 0xff0) && rm != 15));
    R target = R::a1;
    if (shifted && opcode == 13 && rd != 15 && !(set && !register_shift && rm == rd)) {
        if (set) kill(true, false, false);
        clobber(host(rd));
        target = host(rd);
    } else if (shifted && set && !logic && !(opcode >= 5 && opcode <= 7) && (test || rd == rn)) {
        target = hold();
    }
    Carry carry = kKeepCarry;
    Value b;
    if (op & (1u << 25)) {
        const auto rotate = static_cast<int>((op >> 7) & 30);
        b = fixed(std::rotr(op & 0xff, rotate));
        if (rotate) carry = {kCarryConstant, R::zero, static_cast<uint8_t>(b.constant >> 31)};
    } else if (!register_shift) {
        b = shift_immediate((op >> 5) & 3, guest(rm, late), (op >> 7) & 31, set && logic, carry, target);
    } else {
        b = shift_register((op >> 5) & 3, guest(rm, late), guest((op >> 8) & 15, r15_), set && logic, target);
        if (set && logic) carry = {kCarryDone, R::zero, 0};
    }
    const Value a = guest(rn, late);
    if (test) {
        alu(opcode, a, b, carry, set, R::zero);
        if (restore) {
            // A test instruction: an interrupt the line holds still enters
            // after it when it sets I (MappedBus::irq_masked_late()).
            sync();
            li(R::a1, 1);
            call(stubs_.restore_cpsr);
            leave(r15_ + 4, false, !register_shift, Arm7JitExit::control_flow, true);
            return;
        }
        sequential_ = !register_shift;
        return;
    }
    if (rd != 15) {
        alu(opcode, a, b, carry, set, host(rd));
        sequential_ = !register_shift;
        return;
    }
    const R result = hold();
    alu(opcode, a, b, carry, set, result);
    if (restore) return_from_exception(result);
    else refill_dynamic(result, false);
}

// The fields MSR writes (GBATEK "ARM Opcodes: PSR Transfer (MRS, MSR)"), in
// User mode only the flags of CPSR.
uint32_t Compiler::msr_mask(uint32_t op) const {
    uint32_t mask = 0;
    if (op & (1u << 16)) mask |= 0x000000ff;
    if (op & (1u << 17)) mask |= 0x0000ff00;
    if (op & (1u << 18)) mask |= 0x00ff0000;
    if (op & (1u << 19)) mask |= 0xff000000;
    if (!(op & (1u << 22)) && mode_ == kModeUser) mask &= 0xff000000;
    return mask;
}

// MRS and MSR, see Executor::psr_transfer(). The SPSRs and CPSR bits other
// than NZCV are in Arm7State. MSR that may change the mode, T, I or F calls
// a helper and ends the block.
void Compiler::psr_transfer(uint32_t op) {
    const bool saved = op & (1u << 22);
    const int32_t spsr = kStateSpsr + 4 * static_cast<int32_t>(bank_ - 1);
    if (!(op & (1u << 21))) {
        // MRS. A mode without an SPSR reads CPSR, as Executor::spsr().
        const unsigned rd = (op >> 12) & 15;
        if (saved && bank_) {
            state_pointer(R::a0);
            emit(rv::lw(R::a0, R::a0, spsr));
        } else {
            materialize_all();
            call(stubs_.read_cpsr);
        }
        if (rd == 15) {
            refill_dynamic(R::a0, false);
            return;
        }
        set_guest(rd, in(R::a0));
        sequential_ = 1;
        return;
    }
    const Value value =
        op & (1u << 25) ? fixed(std::rotr(op & 0xff, static_cast<int>((op >> 7) & 30))) : guest(op & 15, r15_);
    const uint32_t mask = msr_mask(op);
    sequential_ = 1;
    // Bits of `value` under `field` into those of the word at `offset`.
    const auto insert = [&](int32_t offset, uint32_t field) {
        state_pointer(R::a0);
        emit(rv::lw(R::a1, R::a0, offset));
        li(R::a2, ~field);
        emit(rv::and_(R::a1, R::a1, R::a2));
        if (!value.fixed) {
            li(R::a2, field);
            emit(rv::and_(R::a2, value.reg, R::a2));
            emit(rv::or_(R::a1, R::a1, R::a2));
        } else if (value.constant & field) {
            li(R::a2, value.constant & field);
            emit(rv::or_(R::a1, R::a1, R::a2));
        }
        emit(rv::sw(R::a1, R::a0, offset));
    };
    if (saved) {
        if (bank_ && mask) insert(spsr, mask);
        return;
    }
    if ((mode_ & 0x10) && !(mask & 0x00ffffff)) {
        // Only the flag field: NZCV in their registers, bits 27-24 in Arm7State.
        if (!mask) return;
        flags_from(value);
        insert(kStateCpsr, 0x0f000000);
        return;
    }
    sync();
    load_value(R::a1, value);
    li(R::a2, mask);
    call(stubs_.write_cpsr);
    leave(r15_ + 4, false, true, Arm7JitExit::control_flow, true);
}

// MRC and MCR on CP14, see Executor::debug_channel(): MRC reads the opcode
// the prefetch just fetched, which Arm7State::pipeline[1] holds.
void Compiler::debug_channel(uint32_t op) {
    idle();
    if (op & (1u << 20)) {
        // Mapped blocks do not keep the pipeline: the opcode is the one at
        // the address the prefetch fetched.
        if (mapped_) {
            li(R::a3, code_.read(code_.context, r15_, false));
        } else {
            state_pointer(R::a0);
            emit(rv::lw(R::a3, R::a0, kStatePipeline + 4));
        }
        idle();
        const unsigned rd = (op >> 12) & 15;
        if (rd == 15) flags_from(in(R::a3));
        else set_guest(rd, in(R::a3));
    }
    sequential_ = 0;
}

// MUL and MLA: the operands are read after the prefetch, R15 as PC + 12.
// Without the accumulator, C stays pending on the operands (kCMultiply).
void Compiler::multiply(uint32_t op) {
    const unsigned rd = (op >> 16) & 15;
    const bool accumulate = op & (1u << 21), s = op & (1u << 20);
    const uint32_t late = r15_ + 4;
    Value multiplicand = guest(op & 15, late), multiplier = guest((op >> 8) & 15, late);
    const Value addend = accumulate ? guest((op >> 12) & 15, late) : fixed(0);
    multiply_cycles(multiplier, true, accumulate ? 1 : 0);
    idle_loop();
    const R dest = rd == 15 ? hold() : host(rd);
    if (s && !accumulate) {
        for (Value* v : {&multiplicand, &multiplier}) {
            if (!v->fixed && v->reg != dest) continue;
            const R h = hold();
            load_value(h, *v);
            *v = in(h);
        }
    }
    emit(rv::mul(R::a0, reg(multiplicand, R::a1), reg(multiplier, R::t6)));
    if (accumulate) emit(rv::add(R::a0, R::a0, reg(addend, R::a1)));
    if (s) {
        kill(true, true, false);
        if (accumulate) {
            store_multiply(multiplicand, multiplier, kMultiplySigned | kMultiplyAccumulate, addend, fixed(0));
            call(stubs_.carry);
        } else {
            flags_.c = kCMultiply;
            flags_.c_first = multiplicand.reg;
            flags_.c_second = static_cast<uint8_t>(number(multiplier.reg));
        }
    }
    clobber(dest);
    mv(dest, R::a0);
    if (s) {
        flags_.nz = kNzRegister;
        flags_.nz_reg = dest;
    }
    if (rd == 15) {
        refill_dynamic(dest, false);
        return;
    }
    sequential_ = 0;
}

// The arguments of the carry stub in the frame.
void Compiler::store_multiply(Value multiplicand, Value multiplier, uint32_t kind, Value low, Value high) {
    const Value values[]{multiplicand, multiplier, fixed(kind), low, high};
    for (unsigned i = 0; i < 5; ++i)
        emit(rv::sw(reg(values[i], R::t6), R::sp, kFrameMultiply + 4 * static_cast<int32_t>(i)));
}

// UMULL, UMLAL, SMULL and SMLAL; RdHi wins when both are the same register.
void Compiler::multiply_long(uint32_t op) {
    const unsigned high = (op >> 16) & 15, low = (op >> 12) & 15;
    const bool is_signed = op & (1u << 22), accumulate = op & (1u << 21), s = op & (1u << 20);
    const uint32_t late = r15_ + 4;
    const Value multiplicand = guest(op & 15, late), multiplier = guest((op >> 8) & 15, late);
    const Value low_value = accumulate ? guest(low, late) : fixed(0);
    const Value high_value = accumulate ? guest(high, late) : fixed(0);
    multiply_cycles(multiplier, is_signed, accumulate ? 2 : 1);
    idle_loop();
    const R x = reg(multiplicand, R::a2), y = reg(multiplier, R::t6);
    emit(rv::mul(R::a0, x, y));
    emit((is_signed ? rv::mulh : rv::mulhu)(R::a1, x, y));
    if (accumulate) {
        const R addend = reg(low_value, R::a2);
        emit(rv::add(R::a0, R::a0, addend));
        emit(rv::sltu(R::a3, R::a0, addend));
        emit(rv::add(R::a1, R::a1, reg(high_value, R::t6)));
        emit(rv::add(R::a1, R::a1, R::a3));
    }
    if (s) {
        kill(true, true, false);
        store_multiply(multiplicand, multiplier,
                       (is_signed ? kMultiplySigned : 0) | kMultiplyLong | (accumulate ? kMultiplyAccumulate : 0),
                       low_value, high_value);
        call(stubs_.carry);
        // N from bit 63, Z from all 64 bits.
        mv(kN, R::a1);
        emit(rv::or_(kZ, R::a0, R::a1));
    }
    if (low != 15) set_guest(low, in(R::a0));
    if (high != 15) set_guest(high, in(R::a1));
    if (low == 15 || high == 15) {
        refill_dynamic(high == 15 ? R::a1 : R::a0, false);
        return;
    }
    sequential_ = 0;
}

// SWP and SWPB: a locked read, then the write (TRM 6.11), the registers read
// after the prefetch, R15 as PC + 12.
void Compiler::swap(uint32_t op) {
    const bool byte = op & (1u << 22);
    const unsigned rd = (op >> 12) & 15;
    load_value(R::a2, guest((op >> 16) & 15, r15_ + 4));
    read((byte ? kByte : kWord) | kLock);
    if (!byte) rotate_loaded(3);
    const R value = hold();
    mv(value, R::a0);
    const Value source = guest(op & 15, r15_ + 4);
    if (!byte) load_value(R::a3, source);
    else if (source.fixed) li(R::a3, source.constant & 0xff);
    else emit(rv::andi(R::a3, source.reg, 0xff));
    write((byte ? kByte : kWord) | kLock);
    idle();
    if (rd == 15) {
        refill_dynamic(value, false);
        return;
    }
    set_guest(rd, in(value));
    sequential_ = 0;
}

// LDR, STR and their halfword, byte and signed forms. Post-indexing always
// writes back. A load into the base wins over the write-back; with R15 as
// the base, the write-back refills from the new base.
void Compiler::transfer(unsigned rn, unsigned rd, Value offset, bool up, bool pre, bool writeback, bool load,
                        Load kind, uint32_t width, unsigned scale) {
    const Value base = guest(rn, r15_);
    hint(rn);
    // base + offset, or with scale, base + (offset << scale) in one Zba
    // instruction.
    const auto address = [&](R d) {
        static constexpr rv::Instruction (*kShiftAdd[])(R, R, R){rv::sh1add, rv::sh2add, rv::sh3add};
        if (scale) emit(kShiftAdd[scale - 1](d, offset.reg, base.reg));
        else binary(up ? Op::add : Op::sub, d, base, offset);
    };
    R moved = R::a2;
    if (pre) {
        address(R::a2);
    } else {
        moved = hold();
        address(moved);
        load_value(R::a2, base);
    }
    if (load) {
        this->load(kind);
        if (writeback && rn != 15) set_guest(rn, in(moved));
        if (rd == 15 || !writeback || rn != 15) {
            finish_load(rd);
            return;
        }
        mv(R::a3, R::a0);
        idle();
        set_guest(rd, in(R::a3));
        refill_dynamic(moved, false);
        return;
    }
    store(width, guest(rd, r15_ + 4));
    if (writeback) {
        if (rn == 15) {
            refill_dynamic(moved, false);
            return;
        }
        set_guest(rn, in(moved));
    }
    sequential_ = 0;
}

// A register offset shifted left by 1 to 3 and added, as arrays of
// halfwords and words index, is scaled by Zba.
void Compiler::single_transfer(uint32_t op) {
    const unsigned rn = (op >> 16) & 15, rm = op & 15, amount = (op >> 7) & 31;
    Value offset = fixed(op & 0xfff);
    unsigned scale = 0;
    if ((op & (1u << 25)) && rv::kZb && !(op & 0x60) && amount && amount <= 3 && (op & (1u << 23)) && rn != 15 &&
        rm != 15) {
        offset = in(host(rm));
        scale = amount;
    } else if (op & (1u << 25)) {
        Carry ignored = kKeepCarry;
        offset = shift_immediate((op >> 5) & 3, guest(rm, r15_), amount, false, ignored, R::a1);
    }
    const bool pre = op & (1u << 24), byte = op & (1u << 22);
    // LDR Rd, [PC, #offset] of an aligned word.
    if ((op & 0x037f0003) == 0x011f0000 && m_literal(op & (1u << 23) ? r15_ + (op & 0xfff) : r15_ - (op & 0xfff),
                                                     (op >> 12) & 15))
        return;
    transfer(rn, (op >> 12) & 15, offset, op & (1u << 23), pre, !pre || (op & (1u << 21)), op & (1u << 20),
             byte ? Load::byte : Load::word, byte ? kByte : kWord, scale);
}

// The stores with S set are ARMv5's STRD and LDRD, stored as STRH, see
// Executor::halfword_transfer().
void Compiler::halfword_transfer(uint32_t op) {
    const Value offset = op & (1u << 22) ? fixed(((op >> 4) & 0xf0) | (op & 0xf)) : guest(op & 15, r15_);
    static constexpr Load kLoads[4]{Load::signed_half, Load::half, Load::signed_byte, Load::signed_half};
    const bool pre = op & (1u << 24);
    transfer((op >> 16) & 15, (op >> 12) & 15, offset, op & (1u << 23), pre, !pre || (op & (1u << 21)),
             op & (1u << 20), kLoads[(op >> 5) & 3], kHalf);
}

}  // namespace gbx::gba::arm7_compiler
