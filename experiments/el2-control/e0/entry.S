/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "e0.h"
#ifdef E0_WORLDS
#include "worlds.h"
#endif
.arch armv8.1-a
.section .text,"ax"
.balign 16
.global sixboot_e0_run
sixboot_e0_run:
    mrs x17, NZCV
    mrs x9, CurrentEL
    cmp x9, #8
    b.ne .Lrefuse
    mrs x9, SPSel
    cmp x9, #1
    b.ne .Lrefuse
    mrs x9, DAIF
    cmp x9, #E2_DAIF_MASK
    b.ne .Lrefuse
    mrs x9, HCR_EL2
    tbz x9, #34, .Lrefuse
    tbz x9, #27, .Lrefuse /* inherited VHE host regime */
    tbz x9, #31, .Lrefuse /* AArch64 lower EL */
    tbnz x9, #42, .Lrefuse /* NV/NV1/NV2 are outside this experiment. */
    tbnz x9, #43, .Lrefuse
    tbnz x9, #45, .Lrefuse
    tbnz x9, #0, .Lrefuse
    tbnz x9, #12, .Lrefuse
    tbnz x9, #46, .Lrefuse /* FWB memory encoding is outside E0. */
    mov x9, sp
    tst x9, #15
    b.ne .Lrefuse
    sub sp, sp, #E2_CALL_FRAME_SIZE
    stp x18, x19, [sp]
    stp x20, x21, [sp, #16]
    stp x22, x23, [sp, #32]
    stp x24, x25, [sp, #48]
    stp x26, x27, [sp, #64]
    stp x28, x29, [sp, #80]
    str x30, [sp, #96]
    adrp x9, e0_saved
    add x9, x9, :lo12:e0_saved
    mov x10, sp
    str x10, [x9, #EO_CALL_SP]
    str x17, [x9, #EO_NZCV]
.macro SAVE reg, off
    mrs x10, \reg
    str x10, [x9, #\off]
.endm
    SAVE VBAR_EL2, EO_VBAR
    SAVE HCR_EL2, EO_HCR
    SAVE VTCR_EL2, EO_VTCR
    SAVE VTTBR_EL2, EO_VTTBR
    SAVE HPFAR_EL2, EO_HPFAR
    SAVE ELR_EL2, EO_ELR
    SAVE SPSR_EL2, EO_SPSR
    SAVE ESR_EL2, EO_ESR
    SAVE FAR_EL2, EO_FAR
    SAVE DAIF, EO_DAIF
    SAVE SP_EL1, EO_SP1
    SAVE SCTLR_EL12, EO_SCTLR12
    SAVE VBAR_EL12, EO_VBAR12
    SAVE ELR_EL12, EO_ELR12
    SAVE SPSR_EL12, EO_SPSR12
    SAVE ESR_EL12, EO_ESR12
    SAVE FAR_EL12, EO_FAR12
    SAVE PAR_EL1, EO_PAR
    adrp x10, __e2_stack_end
    add x10, x10, :lo12:__e2_stack_end
    mov sp, x10
    bl e0_prepare
    cbnz w0, .Lreturn
    adrp x9, e2_vectors
    add x9, x9, :lo12:e2_vectors
    msr VBAR_EL2, x9
    isb
.global e0_seed_brk
e0_seed_brk:
    brk #E2_BRK_IMM
.global e0_seed_after
e0_seed_after:
    adrp x9, e0_state
    add x9, x9, :lo12:e0_state
    ldp x10, x11, [x9, #E0_S_CONFIG]
    msr VTCR_EL2, x10
    msr VTTBR_EL2, x11
    isb
    mrs x12, VTCR_EL2
    cmp x10, x12
    b.ne e2_exception_abort
    mrs x12, VTTBR_EL2
    cmp x11, x12
    b.ne e2_exception_abort
    ldr x10, =E2_GUEST_SCTLR
    msr SCTLR_EL12, x10
    mov x10, #E0_CODE_IPA
    msr VBAR_EL12, x10
    ldr x10, =(E0_STACK_IPA + E2_PAGE_SIZE)
    msr SP_EL1, x10
    adrp x9, e0_saved
    add x9, x9, :lo12:e0_saved
    ldr x10, [x9, #EO_HCR]
    /* Under VHE, target the guest regime for TLBI: TGE=0, VM still off.
     * Guest stage1 has already been disabled, and VTTBR selects our VMID. */
    bic x12, x10, #0x8000000
    msr HCR_EL2, x12
    isb
.global e0_tlbi_enter
e0_tlbi_enter:
    dsb ishst
    tlbi vmalls12e1is
    dsb ish
    isb
    ldr x11, =E2_GUEST_HCR_CLEAR
    bic x10, x10, x11
    orr x10, x10, #1
    orr x10, x10, #0x80000000
    msr HCR_EL2, x10
    isb
    mrs x11, HCR_EL2
    cmp x10, x11
    b.ne e2_exception_abort
    adrp x10, e0_guest_payload
    add x10, x10, :lo12:e0_guest_payload
    adrp x11, e0_guest_vectors
    add x11, x11, :lo12:e0_guest_vectors
    sub x10, x10, x11
    mov x12, #E0_CODE_IPA
    add x10, x10, x12
    msr ELR_EL2, x10
    mov x10, #E2_GUEST_SPSR
    msr SPSR_EL2, x10
    adrp x9, e0_state
    add x9, x9, :lo12:e0_state
    ldr x2, [x9, #E0_S_MODE]
    cmp x2, #E0_WRONG_ADDRESS
    mov x2, #8
    csel x2, x2, xzr, eq
    ldr x11, =E0_DENIED_IPA
    add x2, x2, x11
    ldr x10, [x9, #E0_S_MODE]
    cmp x10, #E0_STATE_DENY
    adrp x11, e0_state_canary
    add x11, x11, :lo12:e0_state_canary
    csel x2, x11, x2, eq
    eret
.global e0_guest_return
e0_guest_return:
    adrp x9, e0_state
    add x9, x9, :lo12:e0_state
    mov x10, #1
    str x10, [x9, #E0_S_RETURNED]
    b e0_restore
.global e2_exception_abort
e2_exception_abort:
    adrp x9, e0_status
    mov x10, #E0_UNEXPECTED
    str x10, [x9, :lo12:e0_status]
.global e0_restore
e0_restore:
    msr daifset, #0xf
    adrp x9, __e2_stack_end
    add x9, x9, :lo12:__e2_stack_end
    mov sp, x9
    adrp x9, e0_restoring
    mov x10, #1
    str x10, [x9, :lo12:e0_restoring]
#ifdef E0_WORLDS
    /* PREPARE never programs a test VMID or enables VM. */
    adrp x9, e0_state
    ldr x10, [x9, :lo12:e0_state]
    cmp x10, #EW_PREPARE
    b.eq .Lworlds_host_bank
#endif
    adrp x9, e0_saved
    add x9, x9, :lo12:e0_saved
    /* Disable stage2 before invalidating the test VMID and restoring tables.
     * Keep our VBAR until every risky control write has completed. */
    ldr x10, [x9, #EO_HCR]
    bic x10, x10, #0x8000000
    msr HCR_EL2, x10
    isb
.global e0_tlbi_exit
e0_tlbi_exit:
    dsb ish
    tlbi vmalls12e1is
    dsb ish
    isb
#ifdef E0_WORLDS
.Lworlds_host_bank:
    bl ew_restore_host
    cbnz w0, e0_restore_failed
    adrp x9, e0_saved
    add x9, x9, :lo12:e0_saved
#endif
.macro RESTORE reg, off
    ldr x10, [x9, #\off]
    msr \reg, x10
    isb
    mrs x11, \reg
    cmp x10, x11
    b.ne e0_restore_failed
.endm
.global e0_restore_vtcr
e0_restore_vtcr:
    RESTORE VTCR_EL2, EO_VTCR
    RESTORE VTTBR_EL2, EO_VTTBR
    RESTORE SCTLR_EL12, EO_SCTLR12
    RESTORE VBAR_EL12, EO_VBAR12
    RESTORE ELR_EL12, EO_ELR12
    RESTORE SPSR_EL12, EO_SPSR12
    RESTORE ESR_EL12, EO_ESR12
    RESTORE FAR_EL12, EO_FAR12
    RESTORE SP_EL1, EO_SP1
    RESTORE PAR_EL1, EO_PAR
    RESTORE HPFAR_EL2, EO_HPFAR
    RESTORE ELR_EL2, EO_ELR
    RESTORE SPSR_EL2, EO_SPSR
    RESTORE ESR_EL2, EO_ESR
    RESTORE FAR_EL2, EO_FAR
    RESTORE HCR_EL2, EO_HCR
    /* Incoming DAIF is fully masked. Keep our vectors for its write/check,
     * then commit inherited VBAR only after every other control is restored. */
    RESTORE DAIF, EO_DAIF
    RESTORE VBAR_EL2, EO_VBAR
#ifdef E0_WORLDS
    adrp x9, ew_trace
    add x9, x9, :lo12:ew_trace
    mov x10, #1
    str x10, [x9, #EW_TRACE_HOST_RESTORED]
#endif
    b .Lreturn
.global e0_restore_failed
e0_restore_failed:
    mov x10, #E0_RESTORE_FAILED
    b e0_terminal
.global e2_exception_direct_abort
e2_exception_direct_abort:
    mov x10, #E0_CONTEXT_LOST
.global e0_terminal
e0_terminal:
    /* A partial EL2 context cannot safely call C or return to the loader.
     * Retain owned mappings/trace, mask interrupts and park for inspection.
     * No retry, ERET, firmware reset or dependence on a working caller stack. */
    msr daifset, #0xf
    isb
    adrp x9, e0_status
    str x10, [x9, :lo12:e0_status]
#ifdef E0_WORLDS
    adrp x9, ew_trace
    add x9, x9, :lo12:ew_trace
    str x10, [x9, #40]
    str xzr, [x9, #EW_TRACE_HOST_RESTORED]
#endif
    adrp x9, e0_state
    add x9, x9, :lo12:e0_state
    str xzr, [x9, #E0_S_RETURNED]
    dsb sy
.global e0_terminal_park
e0_terminal_park:
    b e0_terminal_park
.global e0_return
e0_return:
.Lreturn:
    adrp x9, e0_status
    ldr x0, [x9, :lo12:e0_status]
    adrp x9, e0_saved
    ldr x9, [x9, :lo12:e0_saved]
    mov sp, x9
    ldp x18, x19, [sp]
    ldp x20, x21, [sp, #16]
    ldp x22, x23, [sp, #32]
    ldp x24, x25, [sp, #48]
    ldp x26, x27, [sp, #64]
    ldp x28, x29, [sp, #80]
    ldr x30, [sp, #96]
    add sp, sp, #E2_CALL_FRAME_SIZE
    adrp x9, e0_saved
    add x9, x9, :lo12:e0_saved
    ldr x9, [x9, #EO_NZCV]
    msr NZCV, x9
    ret
.Lrefuse:
    msr NZCV, x17
    mov x0, #E0_ENVIRONMENT
    ret

.section .guest_text,"ax"
.balign 0x800
.global e0_guest_vectors
e0_guest_vectors:
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        hvc #(E2_HVC_FAULT_BASE + \n)
        .inst 0
        .space 120
    .endr
.global e0_guest_payload
e0_guest_payload:
    mov x19, #0x319
    mov x20, #0x320
    stp x19, x20, [sp, #-16]!
    mov x0, #E0_VALUE
    ldr x1, =E0_ALLOWED_IPA
    str x0, [x1]
    ldr x0, [x1]
    mov x4, #E0_VALUE
    cmp x0, x4
    b.ne .Lguest_fail
    hvc #E2_HVC_PROBE
.global e0_after_probe
e0_after_probe:
    cmp x0, #E2_HVC_REPLY
    b.ne .Lguest_fail
    mov x1, x2
    mov x3, #E0_STORE_VALUE
.global e0_fault_pc
e0_fault_pc:
    str x3, [x1]
.global e0_after_fault
e0_after_fault:
    cmp x19, #0x319
    b.ne .Lguest_fail
    cmp x20, #0x320
    b.ne .Lguest_fail
    ldp x19, x20, [sp], #16
    mov x0, #0
    b .Lguest_done
.Lguest_fail:
    mov x0, #1
.Lguest_done:
    hvc #E2_HVC_DONE
.global e0_after_done
e0_after_done:
    .inst 0
.section .note.GNU-stack,"",%progbits
