/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "el2_exception.h"
/* VHE (ARMv8.1) names: SCTLR/VBAR/ELR/SPSR_EL12 for the EL1 guest state. */
.arch armv8.1-a
.section .text.exception_session,"ax"
.balign 16
.global e2_exception_run
.type e2_exception_run,%function
e2_exception_run:
    /* Called after complete preflight, on our own stack, in masked EL2h. */
    sub sp, sp, #E2_CALL_FRAME_SIZE
    stp x18, x19, [sp, #0]
    stp x20, x21, [sp, #16]
    stp x22, x23, [sp, #32]
    stp x24, x25, [sp, #48]
    stp x26, x27, [sp, #64]
    stp x28, x29, [sp, #80]
    str x30, [sp, #96]
    adrp x9, e2_exception_session
    add x9, x9, :lo12:e2_exception_session
    mov x10, x9
    mov x11, #(E2_SESSION_SIZE / 16)
.Lclear_session:
    stp xzr, xzr, [x10], #16
    subs x11, x11, #1
    b.ne .Lclear_session
    adrp x10, e2_exception_trace
    add x10, x10, :lo12:e2_exception_trace
    mov x11, #(E2_EXCEPTION_TRACE_BYTES / 16)
.Lclear_trace:
    stp xzr, xzr, [x10], #16
    subs x11, x11, #1
    b.ne .Lclear_trace
    /* w0: run the EL1 guest stage after a passing BRK stage (guest entry only). */
    and x0, x0, #1
    str x0, [x9, #ES_GUEST_REQUESTED]
    mov x10, #E2_GUEST_NOT_RUN
    str x10, [x9, #ES_GUEST_STATUS]
    mov x10, sp
    str x10, [x9, #ES_CALL_SP]
    mrs x10, VBAR_EL2
    str x10, [x9, #ES_VBAR]
    mrs x10, DAIF
    str x10, [x9, #ES_DAIF]
    mrs x10, ELR_EL2
    str x10, [x9, #ES_ELR]
    mrs x10, SPSR_EL2
    str x10, [x9, #ES_SPSR]
    mrs x10, ESR_EL2
    str x10, [x9, #ES_ESR]
    mrs x10, FAR_EL2
    str x10, [x9, #ES_FAR]
    mrs x10, HCR_EL2
    str x10, [x9, #ES_HCR]
    mrs x10, SP_EL0
    str x10, [x9, #ES_SP_EL0]
    mrs x10, SP_EL1
    str x10, [x9, #ES_SP_EL1]
    adr x10, e2_expected_brk
    str x10, [x9, #ES_EXPECTED_PC]
    adr x10, e2_expected_resume
    str x10, [x9, #ES_RESUME_PC]
    adr x10, e2_exception_abort
    str x10, [x9, #ES_ABORT_PC]
    mov x10, #E2_NOT_TAKEN
    str x10, [x9, #ES_STATUS]
    mov x10, #1
    str x10, [x9, #ES_ACTIVE]
    adrp x10, e2_vectors
    add x10, x10, :lo12:e2_vectors
    str x10, [x9, #ES_HANDLER_VBAR]
.global e2_install_vectors
e2_install_vectors:
    msr daifset, #0xf
    msr VBAR_EL2, x10
    isb
    mov x10, #1
    str x10, [x9, #ES_INSTALLED]
    /* The handler must hand back SP exactly; restore re-derives SP from
     * ES_CALL_SP, so check it here or a frame-size error would be hidden. */
    mov x10, sp
    str x10, [x9, #ES_PRE_BRK_SP]
    /* A real future run checks all interrupted GPRs after ERET, rather than
     * accepting a trace record as proof that register restoration worked.
     * The original AAPCS callee state/LR already lives in our saved frame. */
    .irp reg,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
        mov x\reg, #(0x100 + \reg)
    .endr
.global e2_expected_brk
e2_expected_brk:
    brk #E2_BRK_IMM
.global e2_expected_resume
e2_expected_resume:
    .irp reg,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
        cmp x\reg, #(0x100 + \reg)
        b.ne .Lgpr_mismatch
    .endr
.global e2_expected_sp_check
e2_expected_sp_check:
    adrp x9, e2_exception_session
    add x9, x9, :lo12:e2_exception_session
    ldr x10, [x9, #ES_PRE_BRK_SP]
    mov x11, sp
    cmp x10, x11
    b.ne .Lgpr_mismatch
    /* The guest stage runs only on a fully passing BRK stage. */
    ldr x10, [x9, #ES_STATUS]
    cmp x10, #E2_EXPECTED
    b.ne .Lreturned_from_exception
    ldr x10, [x9, #ES_GUEST_REQUESTED]
    cbz x10, .Lreturned_from_exception
    b e2_guest_enter
.Lgpr_mismatch:
    adrp x9, e2_exception_session
    add x9, x9, :lo12:e2_exception_session
    mov x10, #E2_RESTORE_FAILED
    str x10, [x9, #ES_STATUS]
    b .Lreturned_from_exception

/* EL1 guest stage. Entered with x9 = session and SP = the run's call frame.
 * Order: save EL1 banked state, clean the guest code to PoC, set the EL1
 * registers, then write HCR_EL2 last; our vectors are installed throughout,
 * so a refused or trapped write is recorded instead of hanging. */
.global e2_guest_enter
e2_guest_enter:
    mov x13, #E2_GF_PREPARING
    str x13, [x9, #ES_GUEST_FLAGS]
    ldr x10, [x9, #ES_HCR]
    /* The _EL12 accessors below exist only with HCR_EL2.E2H set (VHE). */
    tbz x10, #34, .Lguest_refused
    /* The guest runs with stage 1 off, i.e. on physical addresses, while ELR
     * and VBAR_EL12 are computed from our EL2 addresses: require VA == PA for
     * the guest code page (one translation granule holds all of it). */
    mrs x10, PAR_EL1
    str x10, [x9, #ES_OLD_PAR]
    orr x13, x13, #E2_GF_PAR_SAVED
    str x13, [x9, #ES_GUEST_FLAGS]
    adrp x10, __e2_guest_text_start
    add x10, x10, :lo12:__e2_guest_text_start
    and x10, x10, #0xfffffffffffff000
    adr x15, __e2_guest_text_end
.Lidentity_page:
.global e2_guest_identity_check
e2_guest_identity_check:
    at s1e2r, x10
    isb
    mrs x11, PAR_EL1
    tbnz x11, #0, .Lguest_not_identity
    ubfx x11, x11, #12, #40
    lsr x12, x10, #12
    cmp x12, x11
    b.ne .Lguest_not_identity
    add x10, x10, #0x1000
    cmp x10, x15
    b.lo .Lidentity_page
    adr x10, e2_guest_after_probe
    str x10, [x9, #ES_GUEST_PROBE_PC]
    adr x10, e2_guest_after_done
    str x10, [x9, #ES_GUEST_DONE_PC]
    adr x10, e2_guest_after_signature
    str x10, [x9, #ES_GUEST_SIGNATURE_PC]
    adr x10, e2_guest_return
    str x10, [x9, #ES_GUEST_RETURN_PC]
    str xzr, [x9, #ES_HVC_COUNT]
    mrs x10, SCTLR_EL12
    str x10, [x9, #ES_OLD_SCTLR12]
    mrs x10, VBAR_EL12
    str x10, [x9, #ES_OLD_VBAR12]
    mrs x10, ELR_EL12
    str x10, [x9, #ES_OLD_ELR12]
    mrs x10, SPSR_EL12
    str x10, [x9, #ES_OLD_SPSR12]
    orr x13, x13, #E2_GF_EL12_SAVED
    str x13, [x9, #ES_GUEST_FLAGS]
    /* With stage 1 off the guest fetches non-cacheable: clean to PoC first. */
    mrs x10, CTR_EL0
    ubfx x10, x10, #16, #4
    mov x11, #4
    lsl x11, x11, x10
    adrp x12, __e2_guest_text_start
    add x12, x12, :lo12:__e2_guest_text_start
    adrp x14, __e2_guest_text_end
    add x14, x14, :lo12:__e2_guest_text_end
    sub x10, x11, #1
    bic x12, x12, x10
.Lclean_guest:
    dc cvac, x12
    add x12, x12, x11
    cmp x12, x14
    b.lo .Lclean_guest
    dsb sy
    ic iallu
    dsb sy
    isb
    mov x10, #(E2_GUEST_SCTLR & 0xffff)
    movk x10, #(E2_GUEST_SCTLR >> 16), lsl #16
    msr SCTLR_EL12, x10
    adrp x10, e2_guest_vectors
    add x10, x10, :lo12:e2_guest_vectors
    msr VBAR_EL12, x10
    adrp x10, __e2_guest_stack
    add x10, x10, :lo12:__e2_guest_stack
    add x10, x10, #E2_PAGE_SIZE
    msr SP_EL1, x10
    isb
    ldr x10, [x9, #ES_HCR]
    mov x11, #(E2_GUEST_HCR_CLEAR & 0xffff)
    movk x11, #(E2_GUEST_HCR_CLEAR >> 16), lsl #16
    bic x10, x10, x11
    orr x10, x10, #0x80000000      /* RW: AArch64 EL1 */
    orr x10, x10, #0x400000000     /* E2H stays set */
    str x10, [x9, #ES_GUEST_HCR]
.global e2_guest_hcr_write
e2_guest_hcr_write:
    msr HCR_EL2, x10
    isb
    /* Only now is HCR known to be writable: restore must rewrite it. A write
     * that trapped never gets here, so restore will not retry it. */
    orr x13, x13, #E2_GF_HCR_WRITTEN
    str x13, [x9, #ES_GUEST_FLAGS]
    mrs x11, HCR_EL2
    cmp x10, x11
    b.ne .Lguest_refused
    mov x10, #1
    str x10, [x9, #ES_GUEST_ACTIVE]
    orr x13, x13, #E2_GF_ENTERED
    str x13, [x9, #ES_GUEST_FLAGS]
    adr x10, e2_guest_payload
    msr ELR_EL2, x10
    mov x10, #E2_GUEST_SPSR
    msr SPSR_EL2, x10
.global e2_guest_eret
e2_guest_eret:
    eret
.Lguest_refused:
    mov x10, #E2_GUEST_HCR_REFUSED
    str x10, [x9, #ES_GUEST_STATUS]
    b .Lreturned_from_exception
.Lguest_not_identity:
    mov x10, #E2_GUEST_NOT_IDENTITY
    str x10, [x9, #ES_GUEST_STATUS]
    b .Lreturned_from_exception

/* ERET target after the guest's DONE HVC: EL2h with the host PSTATE, but
 * every GPR still holds guest values. */
.global e2_guest_return
e2_guest_return:
    adrp x9, e2_exception_session
    add x9, x9, :lo12:e2_exception_session
    ldr x10, [x9, #ES_CALL_SP]
    mov sp, x10
    b .Lreturned_from_exception
.global e2_exception_abort
e2_exception_abort:
    /* ERET recovery target; abandon any interrupted handler/C stack frames. */
.Lreturned_from_exception:
    adrp x9, e2_exception_session
    add x9, x9, :lo12:e2_exception_session
    ldr x10, [x9, #ES_ERET_PLANNED]
    str x10, [x9, #ES_RETURNED]
    b e2_exception_restore
.global e2_exception_direct_abort
e2_exception_direct_abort:
    /* No valid host SPSR: restore the explicit register set and return a
     * failure, with PSTATE restore bit clear. Never fabricate full success. */
    adrp x9, e2_exception_session
    add x9, x9, :lo12:e2_exception_session
    mov x10, #E2_CONTEXT_LOST
    str x10, [x9, #ES_STATUS]
    str xzr, [x9, #ES_RETURNED]

.global e2_exception_restore
e2_exception_restore:
    msr daifset, #0xf
    ldr x10, [x9, #ES_CALL_SP]
    mov sp, x10
    mov x12, #0
    /* Guest stage state first, while our vectors are still installed. A trap
     * here re-enters restore through the abort path; RESTORING makes that
     * second pass skip these writes and report failure instead of looping. */
    str xzr, [x9, #ES_GUEST_ACTIVE]
    ldr x13, [x9, #ES_GUEST_FLAGS]
    tbnz x13, #3, .Lguest_restore_done
    orr x14, x13, #E2_GF_RESTORING
    str x14, [x9, #ES_GUEST_FLAGS]
    tbz x13, #0, .Lguest_hcr_done
    ldr x10, [x9, #ES_HCR]
.global e2_restore_hcr_write
e2_restore_hcr_write:
    msr HCR_EL2, x10
    isb
.Lguest_hcr_done:
    orr x12, x12, #E2_R_GUEST
    tbz x13, #1, .Lguest_restore_done
.macro E2_RESTORE_GUEST reg, offset
    ldr x10, [x9, #\offset]
    msr \reg, x10
    isb
    mrs x11, \reg
    cmp x10, x11
    b.eq .Lguest_ok\@
    bic x12, x12, #E2_R_GUEST
.Lguest_ok\@:
.endm
    E2_RESTORE_GUEST SCTLR_EL12, ES_OLD_SCTLR12
    E2_RESTORE_GUEST VBAR_EL12, ES_OLD_VBAR12
    E2_RESTORE_GUEST ELR_EL12, ES_OLD_ELR12
    E2_RESTORE_GUEST SPSR_EL12, ES_OLD_SPSR12
    E2_RESTORE_GUEST SP_EL1, ES_SP_EL1
.Lguest_restore_done:
    /* AT updates PAR even when translation fails. Restore it before VBAR;
     * the second restore pass must not retry a trapping PAR write either. */
    tbnz x13, #3, .Lpar_done
    tbz x13, #5, .Lpar_done
    ldr x10, [x9, #ES_OLD_PAR]
.global e2_restore_par_write
e2_restore_par_write:
    msr PAR_EL1, x10
    isb
    mrs x11, PAR_EL1
    cmp x10, x11
    b.eq .Lpar_done
    bic x12, x12, #E2_R_GUEST
.Lpar_done:
    /* Restore inherited vectors before any C serialization. Use EL2 names
     * consistently under VHE, with explicit synchronization. */
    ldr x10, [x9, #ES_VBAR]
    msr VBAR_EL2, x10
    isb
    mrs x11, VBAR_EL2
    cmp x10, x11
    b.ne .Lvbar_done
    orr x12, x12, #E2_R_VBAR
.Lvbar_done:
.macro E2_RESTORE reg, offset, mask
    ldr x10, [x9, #\offset]
    msr \reg, x10
    isb
    mrs x11, \reg
    cmp x10, x11
    b.ne .Lrestore_done\@
    orr x12, x12, #\mask
.Lrestore_done\@:
.endm
    E2_RESTORE ELR_EL2, ES_ELR, E2_R_ELR
    E2_RESTORE SPSR_EL2, ES_SPSR, E2_R_SPSR
    E2_RESTORE ESR_EL2, ES_ESR, E2_R_ESR
    E2_RESTORE FAR_EL2, ES_FAR, E2_R_FAR
    mrs x10, HCR_EL2
    ldr x11, [x9, #ES_HCR]
    cmp x10, x11
    b.ne .Lstate_done
    mrs x10, SP_EL0
    ldr x11, [x9, #ES_SP_EL0]
    cmp x10, x11
    b.ne .Lstate_done
    mrs x10, SP_EL1
    ldr x11, [x9, #ES_SP_EL1]
    cmp x10, x11
    b.ne .Lstate_done
    mrs x10, SPSel
    cmp x10, #1
    b.ne .Lstate_done
    orr x12, x12, #E2_R_UNCHANGED
.Lstate_done:
    adrp x10, __e2_stack_start
    add x10, x10, :lo12:__e2_stack_start
    cmp sp, x10
    b.lo .Lstack_done
    adrp x10, __e2_stack_end
    add x10, x10, :lo12:__e2_stack_end
    add x11, sp, #E2_CALL_FRAME_SIZE
    cmp x11, x10
    b.hi .Lstack_done
    orr x12, x12, #E2_R_STACK
.Lstack_done:
    ldr x10, [x9, #ES_RETURNED]
    cbz x10, .Lpstate_done
    orr x12, x12, #E2_R_PSTATE
.Lpstate_done:
    str xzr, [x9, #ES_ACTIVE]
    str xzr, [x9, #ES_DEPTH]
    /* Incoming DAIF was fully masked; restore it last nevertheless. */
    E2_RESTORE DAIF, ES_DAIF, E2_R_DAIF
    str x12, [x9, #ES_RESTORE_MASK]
    and x10, x12, #0x7f
    cmp x10, #0x7f
    b.ne .Lrestore_failed
    ldr x0, [x9, #ES_STATUS]
    cmp x0, #E2_EXPECTED
    b.ne .Lsession_return
    cmp x12, #E2_R_ALL
    b.eq .Lsession_return
.Lrestore_failed:
    mov x0, #E2_RESTORE_FAILED
    str x0, [x9, #ES_STATUS]
.Lsession_return:
    ldp x18, x19, [sp, #0]
    ldp x20, x21, [sp, #16]
    ldp x22, x23, [sp, #32]
    ldp x24, x25, [sp, #48]
    ldp x26, x27, [sp, #64]
    ldp x28, x29, [sp, #80]
    ldr x30, [sp, #96]
    add sp, sp, #E2_CALL_FRAME_SIZE
    ret
.size e2_exception_run,.-e2_exception_run

.section .bss.e2_state,"aw",%nobits
.balign 16
.global e2_exception_session
e2_exception_session:
    .skip E2_SESSION_SIZE
.section .bss.e2_exception_trace,"aw",%nobits
.balign 16
.global e2_exception_trace
e2_exception_trace:
    .skip E2_EXCEPTION_TRACE_BYTES
.section .note.GNU-stack,"",%progbits
