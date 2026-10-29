/* SPDX-License-Identifier: GPL-2.0-or-later */
.arch armv8.1-a
.section .text.start,"ax"
.global _start
_start:
    msr daifset, #0xf
    msr SPSel, #1
    ldr x0, =__test_stack_top
    mov sp, x0
    ldr x0, =test_vectors
    msr VBAR_EL2, x0
    isb
    ldr x0, =E2_TEST_HCR
    msr HCR_EL2, x0
    isb
    /* VHE host stage 1: 16 KiB granule, four 32 MiB RAM blocks.
     * No devices are mapped. Match the module's normal-RAM precondition. */
    ldr x1, =test_l2
    ldr x0, =0x40000701
    add x2, x1, #256
    mov x3, #4
    mov x4, #0x2000000
1:  str x0, [x2], #8
    add x0, x0, x4
    subs x3, x3, #1
    b.ne 1b
#ifdef E2_TEST_NATIVE
    /* The native page protector accepts existing L3 pages only. Give it
     * m1n1's 48-bit/four-level geometry, with RAM's first block split. */
    ldr x2, =test_l3
    orr x3, x2, #3
    str x3, [x1, #256]
    ldr x0, =0x40000703
    mov x3, #2048
    mov x4, #0x4000
2:  str x0, [x2], #8
    add x0, x0, x4
    subs x3, x3, #1
    b.ne 2b
    ldr x2, =test_l1
    orr x3, x1, #3
    str x3, [x2]
    ldr x1, =test_l0
    orr x3, x2, #3
    str x3, [x1]
#endif
    mov x0, #0xff
    msr MAIR_EL2, x0
#ifdef E2_TEST_NATIVE
    ldr x0, =0x2009cb510
#else
    ldr x0, =0x2009cb51c
#endif
    msr TCR_EL2, x0
    msr TTBR0_EL2, x1
    dsb sy
    tlbi alle2
    dsb sy
    isb
    mrs x0, SCTLR_EL2
    bic x0, x0, #2
    orr x0, x0, #1
    orr x0, x0, #4
    orr x0, x0, #0x1000
    msr SCTLR_EL2, x0
    isb
    /* Distinct scratch state makes missing restoration observable. */
    ldr x0, =0x30d00800
    msr SCTLR_EL12, x0
    ldr x0, =test_vectors
    msr VBAR_EL12, x0
    ldr x0, =0x40081234
    msr ELR_EL12, x0
    mov x0, #0x3c5
    msr SPSR_EL12, x0
    ldr x0, =0x41000000
    msr SP_EL1, x0
    ldr x0, =0x41100000
    msr SP_EL0, x0
    ldr x0, =0xabcd0000
    msr PAR_EL1, x0
    mov x0, #0x3c9
    msr SPSR_EL2, x0
    ldr x0, =0x40085678
    msr ELR_EL2, x0
    ldr x0, =0x58006e99
    msr ESR_EL2, x0
    ldr x0, =0x41234560
    msr FAR_EL2, x0
    isb
    bl test_main
    mov x0, #99
    b test_exit

.global test_call
.type test_call,%function
test_call:
    sub sp, sp, #112
    stp x18, x19, [sp]
    stp x20, x21, [sp, #16]
    stp x22, x23, [sp, #32]
    stp x24, x25, [sp, #48]
    stp x26, x27, [sp, #64]
    stp x28, x29, [sp, #80]
    str x30, [sp, #96]
#ifdef E2_TEST_NATIVE
    ldr x5, =native_test_stack_top
#else
    mov x16, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
#endif
    adr x17, test_observed
    mov x4, sp
    str x4, [x17, #104]
    .irp n,18,19,20,21,22,23,24,25,26,27,28,29
        mov x\n, #(0x700 + \n)
    .endr
#ifdef E2_TEST_NATIVE
    bl el2_native_invoke
#else
    blr x16
#endif
    /* This executes only after the module's final RET reached its caller. */
    adr x17, test_observed
    stp x18, x19, [x17]
    stp x20, x21, [x17, #16]
    stp x22, x23, [x17, #32]
    stp x24, x25, [x17, #48]
    stp x26, x27, [x17, #64]
    stp x28, x29, [x17, #80]
    mov x4, sp
    str x4, [x17, #96]
    str x0, [x17, #112]
    ldr x4, [x17, #104]
    mov sp, x4
    ldp x18, x19, [sp]
    ldp x20, x21, [sp, #16]
    ldp x22, x23, [sp, #32]
    ldp x24, x25, [sp, #48]
    ldp x26, x27, [sp, #64]
    ldp x28, x29, [sp, #80]
    ldr x30, [sp, #96]
    add sp, sp, #112
    ret
.size test_call,.-test_call

.balign 0x800
test_vectors:
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        mov x0, #\n
        b test_exception
        .space 120
    .endr
test_exception:
    mov x0, #4
    adr x1, .Lfault_text
    hlt #0xf000
    mov x0, #0x20
    adr x1, .Lfault_exit
    hlt #0xf000
    b .
.Lfault_text: .asciz "HARNESS_UNHANDLED_EXCEPTION\n"
.balign 8
.Lfault_exit: .quad 0x20026, 98

.global test_exit
test_exit:
    /* SYS_EXIT_EXTENDED; no host file or disk access. */
    sub sp, sp, #16
    mov x1, #0x26
    movk x1, #2, lsl #16
    stp x1, x0, [sp]
    mov x1, sp
    mov x0, #0x20
    hlt #0xf000
    b .
.section .bss,"aw",%nobits
.balign 16
.global test_observed
test_observed:
    .skip 128
.balign 16384
test_l2:
    .skip 16384
#ifdef E2_TEST_NATIVE
.balign 16384
test_l0: .skip 16384
test_l1: .skip 16384
test_l3: .skip 16384
.balign 16384
    .skip 16384
.global native_test_stack_top
native_test_stack_top:
#endif
.section .note.GNU-stack,"",%progbits
