/* SPDX-License-Identifier: GPL-2.0-or-later */
.section .text.entry,"ax"
.balign 16
.global xboot_probe_entry
.type xboot_probe_entry,%function
xboot_probe_entry:
    /* Private AAPCS call, not an Apple kernel entry. Caller masks interrupts
     * and supplies mapped code/stack. No device registers are accessed. */
    sub sp, sp, #112
    stp x18, x19, [sp, #0]
    stp x20, x21, [sp, #16]
    stp x22, x23, [sp, #32]
    stp x24, x25, [sp, #48]
    stp x26, x27, [sp, #64]
    stp x28, x29, [sp, #80]
    str x30, [sp, #96]
    mov x19, sp
    adrp x9, xp_stack_top
    add x9, x9, :lo12:xp_stack_top
    mov sp, x9
    mrs x4, CurrentEL
    mrs x5, MPIDR_EL1
    bl xp_probe
    mov sp, x19
    ldp x18, x19, [sp, #0]
    ldp x20, x21, [sp, #16]
    ldp x22, x23, [sp, #32]
    ldp x24, x25, [sp, #48]
    ldp x26, x27, [sp, #64]
    ldp x28, x29, [sp, #80]
    ldr x30, [sp, #96]
    add sp, sp, #112
    ret
.size xboot_probe_entry,.-xboot_probe_entry

.section .bss.stack,"aw",%nobits
.balign 16
xp_stack:
    .skip 16384
xp_stack_top:
.section .note.GNU-stack,"",%progbits
