ENTRY(xboot_probe_entry)
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
    /* Relative virtual layout. A loader must map both segments and zero BSS.
     * Zero here is not a J713 physical load-address recommendation. */
    . = 0;
    .text : { *(.text.entry) *(.text .text.*) } :text
    .rodata : { *(.rodata .rodata.*) } :text
    . = ALIGN(0x4000);
    .data : { *(.data .data.*) } :data
    .bss (NOLOAD) : { *(.bss.stack) *(.bss .bss.*) *(COMMON) } :data
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
