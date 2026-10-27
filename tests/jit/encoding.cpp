#include <array>
#include <cstdarg>
#include <cstdio>
#include <initializer_list>

#include "gbx/jit/rv32.h"

// Emit our bytes beside independent assembler input, not a golden derived from
// our own decoder. tools/check_rv32_encoding.py assembles and links the text.
namespace rv = gbx::jit::rv32;
using rv::Reg;
constexpr std::array regs{Reg::zero, Reg::ra, Reg::s0, Reg::a7, Reg::t6};
constexpr unsigned number(Reg r) { return static_cast<unsigned>(r); }

bool good = true;
[[gnu::format(printf, 2, 3)]] void print(rv::Instruction word, const char* format, ...) {
    good &= word.valid;
    std::printf(word.compressed ? "%04x\t" : "%08x\t", word.bits);
    va_list operands;
    va_start(operands, format);
    std::vprintf(format, operands);
    va_end(operands);
    std::printf("\n");
}

int main() {
    struct R { const char* name; rv::Instruction (*encode)(Reg, Reg, Reg); };
    constexpr R r_ops[]{
        {"add", rv::add}, {"sub", rv::sub}, {"sll", rv::sll}, {"slt", rv::slt},
        {"sltu", rv::sltu}, {"xor", rv::xor_}, {"srl", rv::srl}, {"sra", rv::sra},
        {"or", rv::or_}, {"and", rv::and_}, {"mul", rv::mul}, {"mulh", rv::mulh},
        {"mulhsu", rv::mulhsu}, {"mulhu", rv::mulhu}, {"div", rv::div},
        {"divu", rv::divu}, {"rem", rv::rem}, {"remu", rv::remu}};
    for (const auto& op : r_ops) {
        for (auto d : regs) for (auto a : regs) for (auto b : regs) {
            const auto word = op.encode(d, a, b);
            if (!word.valid) return 1;
            std::printf("%08x\t%s x%u,x%u,x%u\n", word.bits, op.name, number(d), number(a), number(b));
        }
        // Every register bit, independently in each of the three fields.
        for (unsigned i = 0; i < 32; ++i) for (unsigned field = 0; field < 3; ++field) {
            const auto d = field == 0 ? static_cast<Reg>(i) : Reg::zero;
            const auto a = field == 1 ? static_cast<Reg>(i) : Reg::zero;
            const auto b = field == 2 ? static_cast<Reg>(i) : Reg::zero;
            const auto word = op.encode(d, a, b);
            if (!word.valid) return 1;
            std::printf("%08x\t%s x%u,x%u,x%u\n", word.bits, op.name, number(d), number(a), number(b));
        }
    }
    struct I { const char* name; rv::Instruction (*encode)(Reg, Reg, int32_t); bool memory; };
    constexpr I i_ops[]{
        {"addi", rv::addi, false}, {"slti", rv::slti, false}, {"sltiu", rv::sltiu, false},
        {"xori", rv::xori, false}, {"ori", rv::ori, false}, {"andi", rv::andi, false},
        {"lb", rv::lb, true}, {"lh", rv::lh, true}, {"lw", rv::lw, true},
        {"lbu", rv::lbu, true}, {"lhu", rv::lhu, true}, {"jalr", rv::jalr, true},
        {"sb", rv::sb, true}, {"sh", rv::sh, true}, {"sw", rv::sw, true}};
    constexpr int immediates[]{-2048, -2047, -1024, -32, -1, 0, 1, 16, 31, 32, 1023, 1024, 2047};
    for (const auto& op : i_ops) for (auto d : regs) for (auto a : regs) for (int imm : immediates) {
        const auto word = op.encode(d, a, imm);
        if (!word.valid) return 1;
        if (op.memory) std::printf("%08x\t%s x%u,%d(x%u)\n", word.bits, op.name, number(d), imm, number(a));
        else std::printf("%08x\t%s x%u,x%u,%d\n", word.bits, op.name, number(d), number(a), imm);
    }
    struct S { const char* name; rv::Instruction (*encode)(Reg, Reg, unsigned); };
    constexpr S s_ops[]{{"slli", rv::slli}, {"srli", rv::srli}, {"srai", rv::srai}};
    for (const auto& op : s_ops) for (auto d : regs) for (auto a : regs) for (unsigned n = 0; n < 32; ++n) {
        const auto word = op.encode(d, a, n);
        if (!word.valid) return 1;
        std::printf("%08x\t%s x%u,x%u,%u\n", word.bits, op.name, number(d), number(a), n);
    }
    constexpr I b_ops[]{{"beq", rv::beq, false}, {"bne", rv::bne, false},
        {"blt", rv::blt, false}, {"bge", rv::bge, false}, {"bltu", rv::bltu, false}, {"bgeu", rv::bgeu, false}};
    constexpr int offsets[]{-4096, -4094, -2048, -1024, -32, -2, 0, 2, 4, 16, 32, 64, 128, 256, 512, 1024, 2048, 4094};
    for (const auto& op : b_ops) for (auto a : regs) for (auto b : regs) for (int offset : offsets) {
        const auto word = op.encode(a, b, offset);
        if (!word.valid) return 1;
        std::printf("%08x\t%s x%u,x%u,.%+d\n", word.bits, op.name, number(a), number(b), offset);
    }
    for (auto d : regs) {
        for (uint32_t upper : {0u, 1u, 0x12345u, 0x80000u, 0xfffffu}) {
            std::printf("%08x\tlui x%u,%u\n", rv::lui(d, upper).bits, number(d), upper);
            std::printf("%08x\tauipc x%u,%u\n", rv::auipc(d, upper).bits, number(d), upper);
        }
        for (int offset : {-1048576, -1048574, -524288, -4096, -2048, -2, 0, 2, 4, 1024, 2048, 4096, 524288, 1048574}) {
            std::printf("%08x\tjal x%u,.%+d\n", rv::jal(d, offset).bits, number(d), offset);
            // The decoder reads back the encoding the assembler checks.
            if (rv::jal_offset(rv::jal(d, offset).bits) != offset) return 1;
        }
    }

    // Zba, Zbb and Zbs.
    for (auto d : regs) for (auto a : regs) {
        for (auto b : regs) {
            print(rv::sh1add(d, a, b), "sh1add x%u,x%u,x%u", number(d), number(a), number(b));
            print(rv::sh2add(d, a, b), "sh2add x%u,x%u,x%u", number(d), number(a), number(b));
            print(rv::sh3add(d, a, b), "sh3add x%u,x%u,x%u", number(d), number(a), number(b));
            print(rv::andn(d, a, b), "andn x%u,x%u,x%u", number(d), number(a), number(b));
            print(rv::ror(d, a, b), "ror x%u,x%u,x%u", number(d), number(a), number(b));
            print(rv::maxu(d, a, b), "maxu x%u,x%u,x%u", number(d), number(a), number(b));
        }
        print(rv::zext_h(d, a), "zext.h x%u,x%u", number(d), number(a));
        print(rv::sext_b(d, a), "sext.b x%u,x%u", number(d), number(a));
        print(rv::clz(d, a), "clz x%u,x%u", number(d), number(a));
        for (unsigned n : {0u, 1u, 12u, 25u, 31u}) {
            print(rv::bexti(d, a, n), "bexti x%u,x%u,%u", number(d), number(a), n);
            print(rv::rori(d, a, n), "rori x%u,x%u,%u", number(d), number(a), n);
            print(rv::bseti(d, a, n), "bseti x%u,x%u,%u", number(d), number(a), n);
            print(rv::bclri(d, a, n), "bclri x%u,x%u,%u", number(d), number(a), n);
            print(rv::binvi(d, a, n), "binvi x%u,x%u,%u", number(d), number(a), n);
        }
    }

    // C extension: 16-bit rows print four hex digits.
    constexpr std::array any{Reg::ra, Reg::sp, Reg::s0, Reg::a5, Reg::a7, Reg::s2, Reg::t6};
    constexpr std::array prime{Reg::s0, Reg::s1, Reg::a0, Reg::a5};
    print(rv::c_nop(), "c.nop");
    for (auto d : any) {
        for (int imm : {-32, -1, 1, 16, 31}) print(rv::c_addi(d, imm), "c.addi x%u,%d", number(d), imm);
        for (int imm : {-32, -1, 0, 1, 31}) print(rv::c_li(d, imm), "c.li x%u,%d", number(d), imm);
        if (d != Reg::sp)
            for (int upper : {1, 31, -32, -1}) print(rv::c_lui(d, upper), "c.lui x%u,0x%x", number(d), upper & 0xfffff);
        for (unsigned n : {1u, 16u, 31u}) print(rv::c_slli(d, n), "c.slli x%u,%u", number(d), n);
        for (unsigned offset : {0u, 4u, 252u}) {
            print(rv::c_lwsp(d, offset), "c.lwsp x%u,%u(x2)", number(d), offset);
            print(rv::c_swsp(d, offset), "c.swsp x%u,%u(x2)", number(d), offset);
        }
        print(rv::c_jr(d), "c.jr x%u", number(d));
        print(rv::c_jalr(d), "c.jalr x%u", number(d));
        for (auto s : any) {
            print(rv::c_mv(d, s), "c.mv x%u,x%u", number(d), number(s));
            print(rv::c_add(d, s), "c.add x%u,x%u", number(d), number(s));
        }
    }
    for (int imm : {-512, -16, 16, 496}) print(rv::c_addi16sp(imm), "c.addi16sp x2,%d", imm);
    for (auto d : prime) {
        for (unsigned imm : {4u, 32u, 1020u}) print(rv::c_addi4spn(d, imm), "c.addi4spn x%u,x2,%u", number(d), imm);
        for (unsigned n : {1u, 16u, 31u}) {
            print(rv::c_srli(d, n), "c.srli x%u,%u", number(d), n);
            print(rv::c_srai(d, n), "c.srai x%u,%u", number(d), n);
        }
        for (int imm : {-32, -1, 0, 31}) print(rv::c_andi(d, imm), "c.andi x%u,%d", number(d), imm);
        for (int offset : {-256, -2, 2, 254}) {
            print(rv::c_beqz(d, offset), "c.beqz x%u,.%+d", number(d), offset);
            print(rv::c_bnez(d, offset), "c.bnez x%u,.%+d", number(d), offset);
        }
        for (auto s : prime) {
            print(rv::c_sub(d, s), "c.sub x%u,x%u", number(d), number(s));
            print(rv::c_xor(d, s), "c.xor x%u,x%u", number(d), number(s));
            print(rv::c_or(d, s), "c.or x%u,x%u", number(d), number(s));
            print(rv::c_and(d, s), "c.and x%u,x%u", number(d), number(s));
            for (unsigned offset : {0u, 4u, 124u}) {
                print(rv::c_lw(d, s, offset), "c.lw x%u,%u(x%u)", number(d), offset, number(s));
                print(rv::c_sw(d, s, offset), "c.sw x%u,%u(x%u)", number(d), offset, number(s));
            }
        }
    }
    for (int offset : {-2048, -2, 2, 2046}) {
        print(rv::c_j(offset), "c.j .%+d", offset);
        print(rv::c_jal(offset), "c.jal .%+d", offset);
    }

    // compress(): each instruction beside the form expected from the ISA's
    // expansions, which is the instruction itself where none applies.
    using rv::compress;
    print(compress(rv::nop()), "c.nop");
    print(compress(rv::addi(Reg::a0, Reg::a0, 5)), "c.addi x10,5");
    print(compress(rv::addi(Reg::s1, Reg::s1, -32)), "c.addi x9,-32");
    print(compress(rv::addi(Reg::a0, Reg::a0, 32)), "addi x10,x10,32");
    print(compress(rv::addi(Reg::s2, Reg::s2, 0)), "addi x18,x18,0");
    print(compress(rv::addi(Reg::a0, Reg::zero, -5)), "c.li x10,-5");
    print(compress(rv::addi(Reg::s3, Reg::zero, 32)), "addi x19,x0,32");
    print(compress(rv::addi(Reg::a5, Reg::s2, 0)), "c.mv x15,x18");
    print(compress(rv::addi(Reg::a5, Reg::s2, 1)), "addi x15,x18,1");
    print(compress(rv::addi(Reg::sp, Reg::sp, -64)), "c.addi16sp x2,-64");
    print(compress(rv::addi(Reg::sp, Reg::sp, 16)), "c.addi x2,16");
    print(compress(rv::addi(Reg::a1, Reg::sp, 32)), "c.addi4spn x11,x2,32");
    print(compress(rv::addi(Reg::t0, Reg::sp, 32)), "addi x5,x2,32");
    print(compress(rv::slli(Reg::s2, Reg::s2, 16)), "c.slli x18,16");
    print(compress(rv::slli(Reg::t6, Reg::s2, 16)), "slli x31,x18,16");
    print(compress(rv::srli(Reg::a0, Reg::a0, 8)), "c.srli x10,8");
    print(compress(rv::srli(Reg::t6, Reg::t6, 8)), "srli x31,x31,8");
    print(compress(rv::srai(Reg::a0, Reg::a0, 24)), "c.srai x10,24");
    print(compress(rv::andi(Reg::a3, Reg::a3, -8)), "c.andi x13,-8");
    print(compress(rv::andi(Reg::a3, Reg::a3, 255)), "andi x13,x13,255");
    print(compress(rv::add(Reg::a0, Reg::a0, Reg::t6)), "c.add x10,x31");
    print(compress(rv::add(Reg::t6, Reg::t6, Reg::a7)), "c.add x31,x17");
    print(compress(rv::add(Reg::a0, Reg::a2, Reg::a0)), "c.add x10,x12");
    print(compress(rv::add(Reg::a0, Reg::zero, Reg::a3)), "c.mv x10,x13");
    print(compress(rv::add(Reg::a0, Reg::a3, Reg::zero)), "c.mv x10,x13");
    print(compress(rv::add(Reg::a0, Reg::a1, Reg::a2)), "add x10,x11,x12");
    print(compress(rv::sub(Reg::a3, Reg::a3, Reg::a2)), "c.sub x13,x12");
    print(compress(rv::sub(Reg::a3, Reg::a2, Reg::a3)), "sub x13,x12,x13");
    print(compress(rv::xor_(Reg::a3, Reg::a1, Reg::a3)), "c.xor x13,x11");
    print(compress(rv::or_(Reg::a3, Reg::a3, Reg::a2)), "c.or x13,x12");
    print(compress(rv::and_(Reg::s2, Reg::s2, Reg::a0)), "and x18,x18,x10");
    print(compress(rv::lw(Reg::a5, Reg::a1, 28)), "c.lw x15,28(x11)");
    print(compress(rv::lw(Reg::ra, Reg::t6, -4)), "lw x1,-4(x31)");
    print(compress(rv::lw(Reg::s2, Reg::sp, 8)), "c.lwsp x18,8(x2)");
    print(compress(rv::lw(Reg::a0, Reg::a1, 128)), "lw x10,128(x11)");
    print(compress(rv::sw(Reg::a2, Reg::a1, 4)), "c.sw x12,4(x11)");
    print(compress(rv::sw(Reg::ra, Reg::sp, 48)), "c.swsp x1,48(x2)");
    print(compress(rv::sw(Reg::t6, Reg::ra, 0)), "sw x31,0(x1)");
    print(compress(rv::lui(Reg::a1, 31)), "c.lui x11,0x1f");
    print(compress(rv::lui(Reg::a2, 0xfffe0)), "c.lui x12,0xfffe0");
    print(compress(rv::lui(Reg::a2, 0x20)), "lui x12,0x20");
    print(compress(rv::lui(Reg::sp, 1)), "lui x2,0x1");
    print(compress(rv::beq(Reg::a5, Reg::zero, 12)), "c.beqz x15,.+12");
    print(compress(rv::beq(Reg::zero, Reg::a1, -8)), "c.beqz x11,.-8");
    print(compress(rv::bne(Reg::a1, Reg::zero, 254)), "c.bnez x11,.+254");
    print(compress(rv::bne(Reg::a1, Reg::zero, 256)), "bne x11,x0,.+256");
    print(compress(rv::beq(Reg::t0, Reg::zero, 8)), "beq x5,x0,.+8");
    print(compress(rv::bltu(Reg::a6, Reg::s1, 8)), "bltu x16,x9,.+8");
    print(compress(rv::jal(Reg::zero, 2046)), "c.j .+2046");
    print(compress(rv::jal(Reg::zero, 2048)), "jal x0,.+2048");
    print(compress(rv::jal(Reg::ra, -2048)), "c.jal .-2048");
    print(compress(rv::jal(Reg::t0, 4)), "jal x5,.+4");
    print(compress(rv::jalr(Reg::zero, Reg::ra, 0)), "c.jr x1");
    print(compress(rv::jalr(Reg::ra, Reg::t6, 0)), "c.jalr x31");
    print(compress(rv::jalr(Reg::zero, Reg::ra, 8)), "jalr x0,8(x1)");
    print(compress(rv::lbu(Reg::a0, Reg::a1, 0)), "lbu x10,0(x11)");
    // Zbb and Zbs forms share opcodes and funct3 with compressible ones.
    print(compress(rv::sext_b(Reg::a0, Reg::a0)), "sext.b x10,x10");
    print(compress(rv::bexti(Reg::a0, Reg::a0, 4)), "bexti x10,x10,4");
    print(compress(rv::zext_h(Reg::a0, Reg::a0)), "zext.h x10,x10");
    print(compress(rv::sh3add(Reg::a0, Reg::a0, Reg::a1)), "sh3add x10,x10,x11");
    return good ? 0 : 1;
}
