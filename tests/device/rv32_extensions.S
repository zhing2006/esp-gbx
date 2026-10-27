// Zcb, Zba, Zbb and Zbs instructions, one case each, assembled by GNU as
// independently of the JIT's encoders. gbx_rv32_probe() runs a case with a
// temporary exception entry, so that an instruction the core does not
// implement reports its trap cause instead of panicking.
// Sources: refs/docs/riscv-isa/src/b-st-ext.adoc and zc.adoc. With CLIC,
// exceptions enter at mtvec[31:6] << 6 (ESP-IDF components/riscv/vectors_clic.S).
.option push
.option arch, +zba, +zbb, +zbs, +zcb
.option norelax

// In internal RAM like ESP-IDF's own vectors: a 64-byte aligned input
// section in flash text would move the start of the flash text segment.
.section .iram1.gbx_rv32_trap, "ax"
.align 6
// Stores mcause at t6 and resumes 4 bytes on: each case pads a 16-bit
// instruction with c.nop. Clobbers t5 only.
gbx_rv32_trap:
    csrr t5, mcause
    sw t5, 0(t6)
    csrr t5, mepc
    addi t5, t5, 4
    csrw mepc, t5
    mret

.text
// uint32_t gbx_rv32_probe(const void* code, uint32_t io[4])
// Runs code with a1 = io[0], a2 = io[1], a3 = &io[2] and a0 = 0, interrupts
// off. io[3] receives mcause if it trapped, else stays zero. Returns a0.
.align 2
.global gbx_rv32_probe
.type gbx_rv32_probe, @function
gbx_rv32_probe:
    addi sp, sp, -16
    sw ra, 12(sp)
    sw s0, 8(sp)
    sw s1, 4(sp)
    sw s2, 0(sp)
    mv s0, a1
    csrrci s1, mstatus, 8
    andi s1, s1, 8
    csrr s2, mtvec
    la t0, gbx_rv32_trap
    andi t1, s2, 0x3f
    or t0, t0, t1
    csrw mtvec, t0
    sw zero, 12(s0)
    addi t6, s0, 12
    mv t0, a0
    lw a1, 0(s0)
    lw a2, 4(s0)
    addi a3, s0, 8
    li a0, 0
    jalr ra, t0, 0
    csrw mtvec, s2
    csrs mstatus, s1
    lw s2, 0(sp)
    lw s1, 4(sp)
    lw s0, 8(sp)
    lw ra, 12(sp)
    addi sp, sp, 16
    ret
.size gbx_rv32_probe, .-gbx_rv32_probe

// Cases: the result in a0. Order matches tests/runner/run_jit.py.
.macro case32 name, text:vararg
.align 1
.Lcase_\name:
    \text
    ret
.endm
// rd' = a0 starts as a1, then the 16-bit instruction.
.macro case16 name, text:vararg
.align 1
.Lcase_\name:
    mv a0, a1
    \text
    c.nop
.endm
// A store, then the word it changed.
.macro store16 name, text:vararg
    case16 \name, \text
    lw a0, 0(a3)
    ret
.endm

    case32 sh1add, sh1add a0, a1, a2
    case32 sh2add, sh2add a0, a1, a2
    case32 sh3add, sh3add a0, a1, a2
    case32 andn, andn a0, a1, a2
    case32 orn, orn a0, a1, a2
    case32 xnor, xnor a0, a1, a2
    case32 clz, clz a0, a1
    case32 ctz, ctz a0, a1
    case32 cpop, cpop a0, a1
    case32 max, max a0, a1, a2
    case32 maxu, maxu a0, a1, a2
    case32 min, min a0, a1, a2
    case32 minu, minu a0, a1, a2
    case32 sext_b, sext.b a0, a1
    case32 sext_h, sext.h a0, a1
    case32 zext_h, zext.h a0, a1
    case32 rol, rol a0, a1, a2
    case32 ror, ror a0, a1, a2
    case32 rori, rori a0, a1, 12
    case32 orc_b, orc.b a0, a1
    case32 rev8, rev8 a0, a1
    case32 bclr, bclr a0, a1, a2
    case32 bclri, bclri a0, a1, 15
    case32 bext, bext a0, a1, a2
    case32 bexti, bexti a0, a1, 7
    case32 binv, binv a0, a1, a2
    case32 binvi, binvi a0, a1, 31
    case32 bset, bset a0, a1, a2
    case32 bseti, bseti a0, a1, 9
    case16 c_lbu, c.lbu a0, 1(a3)
    ret
    case16 c_lhu, c.lhu a0, 2(a3)
    ret
    case16 c_lh, c.lh a0, 2(a3)
    ret
    store16 c_sb, c.sb a2, 1(a3)
    store16 c_sh, c.sh a2, 2(a3)
    case16 c_zext_b, c.zext.b a0
    ret
    case16 c_sext_b, c.sext.b a0
    ret
    case16 c_zext_h, c.zext.h a0
    ret
    case16 c_sext_h, c.sext.h a0
    ret
    case16 c_not, c.not a0
    ret
    case16 c_mul, c.mul a0, a2
    ret

.section .rodata
.align 2
.global gbx_rv32_cases
gbx_rv32_cases:
    .word .Lcase_sh1add, .Lcase_sh2add, .Lcase_sh3add
    .word .Lcase_andn, .Lcase_orn, .Lcase_xnor, .Lcase_clz, .Lcase_ctz, .Lcase_cpop
    .word .Lcase_max, .Lcase_maxu, .Lcase_min, .Lcase_minu
    .word .Lcase_sext_b, .Lcase_sext_h, .Lcase_zext_h
    .word .Lcase_rol, .Lcase_ror, .Lcase_rori, .Lcase_orc_b, .Lcase_rev8
    .word .Lcase_bclr, .Lcase_bclri, .Lcase_bext, .Lcase_bexti
    .word .Lcase_binv, .Lcase_binvi, .Lcase_bset, .Lcase_bseti
    .word .Lcase_c_lbu, .Lcase_c_lhu, .Lcase_c_lh, .Lcase_c_sb, .Lcase_c_sh
    .word .Lcase_c_zext_b, .Lcase_c_sext_b, .Lcase_c_zext_h, .Lcase_c_sext_h
    .word .Lcase_c_not, .Lcase_c_mul
.global gbx_rv32_case_count
gbx_rv32_case_count:
    .word (gbx_rv32_case_count - gbx_rv32_cases) / 4

.option pop
.section .note.GNU-stack,"",@progbits
