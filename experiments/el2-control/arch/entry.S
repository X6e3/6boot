/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "el2_exception.h"
.global e2_result_version
.equ e2_result_version, E2_EXCEPTION_VERSION
.section .text.entry,"ax"
.balign 16
.global sixboot_el2_entry
.type sixboot_el2_entry,%function
sixboot_el2_entry:
    mov w17, #0
    b .Lentry_common
.global sixboot_el2_exception_entry
.type sixboot_el2_exception_entry,%function
sixboot_el2_exception_entry:
    mov w17, #1
.Lentry_common:
    /* Both callable entries refuse incompatible state before changing SP.
     * Only the separate exception entry installs vectors after preflight. */
    mrs x9, CurrentEL
    cmp x9, #8
    b.ne .Lrefuse
    mrs x10, DAIF
    and x12, x10, #E2_DAIF_MASK
    cmp x12, #E2_DAIF_MASK
    b.ne .Lrefuse
    mrs x11, SPSel
    cmp x11, #1
    b.ne .Lrefuse

    /* Check before the first stack write: metadata or output may otherwise
     * alias saved x18..x30 (including the return address). Preserve x9..x11
     * for the snapshot and w17 for the entry selector. */
.global e2_validate_caller_frame
e2_validate_caller_frame:
    mov x12, sp
    tst x12, #15
    b.ne .Lrefuse
    cmp x12, #E2_CALL_FRAME_SIZE
    b.lo .Lrefuse
    sub x13, x12, #E2_CALL_FRAME_SIZE
    adds x14, x0, x1
    b.cs .Lbad_arguments
    cmp x0, x12
    b.hs .Lrequest_clear
    cmp x14, x13
    b.hi .Lbad_arguments
.Lrequest_clear:
    mov x15, #E2_RESULT_SIZE
    cbz w17, .Lresult_size_ready
    mov x15, #E2_EXCEPTION_RESULT_SIZE
.Lresult_size_ready:
    adds x14, x2, x15
    b.cs .Lbad_arguments
    cmp x2, x12
    b.hs .Lbuffers_clear
    cmp x14, x13
    b.hi .Lbad_arguments
.Lbuffers_clear:
.global e2_save_caller_frame
e2_save_caller_frame:
    sub sp, sp, #E2_CALL_FRAME_SIZE
    stp x18, x19, [sp, #0]
    stp x20, x21, [sp, #16]
    stp x22, x23, [sp, #32]
    stp x24, x25, [sp, #48]
    stp x26, x27, [sp, #64]
    stp x28, x29, [sp, #80]
    str x30, [sp, #96]
    mov x19, sp
    mov w20, w17
    mov x21, x2
    mov x22, x3

    adrp x4, e2_entry_snapshot
    add x4, x4, :lo12:e2_entry_snapshot
    str x9, [x4, #E2_SNAP_EL]
    str x11, [x4, #E2_SNAP_SPSEL]
    add x12, sp, #E2_CALL_FRAME_SIZE
    str x12, [x4, #E2_SNAP_ACTIVE_SP]
    /* SP_EL2 is not MRS-readable at EL2. The active SP above is SP_EL2
     * because SPSel==1 was checked before any stack change. */
    mrs x12, SP_EL0
    str x12, [x4, #E2_SNAP_SP_EL0]
    mrs x12, VBAR_EL2
    str x12, [x4, #E2_SNAP_VBAR]
    mrs x12, HCR_EL2
    str x12, [x4, #E2_SNAP_HCR]
    str x10, [x4, #E2_SNAP_DAIF]
    mrs x12, MPIDR_EL1
    str x12, [x4, #E2_SNAP_MPIDR]
    mrs x12, ID_AA64MMFR0_EL1
    str x12, [x4, #E2_SNAP_MMFR0]
    mrs x12, ID_AA64MMFR1_EL1
    str x12, [x4, #E2_SNAP_MMFR1]
    mrs x12, ID_AA64MMFR2_EL1
    str x12, [x4, #E2_SNAP_MMFR2]
    mrs x12, ID_AA64PFR0_EL1
    str x12, [x4, #E2_SNAP_PFR0]
    adrp x5, __e2_image_start
    add x5, x5, :lo12:__e2_image_start
    adrp x9, __e2_stack_end
    add x9, x9, :lo12:__e2_stack_end
    mov sp, x9
    cbnz w20, .Lexception_prepare
    bl e2_foundation_run
    b .Lentry_return
.Lexception_prepare:
    bl e2_exception_prepare
    cbnz w0, .Lentry_return
    /* w0 = 1 only for the guest entry: BRK stage, then the EL1/HVC stage. */
    cmp w20, #2
    cset w0, eq
    bl e2_exception_run
    mov x0, x21
    mov x1, x22
    adrp x2, e2_exception_session
    add x2, x2, :lo12:e2_exception_session
    adrp x3, e2_exception_trace
    add x3, x3, :lo12:e2_exception_trace
    bl e2_exception_encode

.Lentry_return:
    mov sp, x19
    ldp x18, x19, [sp, #0]
    ldp x20, x21, [sp, #16]
    ldp x22, x23, [sp, #32]
    ldp x24, x25, [sp, #48]
    ldp x26, x27, [sp, #64]
    ldp x28, x29, [sp, #80]
    ldr x30, [sp, #96]
    add sp, sp, #E2_CALL_FRAME_SIZE
    ret
.Lbad_arguments:
.global e2_entry_argument_refusal
e2_entry_argument_refusal:
    mov w0, #E2_ARGUMENT
    ret
.Lrefuse:
.global e2_entry_environment_refusal
e2_entry_environment_refusal:
    mov w0, #E2_ENVIRONMENT
    ret
.size sixboot_el2_entry,.-sixboot_el2_entry
.size sixboot_el2_exception_entry,.-sixboot_el2_exception_entry
/* Explicit, separate entry; nothing in the default boot path calls it. Same
 * preflight and caller-frame checks as the exception entry. */
.global sixboot_el2_guest_entry
.type sixboot_el2_guest_entry,%function
sixboot_el2_guest_entry:
    mov w17, #2
    b .Lentry_common
.size sixboot_el2_guest_entry,.-sixboot_el2_guest_entry

.section .bss.e2_state,"aw",%nobits
.balign 16
e2_entry_snapshot:
    .skip E2_SNAPSHOT_SIZE
.section .bss.e2_stack,"aw",%nobits
.balign 16
.global __e2_stack_start
__e2_stack_start:
    .skip E2_PAGE_SIZE
.global __e2_stack_end
__e2_stack_end:
.section .note.GNU-stack,"",%progbits
