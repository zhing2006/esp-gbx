@ Cartridge header and startup shared by the GBX test ROMs.
@
@ The header follows GBATEK "GBA Cartridge Header". The Nintendo logo at
@ 004h-09Fh is Nintendo's data and is left zero: the emulators that run these
@ ROMs skip the BIOS boot sequence, which is the only code that checks it.
@ tools/gba_test_roms.py fills in the title, game code and complement check.
@ 0C0h-0FFh stay zero because 0C4h-0C9h is the cartridge GPIO window
@ (GBATEK "GBA Cart I/O Port (GPIO)"), so no code may live there.
@
@ main() returns the result: 0 when every check passed, otherwise the number
@ of the first failed check. As in jsmolka/gba-tests (lib/macros.inc,
@ m_test_eval), the result is left in r12 and the ROM ends in a branch to
@ itself. The backdrop turns green on success and red on failure.

    .section .header, "ax"
    .arm
    .global _start
_start:
    b       reset
    .fill   156, 1, 0               @ Nintendo logo, left empty
    .fill   12, 1, 0                @ game title
    .fill   4, 1, 0                 @ game code
    .ascii  "00"                    @ maker code
    .byte   0x96                    @ fixed value
    .byte   0                       @ main unit code
    .byte   0                       @ device type
    .fill   7, 1, 0                 @ reserved
    .byte   0                       @ software version
    .byte   0                       @ complement check
    .fill   2, 1, 0                 @ reserved
    .fill   64, 1, 0                @ 0C0h-0FFh, includes the GPIO window

reset:
    @ Stack pointers as the BIOS leaves them (GBATEK "BIOS RAM Usage").
    mov     r0, #0xD2               @ IRQ mode, IRQ and FIQ disabled
    msr     cpsr_c, r0
    ldr     sp, =0x03007FA0
    mov     r0, #0xD3               @ SVC mode
    msr     cpsr_c, r0
    ldr     sp, =0x03007FE0
    mov     r0, #0x1F               @ system mode, interrupts enabled
    msr     cpsr_c, r0
    ldr     sp, =0x03007F00

    @ Copy .iwram (code and initialized data) and clear .bss.
    ldr     r0, =__iwram_load
    ldr     r1, =__iwram_start
    ldr     r2, =__iwram_end
1:  cmp     r1, r2
    ldrlo   r3, [r0], #4
    strlo   r3, [r1], #4
    blo     1b
    ldr     r1, =__bss_start
    ldr     r2, =__bss_end
    mov     r3, #0
2:  cmp     r1, r2
    strlo   r3, [r1], #4
    blo     2b

    ldr     r0, =main
    mov     lr, pc
    bx      r0

    @ Report the result and stop.
    mov     r12, r0
    mov     r1, #0x04000000
    mov     r2, #0
    str     r2, [r1, #0x208]        @ IME = 0
    strh    r2, [r1]                @ DISPCNT: mode 0, no layers
    cmp     r12, #0
    ldreq   r2, =0x03E0             @ green
    ldrne   r2, =0x001F             @ red
    mov     r1, #0x05000000
    strh    r2, [r1]                @ backdrop
idle:
    b       idle
    .pool
