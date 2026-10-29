/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "el2_exception.h"
/* EL1 guest for the HVC stage. It runs with stage 1 and stage 2 off and
 * D/A/I/F masked, makes no memory accesses and can only leave through HVC:
 * even its own exception vectors turn every EL1 exception into an HVC that
 * the EL2 handler treats as unexpected. */
.section .guest_text,"ax"
.balign E2_VECTOR_ALIGN
.global e2_guest_vectors
e2_guest_vectors:
.macro E2_GUEST_VECTOR number
.global e2_guest_vector_\number
e2_guest_vector_\number:
    hvc #(E2_HVC_FAULT_BASE + \number)
    /* EL2 never resumes an unexpected HVC; stay put if it ever did. */
    hvc #E2_HVC_STRAY
    b .
    .space E2_VECTOR_STRIDE - (. - e2_guest_vector_\number)
.endm
.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    E2_GUEST_VECTOR \n
.endr
.global e2_guest_vectors_end
e2_guest_vectors_end:

.balign 16
.global e2_guest_payload
e2_guest_payload:
    .irp reg,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
        mov x\reg, #(E2_GUEST_PATTERN + \reg)
    .endr
.global e2_guest_probe
e2_guest_probe:
    hvc #E2_HVC_PROBE
.global e2_guest_after_probe
e2_guest_after_probe:
    /* EL2 answers in x0 and must leave x1..x30 exactly as seeded. */
    cmp x0, #E2_HVC_REPLY
    b.ne .Lguest_mismatch
    .irp reg,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
        cmp x\reg, #(E2_GUEST_PATTERN + \reg)
        b.ne .Lguest_mismatch
    .endr
.global e2_guest_signature
e2_guest_signature:
    hvc #0x6e37
.global e2_guest_after_signature
e2_guest_after_signature:
    cmp x0, #E2_SIGNATURE_REPLY
    b.ne .Lguest_mismatch
    mov x0, #0
    b e2_guest_done
.Lguest_mismatch:
    mov x0, #1
.global e2_guest_done
e2_guest_done:
    hvc #E2_HVC_DONE
.global e2_guest_after_done
e2_guest_after_done:
    hvc #E2_HVC_STRAY
    b e2_guest_after_done
.section .note.GNU-stack,"",%progbits
