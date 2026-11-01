/* SPDX-License-Identifier: GPL-2.0-or-later */
ENTRY(_start)
PHDRS { hcode PT_LOAD FLAGS(5); hdata PT_LOAD FLAGS(6); code PT_LOAD FLAGS(5); guest PT_LOAD FLAGS(5); private PT_LOAD FLAGS(6); }
SECTIONS {
    . = 0x40400000;
    .harness : {
        *start.o(.text* .rodata*) *harness.o(.text* .rodata*) *visual.o(.text* .rodata*)
        /* Optional emulator display support stays outside both guest domains. */
        *fb.o(.text* .rodata*) *fb_memory.o(.text* .rodata*)
        *font.o(.rodata*) *visual_check.o(.text* .rodata*)
    } :hcode
    .harness_data : { *start.o(.data* .bss*) *harness.o(.data* .bss*) *visual.o(.data* .bss*) } :hdata
    .harness_stack ALIGN(0x4000) (NOLOAD) : { . += 0x8000; __test_stack_top = .; } :hdata
    ASSERT(. < 0x40800000, "harness overlaps E0 arena")
    . = 0x40800000;
    e0_arena_start = .;
    .text : { *(.text*) *(.rodata*) } :code
    .vectors ALIGN(0x800) : { KEEP(*(.vectors)) } :code
    ASSERT(SIZEOF(.vectors) == 0x800, "vector size")
    ASSERT(. <= 0x40804000, "EL2 text overflow")
    .guest_text 0x40804000 : { KEEP(*(.guest_text)) } :guest
    ASSERT(SIZEOF(.guest_text) <= 0x4000, "guest code overflow")
    .state 0x40808000 (NOLOAD) : { *(.bss* .data*) *(COMMON) } :private
    ASSERT(SIZEOF(.state) <= 0x4000, "private state overflow")
    .stack 0x4080c000 (NOLOAD) : { __e2_stack_start = .; . += 0x4000; __e2_stack_end = .; } :private
    .tables 0x40810000 (NOLOAD) : { e0_tables = .; . += 0x8000; } :private
    .guest_stack 0x40818000 (NOLOAD) : { . += 0x4000; } :private
    .allowed 0x4081c000 (NOLOAD) : { . += 0x4000; } :private
    .guard 0x40820000 (NOLOAD) : { . += 0x4000; } :private
    .reserved 0x40824000 (NOLOAD) : {
        ew_tables_b = .; . += 0x8000;
        /* B's stack, data, read-only data, code copy and final guard. */
        . += 0x14000;
    } :private
    /DISCARD/ : { *(.comment .note* .eh_frame*) }
}
