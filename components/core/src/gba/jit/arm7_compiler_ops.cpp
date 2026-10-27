// What ARM and Thumb instructions share: the barrel shifter, the ALU, loads,
// stores and block transfers, with the semantics of src/gba/executor.h.

#include <bit>

#include "../executor.h"
#include "arm7_compiler.h"

namespace gbx::gba::arm7_compiler {
namespace {

// A constant through the immediate shifter, as Executor::shift_immediate();
// ROR #0 is RRX, which the caller handles.
uint32_t shift_constant(unsigned type, uint32_t value, unsigned amount, uint32_t& carry) {
    switch (type) {
    case 0:
        if (!amount) return value;
        carry = (value >> (32 - amount)) & 1;
        return value << amount;
    case 1:
        if (!amount) {
            carry = value >> 31;
            return 0;
        }
        carry = (value >> (amount - 1)) & 1;
        return value >> amount;
    case 2: {
        const unsigned n = amount ? amount : 32;
        carry = (static_cast<int32_t>(value) >> (n - 1)) & 1;
        return static_cast<uint32_t>(static_cast<int32_t>(value) >> (n == 32 ? 31 : n));
    }
    default:
        carry = (value >> (amount - 1)) & 1;
        return std::rotr(value, static_cast<int>(amount));
    }
}

}  // namespace

// The barrel shifter with an immediate amount (GBATEK "ARM Opcodes: Data
// Processing (ALU)"), into `target` unless no instruction is needed. With
// want, the carry out goes to `carry`. RRX reads C.
Value Compiler::shift_immediate(unsigned type, Value m, unsigned amount, bool want, Carry& carry, R target) {
    if (type == 3 && !amount) {
        materialize_c();
        if (m.fixed) {
            li(target, m.constant >> 1);
            emit(rv::slli(R::t6, kC, 31));
            emit(rv::or_(target, target, R::t6));
            if (want) carry = {kCarryConstant, R::zero, static_cast<uint8_t>(m.constant & 1)};
        } else {
            emit(rv::srli(R::t6, m.reg, 1));
            emit(rv::slli(target, kC, 31));
            emit(rv::or_(target, target, R::t6));
            if (want) carry = {kCarryBit, m.reg, 0};
        }
        return in(target);
    }
    if (m.fixed) {
        if (type == 0 && !amount) return m;
        uint32_t bit = 0;
        const uint32_t value = shift_constant(type, m.constant, amount, bit);
        if (want) carry = {kCarryConstant, R::zero, static_cast<uint8_t>(bit)};
        return fixed(value);
    }
    const R r = m.reg;
    switch (type) {
    case 0:
        if (!amount) return m;
        if (want) carry = {kCarryBit, r, static_cast<uint8_t>(32 - amount)};
        emit(rv::slli(target, r, amount));
        return in(target);
    case 1:
        if (want) carry = {kCarryBit, r, static_cast<uint8_t>(amount ? amount - 1 : 31)};
        if (!amount) return fixed(0);
        emit(rv::srli(target, r, amount));
        return in(target);
    case 2:
        if (want) carry = {kCarryBit, r, static_cast<uint8_t>(amount ? amount - 1 : 31)};
        emit(rv::srai(target, r, amount ? amount : 31));
        return in(target);
    default:
        if (want) carry = {kCarryBit, r, static_cast<uint8_t>(amount - 1)};
        if (rv::kZb) {
            emit(rv::rori(target, r, amount));
        } else {
            emit(rv::srli(R::t6, r, amount));
            emit(rv::slli(target, r, 32 - amount));
            emit(rv::or_(target, target, R::t6));
        }
        return in(target);
    }
}

// The shifter with the amount in the low byte of s (Executor::
// shift_register()): 0 keeps the value and C, 32 and more shift every bit
// out, ROR by a multiple of 32 keeps the value and takes C from bit 31. With
// want, C goes to kC. Clobbers a2.
Value Compiler::shift_register(unsigned type, Value m, Value s, bool want, R target) {
    if (s.fixed) li(R::a2, s.constant & 255);
    else emit(rv::andi(R::a2, s.reg, 255));
    if (want) materialize_c();
    load_value(target, m);
    const unsigned keep = want ? branch(rv::beq, R::a2, R::zero) : 0;
    switch (type) {
    case 0:
        if (want) {
            // Bit 32 - amount, while amount <= 32.
            emit(rv::addi(kC, R::a2, -1));
            emit(rv::sll(kC, target, kC));
            emit(rv::srli(kC, kC, 31));
            emit(rv::sltiu(R::t6, R::a2, 33));
            emit(rv::and_(kC, kC, R::t6));
        }
        emit(rv::sll(target, target, R::a2));
        emit(rv::sltiu(R::t6, R::a2, 32));
        emit(rv::sub(R::t6, R::zero, R::t6));
        emit(rv::and_(target, target, R::t6));
        break;
    case 1:
        if (want) {
            emit(rv::addi(kC, R::a2, -1));
            emit(rv::srl(kC, target, kC));
            emit(rv::andi(kC, kC, 1));
            emit(rv::sltiu(R::t6, R::a2, 33));
            emit(rv::and_(kC, kC, R::t6));
        }
        emit(rv::srl(target, target, R::a2));
        emit(rv::sltiu(R::t6, R::a2, 32));
        emit(rv::sub(R::t6, R::zero, R::t6));
        emit(rv::and_(target, target, R::t6));
        break;
    case 2:
        // Amounts from 32 on shift by 31, as an all-ones amount does.
        if (want) {
            emit(rv::addi(kC, R::a2, -1));
            emit(rv::sltiu(R::t6, kC, 32));
            emit(rv::addi(R::t6, R::t6, -1));
            emit(rv::or_(kC, kC, R::t6));
            emit(rv::sra(kC, target, kC));
            emit(rv::andi(kC, kC, 1));
        }
        emit(rv::sltiu(R::t6, R::a2, 32));
        emit(rv::addi(R::t6, R::t6, -1));
        emit(rv::or_(R::t6, R::t6, R::a2));
        emit(rv::sra(target, target, R::t6));
        break;
    default:
        // Bit (amount - 1) & 31, which is 31 for a multiple of 32.
        if (want) {
            emit(rv::addi(kC, R::a2, -1));
            emit(rv::srl(kC, target, kC));
            emit(rv::andi(kC, kC, 1));
        }
        if (rv::kZb) {
            emit(rv::ror(target, target, R::a2));
        } else {
            emit(rv::sub(R::t6, R::zero, R::a2));
            emit(rv::sll(R::t6, target, R::t6));
            emit(rv::srl(target, target, R::a2));
            emit(rv::or_(target, target, R::t6));
        }
        break;
    }
    if (want) bind(keep);
    return in(target);
}

// The ALU operation `opcode` (GBATEK "ARM Opcodes: Data Processing (ALU)")
// on a and b into dest, the destination's host register, or nowhere (zero)
// for TST, TEQ, CMP and CMN. Additions and subtractions with S leave their
// flags pending on two of result, first and second operand.
void Compiler::alu(unsigned opcode, Value a, Value b, Carry carry, bool set, R dest) {
    if (opcode >= 5 && opcode <= 7) {
        carry_arith(opcode, a, b, set, dest);
        return;
    }
    if ((kLogical >> opcode) & 1) {
        logical(opcode, a, b, carry, set, dest);
        return;
    }
    const bool add = opcode == 4 || opcode == 11;
    // RSB: b - a.
    const Value first = opcode == 3 ? b : a, second = opcode == 3 ? a : b;
    if (!set) {
        if (dest == R::zero) return;
        clobber(dest);
        binary(add ? Op::add : Op::sub, dest, first, second);
        return;
    }
    Operand f = pending(first, dest), s = pending(second, dest);
    const Operand r = dest == R::zero ? Operand{kUnknown, R::zero, 0} : register_operand(dest);
    const auto known = [&] {
        return static_cast<unsigned>((r.kind != kUnknown) + (f.kind != kUnknown) + (s.kind != kUnknown));
    };
    // An immediate beside a register must be small(): another is dropped
    // while two operands stay known, or held.
    if (!(f.kind == kImmediate && s.kind == kImmediate)) {
        for (Operand* o : {&f, &s}) {
            if (o->kind != kImmediate || small(o->imm)) continue;
            if (known() > 2) {
                o->kind = kUnknown;
                continue;
            }
            const R h = hold();
            li(h, o->imm);
            *o = register_operand(h);
        }
    }
    // Two of the three must stay known: a copy of an operand that the result
    // overwrites or that is in an untracked temporary.
    if (known() < 2) {
        const bool copy_first = f.kind == kUnknown;
        const R h = hold();
        load_value(h, copy_first ? first : second);
        (copy_first ? f : s) = register_operand(h);
    }
    kill(true, true, true);
    if (dest != R::zero) {
        clobber(dest);
        binary(add ? Op::add : Op::sub, dest, first, second);
    }
    flags_.arith = add ? kArithAdd : kArithSub;
    flags_.result = r;
    flags_.first = f;
    flags_.second = s;
    flags_.nz = kNzArith;
    flags_.c = kCArith;
    flags_.v = true;
}

// AND, EOR, TST, TEQ, ORR, MOV, BIC and MVN: with S, N and Z pending on the
// result, C from the shifter.
void Compiler::logical(unsigned opcode, Value a, Value b, Carry carry, bool set, R dest) {
    R d = dest;
    if (d == R::zero) {
        if (!set) return;
        d = hold();
    }
    if (set) {
        kill(true, carry.kind != kKeep, false);
        set_carry(carry);
    }
    if (dest != R::zero) clobber(d);
    static constexpr Op kOps[16]{Op::and_, Op::xor_, Op::sub, Op::sub, Op::add, Op::add, Op::add, Op::add,
                                 Op::and_, Op::xor_, Op::sub, Op::add, Op::or_, Op::mov, Op::andn, Op::mvn};
    binary(kOps[opcode], d, a, b);
    if (set) {
        flags_.nz = kNzRegister;
        flags_.nz_reg = d;
    }
}

// ADC, SBC and RSC: x + y + C, with y inverted for the last two. They read
// C, so their flags are computed at once.
void Compiler::carry_arith(unsigned opcode, Value a, Value b, bool set, R dest) {
    materialize_c();
    const Value x = opcode == 7 ? b : a, y = opcode == 7 ? a : b;
    const R xr = reg(x, R::a0);
    R yr = R::a3;
    if (opcode == 5) yr = reg(y, R::a3);
    else if (y.fixed) li(R::a3, ~y.constant);
    else emit(rv::xori(R::a3, y.reg, -1));
    emit(rv::add(R::a2, xr, yr));
    if (set) emit(rv::sltu(R::t6, R::a2, xr));
    emit(rv::add(R::a2, R::a2, kC));
    if (set) {
        // The carry of either addition; V as for an addition of x and y.
        kill(true, true, true);
        emit(rv::sltu(kC, R::a2, kC));
        emit(rv::or_(kC, kC, R::t6));
        emit(rv::xor_(R::t6, xr, R::a2));
        emit(rv::xor_(kV, yr, R::a2));
        emit(rv::and_(kV, kV, R::t6));
    }
    if (dest == R::zero) return;
    clobber(dest);
    mv(dest, R::a2);
    if (set) {
        flags_.nz = kNzRegister;
        flags_.nz_reg = dest;
    }
}

// a0 rotated right by (a2 & mask) * 8: a misaligned LDR or SWP, or LDRH at
// an odd address (GBATEK "ARM CPU Memory Alignments"). Shifts take the low
// five bits of the amount, which for words are (a2 & 3) * 8 already.
void Compiler::rotate_loaded(int32_t mask) {
    if (mask == 3) {
        emit(rv::slli(R::t6, R::a2, 3));
    } else {
        emit(rv::andi(R::t6, R::a2, mask));
        emit(rv::slli(R::t6, R::t6, 3));
    }
    if (rv::kZb) {
        emit(rv::ror(R::a0, R::a0, R::t6));
    } else {
        emit(rv::sub(R::a1, R::zero, R::t6));
        emit(rv::sll(R::a1, R::a0, R::a1));
        emit(rv::srl(R::a0, R::a0, R::t6));
        emit(rv::or_(R::a0, R::a0, R::a1));
    }
}

// A read at a2 into a0, rotated or extended as the ARM7 does.
void Compiler::load(Load kind) {
    switch (kind) {
    case Load::word:
        read(kWord);
        rotate_loaded(3);
        break;
    case Load::half:
        read(kHalf);
        rotate_loaded(1);
        break;
    case Load::signed_half:
        // An odd address sign-extends the byte there.
        read(kHalf);
        emit(rv::slli(R::a0, R::a0, 16));
        emit(rv::andi(R::t6, R::a2, 1));
        emit(rv::slli(R::t6, R::t6, 3));
        emit(rv::addi(R::t6, R::t6, 16));
        emit(rv::sra(R::a0, R::a0, R::t6));
        break;
    case Load::byte: read(kByte); break;
    default:
        read(kByte);
        out().sign_extend8(R::a0, R::a0);
        break;
    }
}

// A store of value at a2, in the low bits for halfwords and bytes.
void Compiler::store(uint32_t width, Value value) {
    if (width == kWord) load_value(R::a3, value);
    else if (value.fixed) li(R::a3, value.constant & (width == kHalf ? 0xffff : 0xff));
    else if (width == kHalf) out().zero_extend16(R::a3, value.reg);
    else emit(rv::andi(R::a3, value.reg, 0xff));
    write(width);
}

// The end of a single load from a0: an internal cycle, then the register,
// written after any write-back, or a refill for R15 (TRM 6.7).
void Compiler::finish_load(unsigned rd) {
    mv(R::a3, R::a0);
    idle();
    if (rd == 15) {
        refill_dynamic(R::a3, thumb_);
        return;
    }
    set_guest(rd, in(R::a3));
    sequential_ = 0;
}

// Where register i of User and System mode is when the current mode banks
// it: an offset into Arm7State, or -1 when it is the current one.
int Compiler::user_slot(unsigned i) const {
    if (i >= 13 && i < 15 && bank_) return kStateR13 + 4 * static_cast<int>(i - 13);
    if (i >= 8 && i < 13 && bank_ == kFiqBank) return kStateR8 + 4 * static_cast<int>(i - 8);
    return -1;
}

// LDM and STM, and Thumb's PUSH, POP, LDMIA and STMIA, as Executor::
// block_transfer() describes them: the base written back in the cycle of
// the first access, R15 stored as the instruction's address + 3L, User mode
// registers with S, and with S and R15 loaded, CPSR = SPSR.
void Compiler::block_transfer(unsigned rn, uint32_t list, bool before, bool up, bool writeback, bool load,
                              bool user) {
    const Value base = guest(rn, r15_);
    const unsigned count = list ? std::popcount(list) : 16;
    if (!list) list = 1u << 15;
    const auto bytes = static_cast<int32_t>(4 * count);
    const int32_t start = up ? (before ? 4 : 0) : -bytes + (before ? 0 : 4);
    const bool user_bank = user && !(load && (list & (1u << 15)));
    binary(Op::add, R::a2, base, fixed(static_cast<uint32_t>(start)));
    // The new base: a constant for R15, otherwise in a held register.
    const uint32_t moved_constant = base.constant + static_cast<uint32_t>(up ? bytes : -bytes);
    R moved = R::zero;
    if (writeback && !base.fixed) {
        moved = hold();
        binary(Op::add, moved, base, fixed(static_cast<uint32_t>(up ? bytes : -bytes)));
    }
    // R15 as the base is written by the refill at the end.
    const auto write_back = [&] {
        if (rn == 15) return;
        if (const int slot = user_bank ? user_slot(rn) : -1; slot >= 0) {
            state_pointer(R::t6);
            emit(rv::sw(moved, R::t6, slot));
        } else {
            set_guest(rn, in(moved));
        }
    };
    // A mapped block's transfer from a base that pointed into IWRAM or
    // EWRAM, or an ARM store into VRAM, takes the words at once where it
    // can, see m_block_inline().
    unsigned merge = 0;
    bool inline_tried = false;
    const unsigned region = rn < 15 ? regions_[rn] : 0;
    if (mapped_ && mapped_->iwram && ((region | 1) == 3 || (region == 6 && !load && !thumb_ && mapped_->vram)) &&
        rn != 15 && !user) {
        // Pending flags that read a register the transfer writes are taken
        // care of before the paths part, so that both leave the same.
        if (load)
            for (uint32_t rest = list & 0x7fff; rest; rest &= rest - 1) clobber(host(std::countr_zero(rest)));
        if (writeback) clobber(host(rn));
        merge = m_block_inline(rn, list, load, writeback, moved);
        inline_tried = true;
        cold_mode_ = true;
    }
    uint32_t request = 0;
    if (load) {
        for (uint32_t rest = list; rest; rest &= rest - 1) {
            const unsigned i = std::countr_zero(rest);
            read(kWord | request);
            if (!request && writeback) write_back();
            if (i == 15) {
                mv(R::a3, R::a0);
            } else if (const int slot = user_bank ? user_slot(i) : -1; slot >= 0) {
                state_pointer(R::t6);
                emit(rv::sw(R::a0, R::t6, slot));
            } else {
                set_guest(i, in(R::a0));
            }
            emit(rv::addi(R::a2, R::a2, 4));
            request = kSequential;
        }
        if (inline_tried) {
            bind(jump(), merge);
            cold_mode_ = false;
        }
        // The stubs keep a3, the loaded R15, through the internal cycle.
        idle();
        if (list & (1u << 15)) {
            if (user) return_from_exception(R::a3);
            else refill_dynamic(R::a3, thumb_);
            return;
        }
        if (writeback && rn == 15) {
            refill_static(moved_constant, thumb_);
            return;
        }
        sequential_ = 0;
        return;
    }
    for (uint32_t rest = list; rest; rest &= rest - 1) {
        const unsigned i = std::countr_zero(rest);
        if (i == 15) {
            // R15 as the base holds the new base unless it is the only one.
            li(R::a3, request && writeback && rn == 15 ? moved_constant : r15_ + length_);
        } else if (const int slot = user_bank ? user_slot(i) : -1; slot >= 0) {
            state_pointer(R::t6);
            emit(rv::lw(R::a3, R::t6, slot));
        } else {
            mv(R::a3, host(i));
        }
        write(kWord | request);
        if (!request && writeback) write_back();
        emit(rv::addi(R::a2, R::a2, 4));
        request = kSequential;
    }
    if (inline_tried) {
        bind(jump(), merge);
        cold_mode_ = false;
    }
    if (writeback && rn == 15) {
        refill_static(moved_constant, thumb_);
        return;
    }
    sequential_ = 0;
}

}  // namespace gbx::gba::arm7_compiler
