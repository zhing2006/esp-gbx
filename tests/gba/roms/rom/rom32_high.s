@ Code placed at 09F00000h, above 16 MiB (section .rom_high in gba.ld).
@ high_arm returns its own address, found through a PC-relative load, plus
@ a constant from a literal pool next to it; high_thumb does the same in
@ Thumb state. rom32_cpu.c calls both through the three ROM mirrors.

    .section .rom_high, "ax"
    .arm
    .global high_arm
high_arm:
    sub     r0, pc, #8              @ address of high_arm as executed
    ldr     r1, high_const
    add     r0, r0, r1
    bx      lr
high_const:
    .word   0x12345678

    .thumb
    .global high_thumb
    .thumb_func
high_thumb:
    mov     r0, pc                  @ address of this instruction + 4
    ldr     r1, high_const_t
    add     r0, r0, r1
    bx      lr
    .balign 4
high_const_t:
    .word   0x0FEDCBA0
