/* SPDX-License-Identifier: GPL-2.0-or-later */
ENTRY(_start)
PHDRS { code PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
    . = 0x40400000;
    .text : { KEEP(*(.text.start)) *(.text .text.*) } :code
    .rodata : { *(.rodata .rodata.*) } :code
    .data : { *(.data .data.*) } :data
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) } :data
    ASSERT(. < 0x40700000, "native smoke fixture overlaps its stack")
    .stack 0x40700000 (NOLOAD) : { . += 0x10000; __test_stack_top = .; } :data
    ASSERT(. < 0x40800000, "native smoke fixture overlaps unused E0 arena")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
