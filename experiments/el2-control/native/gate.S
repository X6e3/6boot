/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "native.h"
.arch armv8.1-a
.text
.balign 16
.global el2_native_invoke
.type el2_native_invoke,%function
el2_native_invoke:
    sub sp,sp,#112
    stp x18,x19,[sp]
    stp x20,x21,[sp,#16]
    stp x22,x23,[sp,#32]
    stp x24,x25,[sp,#48]
    stp x26,x27,[sp,#64]
    stp x28,x29,[sp,#80]
    str x30,[sp,#96]
    adrp x17,el2_native_call_state
    add x17,x17,:lo12:el2_native_call_state
    mov x9,sp
    str x9,[x17,#EN_ORIG_SP]
    mrs x9,DAIF
    str x9,[x17,#EN_ORIG_DAIF]
    msr DAIFSet,#0xf
    isb
    mov sp,x5
    str x5,[x17,#EN_EXPECT_SP]
.macro EN_SNAPSHOT
    .irp reg,VBAR_EL2,HCR_EL2,ELR_EL2,SPSR_EL2,ESR_EL2,FAR_EL2,SP_EL0,SP_EL1,SCTLR_EL12,VBAR_EL12,ELR_EL12,SPSR_EL12,PAR_EL1,DAIF,SPSel,PAN,SCTLR_EL2,TCR_EL2,TTBR0_EL2,TTBR1_EL2,MAIR_EL2,MPIDR_EL1,CurrentEL
        mrs x9,\reg
        str x9,[x10],#8
    .endr
    mov x9,sp
    str x9,[x10],#8
    .irp reg,ESR_EL12,FAR_EL12
        mrs x9,\reg
        str x9,[x10],#8
    .endr
.endm
    mov x10,x17
    EN_SNAPSHOT
    mov x16,x0
    mov x0,x1
    mov x1,x2
    mov x2,x3
    mov x3,x4
    .irp n,18,19,20,21,22,23,24,25,26,27,28,29
        mov x\n,#(0x700+\n)
    .endr
    blr x16
    adrp x17,el2_native_call_state
    add x17,x17,:lo12:el2_native_call_state
    mov w0,w0 /* int return: discard unspecified upper bits. */
    str x0,[x17,#EN_RET]
    stp x18,x19,[x17,#EN_GPR]
    stp x20,x21,[x17,#(EN_GPR+16)]
    stp x22,x23,[x17,#(EN_GPR+32)]
    stp x24,x25,[x17,#(EN_GPR+48)]
    stp x26,x27,[x17,#(EN_GPR+64)]
    stp x28,x29,[x17,#(EN_GPR+80)]
    mov x9,sp
    str x9,[x17,#EN_OBS_SP]
    add x10,x17,#EN_AFTER
    EN_SNAPSHOT
    mov x16,#EN_RETURNED
    mov x12,#EN_SYSTEM_COUNT
    mov x13,x17
    add x14,x17,#EN_AFTER
.Lcompare_state:
    ldr x10,[x13],#8
    ldr x11,[x14],#8
    cmp x10,x11
    b.ne .Lstate_done
    subs x12,x12,#1
    b.ne .Lcompare_state
    orr x16,x16,#EN_CONTEXT
.Lstate_done:
    add x13,x17,#EN_GPR
    mov x11,#0x712
    mov x12,#12
.Lcompare_gprs:
    ldr x10,[x13],#8
    cmp x10,x11
    b.ne .Lgprs_done
    add x11,x11,#1
    subs x12,x12,#1
    b.ne .Lcompare_gprs
    orr x16,x16,#EN_GPRS
.Lgprs_done:
    ldr x10,[x17,#EN_EXPECT_SP]
    ldr x11,[x17,#EN_OBS_SP]
    cmp x10,x11
    b.ne .Lsp_done
    orr x16,x16,#EN_SP
.Lsp_done:
    /* Restore PAN before diagnostics even on CONTEXT_LOST. Do not retry a
     * failed HCR/EL12 restore outside the module's vector environment. */
    ldr x9,[x17,#EN_PAN_OFFSET]
    msr PAN,x9
    isb
    cmp x16,#15
    b.ne .Lkeep_masked
    ldr w0,[x17,#EN_RET]
    cmp w0,#E2_CONTEXT_LOST
    b.eq .Lkeep_masked
    cmp w0,#E2_RESTORE_FAILED
    b.eq .Lkeep_masked
    ldr x10,[x17,#EN_ORIG_DAIF]
    msr DAIF,x10
    isb
    mrs x11,DAIF
    cmp x10,x11
    b.ne .Lkeep_masked
    orr x16,x16,#EN_DAIF
    b .Lreturn
.Lkeep_masked:
    msr DAIFSet,#0xf
.Lreturn:
    str x16,[x17,#EN_FLAGS]
    ldr w0,[x17,#EN_RET]
    ldr x9,[x17,#EN_ORIG_SP]
    mov sp,x9
    ldp x18,x19,[sp]
    ldp x20,x21,[sp,#16]
    ldp x22,x23,[sp,#32]
    ldp x24,x25,[sp,#48]
    ldp x26,x27,[sp,#64]
    ldp x28,x29,[sp,#80]
    ldr x30,[sp,#96]
    add sp,sp,#112
    ret
.size el2_native_invoke,.-el2_native_invoke
.section .note.GNU-stack,"",%progbits
