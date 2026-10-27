// Pending flags and conditions, see Compiler's comment in arm7_compiler.h.

#include <algorithm>

#include "../executor.h"
#include "arm7_compiler.h"

namespace gbx::gba::arm7_compiler {
namespace {

// NZCV as 8, 4, 2 and 1: the flags each condition reads (GBATEK "ARM CPU
// Flags & Condition Field"), none for AL.
constexpr std::array<uint8_t, 15> kConditionFlags{4, 4, 2, 2, 8, 8, 1, 1, 6, 6, 9, 9, 13, 13, 0};

// The flags a Thumb instruction reads and those it always writes; false
// where the code may not go on to the next instruction, as after a load
// with an empty list, which loads R15 (GBATEK "Strange Effects on Invalid
// Rlist's"). Writes that a shift by zero leaves out are not counted.
bool thumb_flags(uint32_t op, uint8_t& read, uint8_t& write) {
    read = write = 0;
    switch (op >> 13) {
    case 0:  // shifts by an immediate, ADD and SUB
        write = (op & 0x1800) == 0x1800 ? 15 : (op & 0x1800) == 0 && !((op >> 6) & 31) ? 12 : 14;
        return true;
    case 1:  // MOV, CMP, ADD and SUB with an 8-bit immediate
        write = (op & 0x1800) == 0 ? 12 : 15;
        return true;
    case 2:
        if ((op & 0xfc00) == 0x4000) {  // ALU: ADC and SBC read C; ADC, SBC, NEG, CMP, CMN set all
            const unsigned alu = (op >> 6) & 15;
            if (alu == 5 || alu == 6) read = 2;
            write = alu == 5 || alu == 6 || (alu >= 9 && alu <= 11) ? 15 : 12;
            return true;
        }
        if ((op & 0xfc00) == 0x4400) {  // high registers: CMP sets all; BX and writes to R15 branch
            const unsigned kind = (op >> 8) & 3, rd = ((op >> 4) & 8) | (op & 7);
            if (kind == 3 || (rd == 15 && kind != 1)) return false;
            write = kind == 1 ? 15 : 0;
        }
        return true;  // loads and stores
    case 3: case 4: return true;  // loads and stores
    case 5:
        // ADD Rd, PC or SP; ADD SP; PUSH; POP of a list without R15.
        return (op & 0xf000) == 0xa000 || (op & 0xff00) == 0xb000 || (op & 0xfe00) == 0xb400 ||
               ((op & 0xff00) == 0xbc00 && (op & 0xff));
    case 6:
        if ((op & 0xf000) == 0xc000) return (op & 0x08ff) != 0x0800;  // STMIA, LDMIA of a list
        if (((op >> 8) & 15) < 14) read = kConditionFlags[(op >> 8) & 15];
        return false;
    default: return false;  // B and BL
    }
}

// The same for an ARM instruction.
bool arm_flags(uint32_t op, uint8_t& read, uint8_t& write) {
    read = write = 0;
    const unsigned cond = op >> 28;
    if (cond == 15) return false;
    read = kConditionFlags[cond];
    if ((op & 0x0ffffff0) == 0x012fff10 || (op & 0x0e000000) == 0x0a000000) return false;  // BX, B and BL
    const bool multiply = (op & 0x0fc000f0) == 0x00000090 || (op & 0x0f8000f0) == 0x00800090;
    const bool block = (op & 0x0e000000) == 0x08000000;
    if ((op & 0x0c000000) == 0x04000000 || block || ((op & 0x0e000090) == 0x00000090 && (op & 0x60))) {
        // Transfers: an offset register shifted by RRX reads C. Those of
        // R15, LDM of an empty list and the S bit end the walk.
        if (block ? (op & 0x00408000) || (op & 0x0010ffff) == 0x00100000 : ((op >> 12) & 15) == 15) return false;
        if ((op & 0x0e000010) == 0x06000000 && ((op >> 5) & 3) == 3 && !((op >> 7) & 31)) read |= 2;
        return true;
    }
    if (op & 0x0c000000) return false;  // coprocessors, SWI and the undefined
    const bool set = op & (1u << 20);
    if (multiply) {
        if (set && cond == 14) write = 12;
        return true;
    }
    const unsigned opcode = (op >> 21) & 15;
    // SWP, MRS and MSR, and writes to R15, end the walk.
    if ((op & 0x0e000090) == 0x00000090 || (!set && opcode >= 8 && opcode <= 11) || ((op >> 12) & 15) == 15)
        return false;
    // ADC, SBC, RSC and an operand shifted by RRX read C.
    const bool rrx = !(op & (1u << 25)) && !(op & 0x10) && ((op >> 5) & 3) == 3 && !((op >> 7) & 31);
    if (opcode == 5 || opcode == 6 || opcode == 7 || rrx) read |= 2;
    // The arithmetic operations set all; the logical ones N and Z, and C
    // only from the shifter.
    if (set && cond == 14) write = (opcode >= 2 && opcode <= 7) || opcode == 10 || opcode == 11 ? 15 : 12;
    return true;
}

}  // namespace

// The flags the code at `address` may read before it sets them, from up
// to 8 instructions of the BIOS or the ROM, which do not change: NZCV as
// 8, 4, 2 and 1. A walk that ends at a branch, or before every flag is
// set, keeps the flags still unset. Any other code needs all of them.
uint8_t Compiler::live_flags(uint32_t address) const {
    const unsigned region = address >> 24, length = thumb_ ? 2 : 4;
    if (!(region == 0 && address + 8 * length <= kBiosBytes) && !(region >= 8 && region < 14)) return 15;
    uint8_t live = 0, unset = 15;
    for (unsigned i = 0; i < 8 && unset; ++i) {
        const uint32_t op = code_.read(code_.context, address + i * length, thumb_);
        uint8_t read, write;
        const bool more = thumb_ ? thumb_flags(op & 0xffff, read, write) : arm_flags(op, read, write);
        live |= read & unset;
        if (!more) break;
        unset &= ~write;
    }
    return live | unset;
}

// Registers whose writes the compiler sees: guest registers, and the held
// temporaries, which it hands out.
bool Compiler::tracked(R r) {
    return std::find(kGuest.begin(), kGuest.end(), r) != kGuest.end() ||
           std::find(kHeld.begin(), kHeld.end(), r) != kHeld.end();
}

bool Compiler::refers(R r) const {
    const auto& f = flags_;
    if (f.nz == kNzRegister && f.nz_reg == r) return true;
    if ((f.c == kCBit || f.c == kCMultiply) && f.c_first == r) return true;
    if (f.c == kCMultiply && static_cast<R>(f.c_second) == r) return true;
    if (f.arith != kArithNone)
        for (const Operand* o : {&f.result, &f.first, &f.second})
            if (o->kind == kRegister && o->reg == r) return true;
    return false;
}

// Drops the arithmetic source once no flag needs it.
void Compiler::settle() {
    if (flags_.nz != kNzArith && flags_.c != kCArith && !flags_.v) flags_.arith = kArithNone;
}

// The current instruction sets these flags: the pending ones are dropped.
void Compiler::kill(bool nz, bool c, bool v) {
    if (nz) flags_.nz = kNzDone;
    if (c) flags_.c = kCDone;
    if (v) flags_.v = false;
    settle();
}

// The result, carry and overflow of a pending arithmetic on two constants.
void Compiler::constant_flags(uint32_t& result, uint32_t& carry, uint32_t& overflow) const {
    const uint32_t x = flags_.first.imm, y = flags_.second.imm;
    if (flags_.arith == kArithAdd) {
        result = x + y;
        carry = result < x;
        overflow = ((x ^ result) & (y ^ result)) >> 31;
    } else {
        result = x - y;
        carry = x >= y;
        overflow = ((x ^ y) & (x ^ result)) >> 31;
    }
}

// Emitters on known pending operands, of which immediates beside a register
// are small(). t6 is scratch.
void Compiler::emit_add(R d, const Operand& a, const Operand& b) {
    if (a.kind == kImmediate && b.kind == kImmediate) li(d, a.imm + b.imm);
    else if (a.kind == kImmediate) emit(rv::addi(d, b.reg, immediate(a.imm)));
    else if (b.kind == kImmediate) emit(rv::addi(d, a.reg, immediate(b.imm)));
    else emit(rv::add(d, a.reg, b.reg));
}

void Compiler::emit_sub(R d, const Operand& a, const Operand& b) {
    if (a.kind == kImmediate && b.kind == kImmediate) {
        li(d, a.imm - b.imm);
    } else if (b.kind == kImmediate) {
        emit(rv::addi(d, a.reg, -immediate(b.imm)));
    } else if (a.kind == kImmediate) {
        emit(rv::sub(d, R::zero, b.reg));
        if (a.imm) emit(rv::addi(d, d, immediate(a.imm)));
    } else {
        emit(rv::sub(d, a.reg, b.reg));
    }
}

void Compiler::emit_xor(R d, const Operand& a, const Operand& b) {
    if (a.kind == kImmediate && b.kind == kImmediate) li(d, a.imm ^ b.imm);
    else if (a.kind == kImmediate) emit(rv::xori(d, b.reg, immediate(a.imm)));
    else if (b.kind == kImmediate) emit(rv::xori(d, a.reg, immediate(b.imm)));
    else emit(rv::xor_(d, a.reg, b.reg));
}

void Compiler::emit_sltu(R d, const Operand& a, const Operand& b) {
    if (a.kind == kImmediate && b.kind == kImmediate) {
        li(d, a.imm < b.imm);
    } else if (b.kind == kImmediate) {
        emit(rv::sltiu(d, a.reg, immediate(b.imm)));
    } else if (a.kind == kImmediate) {
        li(R::t6, a.imm);
        emit(rv::sltu(d, R::t6, b.reg));
    } else {
        emit(rv::sltu(d, a.reg, b.reg));
    }
}

// The path of a conditional instruction that merges with the other may not
// compute flags pending from before it: the other path would not have them,
// see Compiler::arm().
bool Compiler::may_materialize() {
    if (merging_) failed_ = true;
    return !merging_;
}

void Compiler::materialize_nz() {
    if (flags_.nz == kNzDone || !may_materialize()) return;
    const auto& f = flags_;
    if (f.nz == kNzRegister || f.result.kind == kRegister) {
        const R r = f.nz == kNzRegister ? f.nz_reg : f.result.reg;
        mv(kN, r);
        mv(kZ, r);
    } else {
        if (f.arith == kArithAdd) emit_add(kZ, f.first, f.second);
        else emit_sub(kZ, f.first, f.second);
        mv(kN, kZ);
    }
    flags_.nz = kNzDone;
    settle();
}

void Compiler::materialize_c() {
    if (flags_.c == kCDone || !may_materialize()) return;
    const auto& f = flags_;
    const Operand c = register_operand(kC);
    switch (f.c) {
    case kCArith:
        if (constant_arith()) {
            uint32_t result, carry, overflow;
            constant_flags(result, carry, overflow);
            li(kC, carry);
        } else if (f.arith == kArithAdd) {
            // A carry out leaves the sum below either addend.
            if (f.result.kind) {
                emit_sltu(kC, f.result, f.first.kind ? f.first : f.second);
            } else {
                emit_add(kC, f.first, f.second);
                emit_sltu(kC, c, f.first);
            }
        } else {
            // C is no borrow: first >= second.
            if (f.first.kind && f.second.kind) {
                emit_sltu(kC, f.first, f.second);
            } else if (f.second.kind) {
                emit_add(kC, f.result, f.second);
                emit_sltu(kC, c, f.second);
            } else {
                emit_sub(kC, f.first, f.result);
                emit_sltu(kC, f.first, c);
            }
            emit(rv::xori(kC, kC, 1));
        }
        break;
    case kCBit: out().extract_bit(kC, f.c_first, f.c_second); break;
    case kCZero: li(kC, 0); break;
    case kCOne: li(kC, 1); break;
    default:  // kCMultiply
        emit(rv::sw(f.c_first, R::sp, kFrameMultiply));
        emit(rv::sw(static_cast<R>(f.c_second), R::sp, kFrameMultiply + 4));
        call(stubs_.carry_mul);
        break;
    }
    flags_.c = kCDone;
    settle();
}

void Compiler::materialize_v() {
    if (!flags_.v || !may_materialize()) return;
    const auto& f = flags_;
    const Operand v = register_operand(kV), scratch = register_operand(R::t6);
    if (constant_arith()) {
        uint32_t result, carry, overflow;
        constant_flags(result, carry, overflow);
        li(kV, overflow << 31);
    } else if (f.arith == kArithAdd) {
        // (first ^ result) & (second ^ result): both addends differ in sign
        // from the sum.
        if (f.result.kind && f.first.kind && f.second.kind) {
            emit_xor(R::t6, f.first, f.result);
            emit_xor(kV, f.second, f.result);
        } else if (!f.result.kind) {
            emit_add(kV, f.first, f.second);
            emit_xor(R::t6, f.first, v);
            emit_xor(kV, f.second, v);
        } else {
            const Operand& known = f.first.kind ? f.first : f.second;
            emit_sub(R::t6, f.result, known);
            emit_xor(R::t6, scratch, f.result);
            emit_xor(kV, known, f.result);
        }
        emit(rv::and_(kV, kV, R::t6));
    } else {
        // (first ^ second) & (first ^ result).
        if (f.result.kind && f.first.kind && f.second.kind) {
            emit_xor(R::t6, f.first, f.second);
            emit_xor(kV, f.first, f.result);
        } else if (!f.result.kind) {
            emit_sub(kV, f.first, f.second);
            emit_xor(kV, f.first, v);
            emit_xor(R::t6, f.first, f.second);
        } else if (!f.first.kind) {
            emit_add(R::t6, f.result, f.second);
            emit(rv::xor_(kV, R::t6, f.result.reg));
            emit_xor(R::t6, scratch, f.second);
        } else {
            emit_sub(R::t6, f.first, f.result);
            emit_xor(R::t6, f.first, scratch);
            emit_xor(kV, f.first, f.result);
        }
        emit(rv::and_(kV, kV, R::t6));
    }
    flags_.v = false;
    settle();
}

// Before host register r is written: the pending flags that read it keep a
// copy of it in a held register, or are computed, unless the other operands
// of an arithmetic still determine them. Computing C from a bit takes one
// instruction with Zbs, as few as the copy.
void Compiler::clobber(R r) {
    auto& f = flags_;
    const bool nz = f.nz == kNzRegister && f.nz_reg == r;
    const bool c = (f.c == kCBit || f.c == kCMultiply) &&
                   (f.c_first == r || (f.c == kCMultiply && static_cast<R>(f.c_second) == r));
    bool arith = false;
    if (f.arith != kArithNone) {
        Operand* operands[]{&f.result, &f.first, &f.second};
        unsigned known = 0, hits = 0;
        for (const auto* o : operands) {
            known += o->kind != kUnknown;
            hits += o->kind == kRegister && o->reg == r;
        }
        if (hits && known - hits >= 2) {
            for (auto* o : operands)
                if (o->kind == kRegister && o->reg == r) o->kind = kUnknown;
        } else {
            arith = hits != 0;
        }
    }
    if (!nz && !c && !arith) return;
    const bool cheap = !nz && !arith && f.c == kCBit && rv::kZb;
    if (!cheap && !merging_ && relocate(r)) return;
    if (nz) materialize_nz();
    if (c) materialize_c();
    if (!arith) return;
    if (f.nz == kNzArith) materialize_nz();
    if (f.v) materialize_v();
    if (f.c == kCArith) materialize_c();
}

// The pending flags read a copy of r in a free held register instead.
bool Compiler::relocate(R r) {
    for (unsigned i = 0; i < kHeld.size(); ++i) {
        const R h = kHeld[i];
        if ((busy_ & (1u << i)) || refers(h)) continue;
        mv(h, r);
        auto& f = flags_;
        if (f.nz_reg == r) f.nz_reg = h;
        if (f.c_first == r) f.c_first = h;
        if (f.c == kCMultiply && static_cast<R>(f.c_second) == r) f.c_second = static_cast<uint8_t>(number(h));
        for (Operand* o : {&f.result, &f.first, &f.second})
            if (o->kind == kRegister && o->reg == r) o->reg = h;
        return true;
    }
    return false;
}

// Before a conditional instruction that writes these guest registers: their
// values that pending flags read move to held registers, so that both paths
// leave the same flags.
void Compiler::preserve(uint16_t writes) {
    for (unsigned i = 0; i < kGuest.size(); ++i) {
        if (!((writes >> i) & 1) || !refers(host(i))) continue;
        if (!relocate(host(i))) {
            materialize_all();
            return;
        }
    }
}

unsigned Compiler::free_held() const {
    return static_cast<unsigned>(std::count_if(kHeld.begin(), kHeld.end(), [&](R r) { return !refers(r); }));
}

// A temporary that may outlive the instruction as a flag source.
R Compiler::hold() {
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        for (unsigned i = 0; i < kHeld.size(); ++i) {
            if (!(busy_ & (1u << i)) && !refers(kHeld[i])) {
                busy_ |= static_cast<uint8_t>(1u << i);
                return kHeld[i];
            }
        }
        if (!attempt) materialize_all();
    }
    failed_ = true;
    return kHeld[0];
}

// The source a pending arithmetic keeps of a value read before `dest` is
// written.
Operand Compiler::pending(Value value, R dest) {
    if (value.fixed) return {kImmediate, R::zero, value.constant};
    if (!tracked(value.reg) || value.reg == dest) return {kUnknown, R::zero, 0};
    return register_operand(value.reg);
}

void Compiler::set_carry(const Carry& carry) {
    switch (carry.kind) {
    case kCarryConstant: flags_.c = carry.bit ? kCOne : kCZero; break;
    case kCarryBit:
        flags_.c = kCBit;
        flags_.c_first = carry.reg;
        flags_.c_second = carry.bit;
        break;
    case kCarryDone: flags_.c = kCDone; break;
    default: break;
    }
}

// NZCV from bits 31-28 of value: MSR to the flags, MRC to R15.
void Compiler::flags_from(Value value) {
    kill(true, true, true);
    if (value.fixed) {
        const uint32_t v = value.constant;
        li(kN, v & kFlagN);
        li(kZ, v & kFlagZ ? 0 : 1);
        li(kC, (v >> 29) & 1);
        li(kV, (v << 3) & kFlagN);
        return;
    }
    const R v = value.reg;
    mv(kN, v);
    out().extract_bit(kZ, v, 30);
    emit(rv::xori(kZ, kZ, 1));
    out().extract_bit(kC, v, 29);
    emit(rv::slli(kV, v, 3));
}

// Emits branches taken when condition cond (0-13, GBATEK "ARM CPU Flags &
// Condition Field") holds, into refs; returns how many. Flags it needs are
// computed before the first branch, so that every path has them.
unsigned Compiler::when(unsigned cond, std::array<unsigned, 2>& refs) {
    const auto& f = flags_;
    const auto compare = [&](Branch op, const Operand& a, const Operand& b) {
        if (a.kind == kImmediate && b.kind == kImmediate) {
            // Decided now.
            uint32_t result, carry, overflow;
            constant_flags(result, carry, overflow);
            const unsigned nzcv = (result >> 31) << 3 | (result ? 0 : 4) | carry << 1 | overflow;
            if (!((kConditions[cond] >> nzcv) & 1)) return 0u;
            refs[0] = jump();
            return 1u;
        }
        const auto operand = [&](const Operand& o) {
            if (o.kind == kRegister) return o.reg;
            if (!o.imm) return R::zero;
            li(R::t6, o.imm);
            return R::t6;
        };
        const R x = operand(a), y = operand(b);
        refs[0] = branch(op, x, y);
        return 1u;
    };
    // first - second: the signed and unsigned orders give every condition
    // but the sign and the overflow alone.
    if (f.arith == kArithSub && f.first.kind && f.second.kind) {
        const bool z = f.nz == kNzArith, c = f.c == kCArith, v = f.v;
        switch (cond) {
        case 0: if (z) return compare(rv::beq, f.first, f.second); break;
        case 1: if (z) return compare(rv::bne, f.first, f.second); break;
        case 2: if (c) return compare(rv::bgeu, f.first, f.second); break;
        case 3: if (c) return compare(rv::bltu, f.first, f.second); break;
        case 8: if (c && z) return compare(rv::bltu, f.second, f.first); break;
        case 9: if (c && z) return compare(rv::bgeu, f.second, f.first); break;
        case 10: if (z && v) return compare(rv::bge, f.first, f.second); break;
        case 11: if (z && v) return compare(rv::blt, f.first, f.second); break;
        case 12: if (z && v) return compare(rv::blt, f.second, f.first); break;
        case 13: if (z && v) return compare(rv::bge, f.second, f.first); break;
        default: break;
        }
    }
    // EQ, NE, MI and PL on a result in a register.
    if (cond < 2 || cond == 4 || cond == 5) {
        R x = R::zero;
        if (f.nz == kNzRegister) {
            x = f.nz_reg;
        } else if (f.nz == kNzArith && f.result.kind == kRegister) {
            x = f.result.reg;
        } else if (f.nz == kNzArith && cond < 2 && !constant_arith()) {
            if (f.arith == kArithAdd) emit_add(R::t6, f.first, f.second);
            else emit_sub(R::t6, f.first, f.second);
            x = R::t6;
        }
        if (x != R::zero) {
            static constexpr Branch kBranches[]{rv::beq, rv::bne, nullptr, nullptr, rv::blt, rv::bge};
            refs[0] = branch(kBranches[cond], x, R::zero);
            return 1;
        }
    }
    // From the flag registers.
    const bool n = cond == 4 || cond == 5 || cond >= 10, z = cond < 2 || cond == 8 || cond == 9 || cond >= 12;
    const bool c = cond == 2 || cond == 3 || cond == 8 || cond == 9, v = cond == 6 || cond == 7 || cond >= 10;
    if (n || z) materialize_nz();
    if (c) materialize_c();
    if (v) materialize_v();
    switch (cond) {
    case 0: refs[0] = branch(rv::beq, kZ, R::zero); return 1;
    case 1: refs[0] = branch(rv::bne, kZ, R::zero); return 1;
    case 2: refs[0] = branch(rv::bne, kC, R::zero); return 1;
    case 3: refs[0] = branch(rv::beq, kC, R::zero); return 1;
    case 4: refs[0] = branch(rv::blt, kN, R::zero); return 1;
    case 5: refs[0] = branch(rv::bge, kN, R::zero); return 1;
    case 6: refs[0] = branch(rv::blt, kV, R::zero); return 1;
    case 7: refs[0] = branch(rv::bge, kV, R::zero); return 1;
    case 8: {  // C and not Z
        const unsigned skip = branch(rv::beq, kC, R::zero);
        refs[0] = branch(rv::bne, kZ, R::zero);
        bind(skip);
        return 1;
    }
    case 9:  // not C or Z
        refs[0] = branch(rv::beq, kC, R::zero);
        refs[1] = branch(rv::beq, kZ, R::zero);
        return 2;
    case 10:
    case 11:
        emit(rv::xor_(R::t6, kN, kV));
        refs[0] = branch(cond == 10 ? rv::bge : rv::blt, R::t6, R::zero);
        return 1;
    case 12: {  // not Z and N = V
        const unsigned skip = branch(rv::beq, kZ, R::zero);
        emit(rv::xor_(R::t6, kN, kV));
        refs[0] = branch(rv::bge, R::t6, R::zero);
        bind(skip);
        return 1;
    }
    default:  // Z or N != V
        refs[0] = branch(rv::beq, kZ, R::zero);
        emit(rv::xor_(R::t6, kN, kV));
        refs[1] = branch(rv::blt, R::t6, R::zero);
        return 2;
    }
}

}  // namespace gbx::gba::arm7_compiler
