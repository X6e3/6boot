/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "el2_exception.h"
.section .vectors,"ax"
.balign E2_VECTOR_ALIGN
.global e2_vectors
.type e2_vectors,%function
e2_vectors:
.macro E2_VECTOR number
.global e2_vector_\number
e2_vector_\number:
    /* Hardware selects SP_EL2 on entry. Its first 16-byte landing must be
     * accessible (controlled-entry precondition); bound the complete frame
     * plus the C leaf's audited stack budget before storing the rest. */
    stp x0, x1, [sp, #-16]!
    adrp x0, __e2_stack_start
    add x0, x0, :lo12:__e2_stack_start
    add x0, x0, #(E2_FRAME_SIZE - 16 + E2_HANDLER_C_BUDGET)
    mov x1, sp
    cmp x1, x0
    b.lo .Lbad_\number
    adrp x0, __e2_stack_end
    add x0, x0, :lo12:__e2_stack_end
    add x1, sp, #16
    cmp x1, x0
    b.hi .Lbad_\number
    sub sp, sp, #(E2_FRAME_SIZE - 16)
    ldp x0, x1, [sp, #(E2_FRAME_SIZE - 16)]
    stp x0, x1, [sp]
    mov w0, #\number
    b e2_exception_common
.Lbad_\number:
    mov w0, #\number
    b e2_exception_bad_stack
    .space E2_VECTOR_STRIDE - (. - e2_vector_\number)
.endm
/* Four groups: current EL/SP0, current EL/SPx, lower A64, lower A32.
 * In each group: synchronous, IRQ, FIQ, SError. */
E2_VECTOR 0
E2_VECTOR 1
E2_VECTOR 2
E2_VECTOR 3
E2_VECTOR 4
E2_VECTOR 5
E2_VECTOR 6
E2_VECTOR 7
E2_VECTOR 8
E2_VECTOR 9
E2_VECTOR 10
E2_VECTOR 11
E2_VECTOR 12
E2_VECTOR 13
E2_VECTOR 14
E2_VECTOR 15
.global e2_vectors_end
e2_vectors_end:
.size e2_vectors,.-e2_vectors

.section .text.exception_handler,"ax"
.balign 16
.global e2_exception_common
.type e2_exception_common,%function
e2_exception_common:
    /* x0/x1 were saved before the vector slot used either as scratch. */
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    str x0, [sp, #EF_VECTOR]
    add x9, sp, #E2_FRAME_SIZE
    str x9, [sp, #EF_SP]
    adrp x1, e2_exception_session
    add x1, x1, :lo12:e2_exception_session
    ldr x9, [x1, #ES_DEPTH]
    add x9, x9, #1
    str x9, [x1, #ES_DEPTH]
    str x9, [sp, #EF_DEPTH]
    /* Capture the host PSTATE early, before any other optional diagnostics. */
    mrs x10, SPSR_EL2
    str x10, [sp, #EF_SPSR]
.global e2_host_capture_once
e2_host_capture_once:
    ldr x11, [x1, #ES_HOST_VALID]
    cbnz x11, .Lhave_host
    cmp x9, #1
    b.ne .Lhave_host
    and x11, x10, #0x1f
    cmp x11, #9
    b.ne .Lhave_host
    tbnz x10, #20, .Lhave_host
    and x11, x10, #E2_DAIF_MASK
    cmp x11, #E2_DAIF_MASK
    b.ne .Lhave_host
    str x10, [x1, #ES_HOST_SPSR]
    mov x11, #1
    str x11, [x1, #ES_HOST_VALID]
.Lhave_host:
.global e2_host_capture_done
e2_host_capture_done:
    mrs x9, ESR_EL2
    str x9, [sp, #EF_ESR]
    mrs x9, ELR_EL2
    str x9, [sp, #EF_ELR]
    mrs x9, FAR_EL2
    str x9, [sp, #EF_FAR]
    mrs x9, DAIF
    str x9, [sp, #EF_DAIF]
    mrs x9, HCR_EL2
    str x9, [sp, #EF_HCR]
    mrs x9, VBAR_EL2
    str x9, [sp, #EF_VBAR]
    mrs x9, MPIDR_EL1
    str x9, [sp, #EF_MPIDR]
    mrs x9, SP_EL0
    str x9, [sp, #EF_SP_EL0]
    mrs x9, SP_EL1
    str x9, [sp, #EF_SP_EL1]
    mov x0, sp
    adrp x2, e2_exception_trace
    add x2, x2, :lo12:e2_exception_trace
    bl e2_exception_dispatch
    cbnz w0, e2_exception_direct_abort
    adrp x9, e2_exception_session
    add x9, x9, :lo12:e2_exception_session
    ldr x10, [x9, #ES_DEPTH]
    sub x10, x10, #1
    str x10, [x9, #ES_DEPTH]
    ldr x9, [sp, #EF_RESUME_ELR]
    ldr x10, [sp, #EF_RESUME_SPSR]
    msr ELR_EL2, x9
    msr SPSR_EL2, x10
    isb
    /* Restore every interrupted GPR, including platform register x18. */
    ldp x0, x1, [sp]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #E2_FRAME_SIZE
    eret
.size e2_exception_common,.-e2_exception_common

.global e2_exception_bad_stack
.type e2_exception_bad_stack,%function
e2_exception_bad_stack:
    /* No C call or further access to the rejected stack. A complete GPR
     * frame is not claimed. Abort, never resume the faulting instruction. */
    adrp x9, e2_exception_session
    add x9, x9, :lo12:e2_exception_session
    str x0, [x9, #ES_LAST_VECTOR]
    mov x10, #E2_BAD_STACK
    ldr x11, [x9, #ES_GUEST_FLAGS]
    tbz x11, #4, .Lbad_host_stage
    str x10, [x9, #ES_GUEST_STATUS]
    str xzr, [x9, #ES_GUEST_ACTIVE]
    b .Lbad_select_context
.Lbad_host_stage:
    str x10, [x9, #ES_STATUS]
.Lbad_select_context:
    /* A nested landing failure sees the handler's SPSR, not the original
     * interrupted host. Prefer the retained host context; if nesting began
     * before it was captured, do not claim a restored original PSTATE. */
.global e2_bad_stack_select_host
e2_bad_stack_select_host:
    ldr x10, [x9, #ES_HOST_VALID]
    cmp x10, #1
    b.eq .Lbad_saved_host
    cbnz x10, e2_exception_direct_abort
    ldr x10, [x9, #ES_DEPTH]
    cbnz x10, e2_exception_direct_abort
    mrs x10, SPSR_EL2
    b .Lbad_validate_host
.Lbad_saved_host:
.global e2_bad_stack_saved_host
e2_bad_stack_saved_host:
    ldr x10, [x9, #ES_HOST_SPSR]
.Lbad_validate_host:
    and x11, x10, #0x1f
    cmp x11, #9
    b.ne e2_exception_direct_abort
    tbnz x10, #20, e2_exception_direct_abort
    and x11, x10, #E2_DAIF_MASK
    cmp x11, #E2_DAIF_MASK
    b.ne e2_exception_direct_abort
    str x10, [x9, #ES_HOST_SPSR]
    mov x11, #1
    str x11, [x9, #ES_HOST_VALID]
    /* A valid current-EL host PSTATE permits a bounded ERET to cleanup. */
    ldr x11, [x9, #ES_CALL_SP]
    mov sp, x11
    adr x11, e2_exception_abort
    msr ELR_EL2, x11
    msr SPSR_EL2, x10
    mov x11, #1
    str x11, [x9, #ES_ERET_PLANNED]
    isb
    eret
.size e2_exception_bad_stack,.-e2_exception_bad_stack
.section .note.GNU-stack,"",%progbits
