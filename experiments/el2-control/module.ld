/* SPDX-License-Identifier: GPL-2.0-or-later */
ENTRY(sixboot_el2_entry)
PHDRS { code PT_LOAD FLAGS(5); private PT_LOAD FLAGS(6); }
SECTIONS {
    /* Relative layout only. A future loader must allocate/map the whole arena. */
    . = 0;
    __e2_image_start = .;
    .text : { KEEP(*(.text.entry)) *(.text .text.*) } :code
    .rodata : { *(.rodata .rodata.*) } :code
    .vectors ALIGN(0x800) : { KEEP(*(.vectors)) } :code
    ASSERT(SIZEOF(.vectors) == 0x800, "EL2 vector table must be 16 x 128 bytes")
    ASSERT((ADDR(.vectors) & 0x7ff) == 0, "EL2 vector table alignment")
    /* EL1 guest vectors (2 KiB aligned for VBAR_EL12) followed by its payload. */
    .guest_text ALIGN(0x800) : {
        __e2_guest_text_start = .; KEEP(*(.guest_text)) __e2_guest_text_end = .;
    } :code
    ASSERT((ADDR(.guest_text) & 0x7ff) == 0, "EL1 guest vector alignment")
    __e2_code_end = .;
    ASSERT(__e2_code_end <= 0x4000, "EL2 code exceeds one page")
    .state 0x04000 (NOLOAD) : { *(.bss.e2_state) . = 0x4000; } :private
    .stack 0x08000 (NOLOAD) : { *(.bss.e2_stack) } :private
    ASSERT(SIZEOF(.stack) == 0x4000, "EL2 stack size changed")
    .guest 0x0c000 (NOLOAD) : { __e2_guest = .; . += 0x4000; } :private
    .guest_stack 0x10000 (NOLOAD) : { __e2_guest_stack = .; . += 0x4000; } :private
    .trace 0x14000 (NOLOAD) : {
        __e2_trace = .; KEEP(*(.bss.e2_exception_trace)) . = 0x4000;
    } :private
    .reserved 0x18000 (NOLOAD) : { . += 0x8000; } :private
    __e2_image_end = .;
    ASSERT(__e2_image_end == 0x20000, "EL2 arena size changed")
    .unexpected : { *(.data .data.*) *(.bss .bss.*) *(COMMON) } :private
    ASSERT(SIZEOF(.unexpected) == 0, "unplanned mutable state")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
