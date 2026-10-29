/* SPDX-License-Identifier: GPL-2.0-or-later */
ENTRY(_start)
PHDRS { code PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); module PT_LOAD FLAGS(5); }
SECTIONS {
    . = 0x40400000;
    .text : { KEEP(*(.text.start)) *(.text .text.*) } :code
    .rodata : { *(.rodata .rodata.*) } :code
    .data : { *(.data .data.*) } :data
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) } :data
    ASSERT(. < 0x40500000, "test harness overlaps its stack")
    .stack 0x40500000 (NOLOAD) : { . += 0x8000; __test_stack_top = .; } :data
    .module 0x40800000 : { KEEP(*(.module_image)) . = 0x20000; } :module
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
