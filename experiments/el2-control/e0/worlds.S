/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "worlds.h"
.arch armv8.1-a
.section .guest_text,"ax"
.balign 16
.global ew_payload
ew_payload:
    ldr x1, =E0_ALLOWED_IPA
    str x0, [x1]
    add x1, x0, #1
    hvc #EW_HVC_YIELD
.global ew_after_yield
ew_after_yield:
    cmp x0, #EW_PATTERN_A
    b.eq .Lcheck_a
    cmp x0, #EW_PATTERN_B
    b.ne .Lfail
    b .Lcheck_b
.macro CHECK pattern
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
        cmp x\n, #(\pattern + \n)
        b.ne .Lfail
    .endr
    b .Lchecked
.endm
.Lcheck_a:
    CHECK EW_PATTERN_A
.Lcheck_b:
    CHECK EW_PATTERN_B
.Lchecked:
    /* All GPRs were checked before using any as scratch. Test private RAM
     * and a real stack round trip after a different world's execution. */
    stp x19, x20, [sp, #-16]!
    ldp x2, x3, [sp], #16
    cmp x2, x19
    b.ne .Lfail
    cmp x3, x20
    b.ne .Lfail
    ldr x1, =E0_ALLOWED_IPA
    ldr x2, [x1]
    cmp x2, x0
    b.ne .Lfail
    ldr x1, =E0_DENIED_IPA
    mov x3, #E0_STORE_VALUE
.global ew_permission_pc
ew_permission_pc:
    str x3, [x1]
.global ew_after_permission
ew_after_permission:
    ldr x2, [x1]
    mov x4, #E0_GUARD_VALUE
    cmp x2, x4
    b.ne .Lfail
    ldr x1, [x1, #8]
.global ew_foreign_pc
ew_foreign_pc:
    str x3, [x1]
.global ew_after_foreign
ew_after_foreign:
    mov x0, #0
    hvc #EW_HVC_DONE
.global ew_after_done
ew_after_done:
    .inst 0
.Lfail:
    hvc #EW_HVC_FAIL
    .inst 0
.section .note.GNU-stack,"",%progbits
