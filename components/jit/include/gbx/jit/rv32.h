#pragma once

#include <cstdint>
#include <initializer_list>

// RV32I/M, C and some Zba/Zbb/Zbs encodings from refs/docs/riscv-isa
// (20240411). No vendor instructions. Offsets are bytes relative to the
// instruction's PC; upper immediates are the unshifted 20-bit field, not a
// complete address.
namespace gbx::jit::rv32 {

enum class Reg : uint8_t {
    zero, ra, sp, gp, tp, t0, t1, t2, s0, s1,
    a0, a1, a2, a3, a4, a5, a6, a7, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11,
    t3, t4, t5, t6
};

struct Instruction {
    uint32_t bits = 0;
    bool valid = false;
    bool compressed = false;  // 16 bits, in the low half of bits.
};

namespace detail {
constexpr uint32_t reg(Reg r) { return static_cast<uint32_t>(r); }
constexpr bool registers(Reg a, Reg b = Reg::zero, Reg c = Reg::zero) {
    return reg(a) < 32 && reg(b) < 32 && reg(c) < 32;
}
constexpr Instruction r(uint32_t funct7, uint32_t funct3, Reg d, Reg a, Reg b) {
    return {funct7 << 25 | reg(b) << 20 | reg(a) << 15 | funct3 << 12 | reg(d) << 7 | 0x33,
            registers(d, a, b)};
}
constexpr Instruction i(uint32_t opcode, uint32_t funct3, Reg d, Reg a, int32_t imm) {
    return {(static_cast<uint32_t>(imm) & 0xfff) << 20 | reg(a) << 15 |
            funct3 << 12 | reg(d) << 7 | opcode,
            registers(d, a) && imm >= -2048 && imm <= 2047};
}
constexpr Instruction shift(uint32_t funct3, uint32_t upper, Reg d, Reg a, unsigned amount) {
    auto result = i(0x13, funct3, d, a, static_cast<int32_t>(upper | (amount & 31)));
    result.valid = result.valid && amount < 32;
    return result;
}
constexpr Instruction s(uint32_t funct3, Reg value, Reg base, int32_t offset) {
    const uint32_t imm = static_cast<uint32_t>(offset) & 0xfff;
    return {(imm >> 5) << 25 | reg(value) << 20 | reg(base) << 15 |
            funct3 << 12 | (imm & 31) << 7 | 0x23,
            registers(value, base) && offset >= -2048 && offset <= 2047};
}
constexpr Instruction b(uint32_t funct3, Reg a, Reg b, int32_t offset) {
    const uint32_t imm = static_cast<uint32_t>(offset);
    return {((imm >> 12) & 1) << 31 | ((imm >> 5) & 63) << 25 |
            reg(b) << 20 | reg(a) << 15 | funct3 << 12 |
            ((imm >> 1) & 15) << 8 | ((imm >> 11) & 1) << 7 | 0x63,
            registers(a, b) && offset >= -4096 && offset <= 4094 && !(imm & 1)};
}
constexpr Instruction u(uint32_t opcode, Reg d, uint32_t upper) {
    return {upper << 12 | reg(d) << 7 | opcode, registers(d) && upper <= 0xfffff};
}
}  // namespace detail

#define GBX_RV32_R(name, f7, f3) \
    constexpr Instruction name(Reg d, Reg a, Reg b) { return detail::r(f7, f3, d, a, b); }
GBX_RV32_R(add,  0x00, 0)
GBX_RV32_R(sub,  0x20, 0)
GBX_RV32_R(sll,  0x00, 1)
GBX_RV32_R(slt,  0x00, 2)
GBX_RV32_R(sltu, 0x00, 3)
GBX_RV32_R(xor_, 0x00, 4)
GBX_RV32_R(srl,  0x00, 5)
GBX_RV32_R(sra,  0x20, 5)
GBX_RV32_R(or_,  0x00, 6)
GBX_RV32_R(and_, 0x00, 7)
GBX_RV32_R(mul,    1, 0)
GBX_RV32_R(mulh,   1, 1)
GBX_RV32_R(mulhsu, 1, 2)
GBX_RV32_R(mulhu,  1, 3)
GBX_RV32_R(div,    1, 4)
GBX_RV32_R(divu,   1, 5)
GBX_RV32_R(rem,    1, 6)
GBX_RV32_R(remu,   1, 7)
#undef GBX_RV32_R

#define GBX_RV32_I(name, op, f3) \
    constexpr Instruction name(Reg d, Reg a, int32_t imm) { return detail::i(op, f3, d, a, imm); }
GBX_RV32_I(addi,  0x13, 0)
GBX_RV32_I(slti,  0x13, 2)
GBX_RV32_I(sltiu, 0x13, 3)
GBX_RV32_I(xori,  0x13, 4)
GBX_RV32_I(ori,   0x13, 6)
GBX_RV32_I(andi,  0x13, 7)
GBX_RV32_I(lb,    0x03, 0)
GBX_RV32_I(lh,    0x03, 1)
GBX_RV32_I(lw,    0x03, 2)
GBX_RV32_I(lbu,   0x03, 4)
GBX_RV32_I(lhu,   0x03, 5)
GBX_RV32_I(jalr,  0x67, 0)
#undef GBX_RV32_I

constexpr Instruction slli(Reg d, Reg a, unsigned n) { return detail::shift(1, 0, d, a, n); }
constexpr Instruction srli(Reg d, Reg a, unsigned n) { return detail::shift(5, 0, d, a, n); }
constexpr Instruction srai(Reg d, Reg a, unsigned n) { return detail::shift(5, 0x400, d, a, n); }
constexpr Instruction sb(Reg value, Reg base, int32_t offset) { return detail::s(0, value, base, offset); }
constexpr Instruction sh(Reg value, Reg base, int32_t offset) { return detail::s(1, value, base, offset); }
constexpr Instruction sw(Reg value, Reg base, int32_t offset) { return detail::s(2, value, base, offset); }

#define GBX_RV32_B(name, f3) \
    constexpr Instruction name(Reg a, Reg b, int32_t offset) { return detail::b(f3, a, b, offset); }
GBX_RV32_B(beq, 0)
GBX_RV32_B(bne, 1)
GBX_RV32_B(blt, 4)
GBX_RV32_B(bge, 5)
GBX_RV32_B(bltu, 6)
GBX_RV32_B(bgeu, 7)
#undef GBX_RV32_B

constexpr Instruction lui(Reg d, uint32_t upper) { return detail::u(0x37, d, upper); }
constexpr Instruction auipc(Reg d, uint32_t upper) { return detail::u(0x17, d, upper); }
constexpr Instruction jal(Reg d, int32_t offset) {
    const uint32_t imm = static_cast<uint32_t>(offset);
    return {((imm >> 20) & 1) << 31 | ((imm >> 1) & 1023) << 21 |
            ((imm >> 11) & 1) << 20 | ((imm >> 12) & 255) << 12 | detail::reg(d) << 7 | 0x6f,
            detail::registers(d) && offset >= -1048576 && offset <= 1048574 && !(imm & 1)};
}
// The byte offset a jal encodes.
constexpr int32_t jal_offset(uint32_t bits) {
    const uint32_t imm = (bits >> 31 & 1) << 20 | (bits >> 12 & 255) << 12 | (bits >> 20 & 1) << 11 |
        (bits >> 21 & 1023) << 1;
    return static_cast<int32_t>(imm << 11) >> 11;
}
// The register a jal links through.
constexpr Reg jal_link(uint32_t bits) { return static_cast<Reg>(bits >> 7 & 31); }
constexpr Instruction ret() { return jalr(Reg::zero, Reg::ra, 0); }
constexpr Instruction nop() { return addi(Reg::zero, Reg::zero, 0); }

// Zba, Zbb and Zbs ("B" Extension for Bit Manipulation 1.0.0), the forms code
// generation uses. ESP32-P4 v3.2 executes them and v1.3 raises an illegal-
// instruction exception (run_jit.py --group extensions), so only builds for
// v3.x and later define GBX_RV32_ZB and may emit them.
#ifdef GBX_RV32_ZB
inline constexpr bool kZb = true;
#else
inline constexpr bool kZb = false;
#endif
// d = (a << 1) + b, (a << 2) + b, (a << 3) + b
constexpr Instruction sh1add(Reg d, Reg a, Reg b) { return detail::r(0x10, 2, d, a, b); }
constexpr Instruction sh2add(Reg d, Reg a, Reg b) { return detail::r(0x10, 4, d, a, b); }
constexpr Instruction sh3add(Reg d, Reg a, Reg b) { return detail::r(0x10, 6, d, a, b); }
// d = a & ~b
constexpr Instruction andn(Reg d, Reg a, Reg b) { return detail::r(0x20, 7, d, a, b); }
// d = a rotated right by b & 31, or by n
constexpr Instruction ror(Reg d, Reg a, Reg b) { return detail::r(0x30, 5, d, a, b); }
constexpr Instruction rori(Reg d, Reg a, unsigned n) { return detail::shift(5, 0x600, d, a, n); }
// d = the larger of a and b, unsigned
constexpr Instruction maxu(Reg d, Reg a, Reg b) { return detail::r(0x05, 7, d, a, b); }
// d = the number of leading zero bits of a, 32 for zero
constexpr Instruction clz(Reg d, Reg a) { return detail::shift(1, 0x600, d, a, 0); }
// d = a & 0xffff, in RV32's encoding
constexpr Instruction zext_h(Reg d, Reg a) { return detail::r(0x04, 4, d, a, Reg::zero); }
// d = a sign-extended from bit 7
constexpr Instruction sext_b(Reg d, Reg a) { return detail::shift(1, 0x600, d, a, 4); }
// d = a >> n & 1
constexpr Instruction bexti(Reg d, Reg a, unsigned n) { return detail::shift(5, 0x480, d, a, n); }
// d = a with bit n set, cleared, inverted
constexpr Instruction bseti(Reg d, Reg a, unsigned n) { return detail::shift(1, 0x280, d, a, n); }
constexpr Instruction bclri(Reg d, Reg a, unsigned n) { return detail::shift(1, 0x480, d, a, n); }
constexpr Instruction binvi(Reg d, Reg a, unsigned n) { return detail::shift(1, 0x680, d, a, n); }

// C extension ("C" Extension for Compressed Instructions, RV32 forms).
// Each is valid only where it expands to the instruction named in its
// comment; HINT and reserved encodings are invalid.
namespace detail {
constexpr bool prime(Reg r) { return reg(r) >= 8 && reg(r) < 16; }
constexpr uint32_t bit(uint32_t value, unsigned from, unsigned to) { return (value >> from & 1) << to; }
constexpr Instruction c(uint32_t bits, bool valid) { return {bits & 0xffff, valid, true}; }
// CR: funct4, rd/rs1, rs2.
constexpr Instruction cr(uint32_t funct4, Reg d, Reg s, bool valid) {
    return c(funct4 << 12 | reg(d) << 7 | reg(s) << 2 | 2, valid && registers(d, s));
}
// CI with a 6-bit signed immediate: funct3, rd/rs1, imm, quadrant.
constexpr Instruction ci(uint32_t funct3, Reg d, int32_t imm, uint32_t quadrant, bool valid) {
    const auto u = static_cast<uint32_t>(imm);
    return c(funct3 << 13 | bit(u, 5, 12) | reg(d) << 7 | (u & 31) << 2 | quadrant,
             valid && registers(d) && imm >= -32 && imm <= 31);
}
// CA: funct2 selects SUB, XOR, OR or AND.
constexpr Instruction ca(uint32_t funct2, Reg d, Reg s) {
    return c(0x8c01 | (reg(d) & 7) << 7 | funct2 << 5 | (reg(s) & 7) << 2, prime(d) && prime(s));
}
// CB shifts and ANDI: funct2 at bits 11:10.
constexpr Instruction cb(uint32_t funct2, Reg d, int32_t imm, bool valid) {
    const auto u = static_cast<uint32_t>(imm);
    return c(0x8001 | bit(u, 5, 12) | funct2 << 10 | (reg(d) & 7) << 7 | (u & 31) << 2, valid && prime(d));
}
// CJ: offset[11|4|9:8|10|6|7|3:1|5].
constexpr Instruction cj(uint32_t funct3, int32_t offset) {
    const auto u = static_cast<uint32_t>(offset);
    return c(funct3 << 13 | bit(u, 11, 12) | bit(u, 4, 11) | bit(u, 9, 10) | bit(u, 8, 9) | bit(u, 10, 8) |
             bit(u, 6, 7) | bit(u, 7, 6) | (u >> 1 & 7) << 3 | bit(u, 5, 2) | 1,
             offset >= -2048 && offset <= 2046 && !(u & 1));
}
// CB branches: offset[8|4:3], rs1', offset[7:6|2:1|5].
constexpr Instruction cbranch(uint32_t funct3, Reg s, int32_t offset) {
    const auto u = static_cast<uint32_t>(offset);
    return c(funct3 << 13 | bit(u, 8, 12) | (u >> 3 & 3) << 10 | (reg(s) & 7) << 7 | (u >> 6 & 3) << 5 |
             (u >> 1 & 3) << 3 | bit(u, 5, 2) | 1,
             prime(s) && offset >= -256 && offset <= 254 && !(u & 1));
}
// CL and CS words: offset[5:3] at 12:10, offset[2|6] at 6:5.
constexpr Instruction cls(uint32_t funct3, Reg r, Reg base, uint32_t offset) {
    return c(funct3 << 13 | (offset >> 3 & 7) << 10 | (reg(base) & 7) << 7 | bit(offset, 2, 6) |
             bit(offset, 6, 5) | (reg(r) & 7) << 2,
             prime(r) && prime(base) && offset < 128 && !(offset & 3));
}
}  // namespace detail

constexpr Instruction c_nop() { return detail::c(1, true); }
// addi d, d, imm
constexpr Instruction c_addi(Reg d, int32_t imm) { return detail::ci(0, d, imm, 1, d != Reg::zero && imm); }
// addi d, zero, imm
constexpr Instruction c_li(Reg d, int32_t imm) { return detail::ci(2, d, imm, 1, d != Reg::zero); }
// lui d, upper: a nonzero upper field from -32 to 31, sign-extended.
constexpr Instruction c_lui(Reg d, int32_t upper) {
    return detail::ci(3, d, upper, 1, d != Reg::zero && d != Reg::sp && upper);
}
// addi sp, sp, imm
constexpr Instruction c_addi16sp(int32_t imm) {
    const auto u = static_cast<uint32_t>(imm);
    return detail::c(0x6101 | detail::bit(u, 9, 12) | detail::bit(u, 4, 6) | detail::bit(u, 6, 5) |
                     (u >> 7 & 3) << 3 | detail::bit(u, 5, 2),
                     imm && imm >= -512 && imm <= 496 && !(u & 15));
}
// addi d, sp, imm
constexpr Instruction c_addi4spn(Reg d, uint32_t imm) {
    return detail::c((imm >> 4 & 3) << 11 | (imm >> 6 & 15) << 7 | detail::bit(imm, 2, 6) |
                     detail::bit(imm, 3, 5) | (detail::reg(d) & 7) << 2,
                     detail::prime(d) && imm && imm < 1024 && !(imm & 3));
}
// srli d, d, n; srai d, d, n; andi d, d, imm
constexpr Instruction c_srli(Reg d, unsigned n) { return detail::cb(0, d, static_cast<int32_t>(n), n && n < 32); }
constexpr Instruction c_srai(Reg d, unsigned n) { return detail::cb(1, d, static_cast<int32_t>(n), n && n < 32); }
constexpr Instruction c_andi(Reg d, int32_t imm) { return detail::cb(2, d, imm, imm >= -32 && imm <= 31); }
// d = d op s
constexpr Instruction c_sub(Reg d, Reg s) { return detail::ca(0, d, s); }
constexpr Instruction c_xor(Reg d, Reg s) { return detail::ca(1, d, s); }
constexpr Instruction c_or(Reg d, Reg s) { return detail::ca(2, d, s); }
constexpr Instruction c_and(Reg d, Reg s) { return detail::ca(3, d, s); }
// jal zero, offset; jal ra, offset
constexpr Instruction c_j(int32_t offset) { return detail::cj(5, offset); }
constexpr Instruction c_jal(int32_t offset) { return detail::cj(1, offset); }
// beq s, zero, offset; bne s, zero, offset
constexpr Instruction c_beqz(Reg s, int32_t offset) { return detail::cbranch(6, s, offset); }
constexpr Instruction c_bnez(Reg s, int32_t offset) { return detail::cbranch(7, s, offset); }
// slli d, d, n
constexpr Instruction c_slli(Reg d, unsigned n) {
    return detail::ci(0, d, static_cast<int32_t>(n & 31), 2, d != Reg::zero && n && n < 32);
}
// lw d, offset(sp); sw s, offset(sp)
constexpr Instruction c_lwsp(Reg d, uint32_t offset) {
    return detail::c(0x4002 | detail::bit(offset, 5, 12) | detail::reg(d) << 7 | (offset >> 2 & 7) << 4 |
                     (offset >> 6 & 3) << 2,
                     detail::registers(d) && d != Reg::zero && offset < 256 && !(offset & 3));
}
constexpr Instruction c_swsp(Reg s, uint32_t offset) {
    return detail::c(0xc002 | (offset >> 2 & 15) << 9 | (offset >> 6 & 3) << 7 | detail::reg(s) << 2,
                     detail::registers(s) && offset < 256 && !(offset & 3));
}
// jalr zero, 0(s); jalr ra, 0(s); add d, zero, s; add d, d, s
constexpr Instruction c_jr(Reg s) { return detail::cr(8, s, Reg::zero, s != Reg::zero); }
constexpr Instruction c_jalr(Reg s) { return detail::cr(9, s, Reg::zero, s != Reg::zero); }
constexpr Instruction c_mv(Reg d, Reg s) { return detail::cr(8, d, s, d != Reg::zero && s != Reg::zero); }
constexpr Instruction c_add(Reg d, Reg s) { return detail::cr(9, d, s, d != Reg::zero && s != Reg::zero); }
// lw d, offset(base); sw s, offset(base)
constexpr Instruction c_lw(Reg d, Reg base, uint32_t offset) { return detail::cls(2, d, base, offset); }
constexpr Instruction c_sw(Reg s, Reg base, uint32_t offset) { return detail::cls(6, s, base, offset); }

// The 16-bit form of a valid 32-bit instruction with the same effect, or
// the instruction itself when it has none.
constexpr Instruction compress(Instruction instruction) {
    if (!instruction.valid || instruction.compressed) return instruction;
    const uint32_t w = instruction.bits, opcode = w & 0x7f, funct3 = w >> 12 & 7, funct7 = w >> 25;
    const Reg d = static_cast<Reg>(w >> 7 & 31), a = static_cast<Reg>(w >> 15 & 31), b = static_cast<Reg>(w >> 20 & 31);
    const int32_t imm = static_cast<int32_t>(w) >> 20;
    // The forms are tried in order, each only where its registers allow it,
    // so that code generation encodes no form it does not use.
    switch (opcode) {
    case 0x13:
        switch (funct3) {
        case 0: {
            if (d == Reg::zero) return a == Reg::zero && !imm ? c_nop() : instruction;
            if (d == a) {
                if (const auto c = c_addi(d, imm); c.valid) return c;
            }
            if (a == Reg::zero) {
                if (const auto c = c_li(d, imm); c.valid) return c;
            }
            if (!imm && d != a) {
                if (const auto c = c_mv(d, a); c.valid) return c;
            }
            if (d == Reg::sp && a == Reg::sp) {
                if (const auto c = c_addi16sp(imm); c.valid) return c;
            }
            if (a == Reg::sp && imm > 0) {
                if (const auto c = c_addi4spn(d, static_cast<uint32_t>(imm)); c.valid) return c;
            }
            return instruction;
        }
        case 1: {
            if (d != a || funct7) return instruction;
            const auto c = c_slli(d, w >> 20 & 31);
            return c.valid ? c : instruction;
        }
        case 5: {
            if (d != a || (funct7 & ~0x20u)) return instruction;
            const auto c = funct7 ? c_srai(d, w >> 20 & 31) : c_srli(d, w >> 20 & 31);
            return c.valid ? c : instruction;
        }
        case 7: {
            if (d != a) return instruction;
            const auto c = c_andi(d, imm);
            return c.valid ? c : instruction;
        }
        default: return instruction;
        }
    case 0x33:
        if (funct7 == 0 && funct3 == 0) {
            if (d == a) {
                if (const auto c = c_add(d, b); c.valid) return c;
            }
            if (d == b) {
                if (const auto c = c_add(d, a); c.valid) return c;
            }
            if (a == Reg::zero) {
                if (const auto c = c_mv(d, b); c.valid) return c;
            }
            if (b == Reg::zero) {
                if (const auto c = c_mv(d, a); c.valid) return c;
            }
            return instruction;
        }
        if (funct7 == 0x20 && funct3 == 0) {
            if (d != a) return instruction;
            const auto c = c_sub(d, b);
            return c.valid ? c : instruction;
        }
        if (funct7 == 0 && (funct3 == 4 || funct3 == 6 || funct3 == 7)) {
            const auto op = funct3 == 4 ? c_xor : funct3 == 6 ? c_or : c_and;
            if (d == a) {
                if (const auto c = op(d, b); c.valid) return c;
            }
            if (d == b) {
                if (const auto c = op(d, a); c.valid) return c;
            }
        }
        return instruction;
    case 0x03: {
        if (funct3 != 2 || imm < 0) return instruction;
        if (const auto c = c_lw(d, a, static_cast<uint32_t>(imm)); c.valid) return c;
        if (a != Reg::sp) return instruction;
        const auto c = c_lwsp(d, static_cast<uint32_t>(imm));
        return c.valid ? c : instruction;
    }
    case 0x23: {
        const int32_t offset = static_cast<int32_t>(w & 0xfe000000) >> 20 | static_cast<int32_t>(w >> 7 & 31);
        if (funct3 != 2 || offset < 0) return instruction;
        if (const auto c = c_sw(b, a, static_cast<uint32_t>(offset)); c.valid) return c;
        if (a != Reg::sp) return instruction;
        const auto c = c_swsp(b, static_cast<uint32_t>(offset));
        return c.valid ? c : instruction;
    }
    case 0x37: {
        const auto c = c_lui(d, static_cast<int32_t>(w) >> 12);
        return c.valid ? c : instruction;
    }
    case 0x63: {
        if (funct3 > 1) return instruction;
        const auto u = static_cast<uint32_t>(w);
        const int32_t offset = static_cast<int32_t>((u >> 31 ? 0xfffff000u : 0) | (u << 4 & 0x800) |
                                                    (u >> 20 & 0x7e0) | (u >> 7 & 0x1e));
        const auto op = funct3 ? c_bnez : c_beqz;
        if (b == Reg::zero) {
            if (const auto c = op(a, offset); c.valid) return c;
        }
        if (a == Reg::zero) {
            if (const auto c = op(b, offset); c.valid) return c;
        }
        return instruction;
    }
    case 0x6f: {
        if (d != Reg::zero && d != Reg::ra) return instruction;
        const auto u = static_cast<uint32_t>(w);
        const int32_t offset = static_cast<int32_t>((u >> 31 ? 0xfff00000u : 0) | (u & 0xff000) |
                                                    (u >> 9 & 0x800) | (u >> 20 & 0x7fe));
        const auto c = d == Reg::zero ? c_j(offset) : c_jal(offset);
        return c.valid ? c : instruction;
    }
    case 0x67: {
        if (funct3 || imm || (d != Reg::zero && d != Reg::ra)) return instruction;
        const auto c = d == Reg::zero ? c_jr(a) : c_jalr(a);
        return c.valid ? c : instruction;
    }
    default: return instruction;
    }
}

}  // namespace gbx::jit::rv32
