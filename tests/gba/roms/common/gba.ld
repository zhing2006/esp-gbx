/* Memory layout of the GBX test ROMs: code and constants in cartridge ROM,
 * .iwram (ARM code that has to run from IWRAM, and initialized data) copied
 * to IWRAM by crt0.s, .bss in IWRAM below the stacks. Sections named
 * .rom_high.* are placed at 09F00000h, above 16 MiB, for the 32 MiB ROM test;
 * tools/gba_test_roms.py writes them into the ROM image at that offset. */

OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY {
    rom   (rx)  : ORIGIN = 0x08000000, LENGTH = 32M
    iwram (rwx) : ORIGIN = 0x03000000, LENGTH = 0x7E00
    ewram (rw)  : ORIGIN = 0x02000000, LENGTH = 256K
}

SECTIONS {
    .text : {
        KEEP(*(.header))
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > rom

    .iwram : {
        __iwram_start = .;
        *(.iwram .iwram.*)
        *(.data .data.*)
        . = ALIGN(4);
        __iwram_end = .;
    } > iwram AT > rom
    __iwram_load = LOADADDR(.iwram);

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > iwram

    .ewram (NOLOAD) : {
        *(.ewram .ewram.*)
    } > ewram

    .rom_high 0x09F00000 : {
        KEEP(*(.rom_high .rom_high.*))
    }

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
