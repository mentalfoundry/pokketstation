/* Linker script for pk_timing_bench: places every section at the exact
 * body-relative offset this app's own runtime code depends on (real BIOS
 * icon rendering reads a fixed offset, WRAM-mirror copies use fixed
 * source offsets, etc.) - see README.md and docs/app-notes.md for why
 * each of these offsets is load-bearing, not arbitrary. */

ENTRY(_start)

SECTIONS
{
    . = 0x02000000;   /* FLASH1_BASE - Title Sector body starts here */

    .header : { KEEP(*(.header)) }

    . = 0x02000100;
    .icon : { KEEP(*(.icon)) }

    /* No reserved MCX1 snapshot region - see header.s for why (an MCX1
     * conversion was tried and reverted; it broke the real BIOS's own
     * browse-icon rendering for this app, and turned out to be
     * unnecessary anyway). */
    . = 0x02000300;
    .arm_mirror : { KEEP(*(.arm_mirror)) }

    . = 0x0200031C;
    .thumb_mirror : { KEEP(*(.thumb_mirror)) }

    . = 0x02000338;
    .font_table : { KEEP(*(.font_table)) }

    . = 0x02000388;
    .thumb_loop : { KEEP(*(.thumb_loop)) }

    .text : ALIGN(4) { *(.text) }

    ASSERT(. <= 0x02002000, "pk_timing_bench body overflowed 8KB (BODY_SIZE)")

    /DISCARD/ : { *(.ARM.attributes) *(.ARM.exidx*) *(.comment) *(.note.*) }
}
