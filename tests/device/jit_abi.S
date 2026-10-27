// Independent psABI probe: seed every integer callee-saved register before
// entering generated code, then check it and restore the C++ caller's state.
// Sources: refs/docs/riscv-psabi/riscv-cc.adoc, integer register convention.
.text
.option push
.option norvc
.align 2
.global gbx_jit_abi_probe
.type gbx_jit_abi_probe, @function
gbx_jit_abi_probe:
    addi sp, sp, -80
    sw ra, 76(sp)
    sw s0, 72(sp)
    sw s1, 68(sp)
    sw s2, 64(sp)
    sw s3, 60(sp)
    sw s4, 56(sp)
    sw s5, 52(sp)
    sw s6, 48(sp)
    sw s7, 44(sp)
    sw s8, 40(sp)
    sw s9, 36(sp)
    sw s10, 32(sp)
    sw s11, 28(sp)
    sw gp, 24(sp)
    sw tp, 20(sp)
    mv t0, a0
    mv a0, a1
    li s0, 256
    li s1, 257
    li s2, 258
    li s3, 259
    li s4, 260
    li s5, 261
    li s6, 262
    li s7, 263
    li s8, 264
    li s9, 265
    li s10, 266
    li s11, 267
    jalr ra, t0, 0
    li t0, 256
    bne s0, t0, .Lfailed
    li t0, 257
    bne s1, t0, .Lfailed
    li t0, 258
    bne s2, t0, .Lfailed
    li t0, 259
    bne s3, t0, .Lfailed
    li t0, 260
    bne s4, t0, .Lfailed
    li t0, 261
    bne s5, t0, .Lfailed
    li t0, 262
    bne s6, t0, .Lfailed
    li t0, 263
    bne s7, t0, .Lfailed
    li t0, 264
    bne s8, t0, .Lfailed
    li t0, 265
    bne s9, t0, .Lfailed
    li t0, 266
    bne s10, t0, .Lfailed
    li t0, 267
    bne s11, t0, .Lfailed
    lw t0, 24(sp)
    bne gp, t0, .Lfailed
    lw t0, 20(sp)
    bne tp, t0, .Lfailed
    li a0, 1
    j .Lrestore
.Lfailed:
    li a0, 0
.Lrestore:
    lw ra, 76(sp)
    lw s0, 72(sp)
    lw s1, 68(sp)
    lw s2, 64(sp)
    lw s3, 60(sp)
    lw s4, 56(sp)
    lw s5, 52(sp)
    lw s6, 48(sp)
    lw s7, 44(sp)
    lw s8, 40(sp)
    lw s9, 36(sp)
    lw s10, 32(sp)
    lw s11, 28(sp)
    lw gp, 24(sp)
    lw tp, 20(sp)
    addi sp, sp, 80
    ret
.size gbx_jit_abi_probe, .-gbx_jit_abi_probe
.option pop
.section .note.GNU-stack,"",@progbits
