/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SIXBOOT_EL2_EXCEPTION_H
#define SIXBOOT_EL2_EXCEPTION_H
#include "el2_control.h"

#define E2_EXPECTED 13
#define E2_UNEXPECTED 14
#define E2_NESTED 15
#define E2_BAD_STACK 16
#define E2_TRACE_FULL 17
#define E2_NOT_TAKEN 18
#define E2_RESTORE_FAILED 19
#define E2_CONTEXT_LOST 20
/* Guest (EL1/HVC) stage results, kept apart from the BRK stage status. */
#define E2_GUEST_HVC 21          /* probe HVC taken at EL2 and returned to EL1 */
#define E2_GUEST_DONE 22         /* guest verified its registers and finished */
#define E2_GUEST_MISMATCH 23     /* guest finished but its register check failed */
#define E2_GUEST_HCR_REFUSED 24  /* HCR_EL2 read back differently; guest not entered */
#define E2_GUEST_NOT_RUN 25
#define E2_GUEST_UNEXPECTED 26   /* any other exception while the guest was active */
#define E2_GUEST_NOT_IDENTITY 27 /* guest code not identity-mapped at EL2; not entered */
#define E2_GUEST_SIGNATURE 28
#define E2_BRK_IMM 0x6e32
#define E2_HVC_PROBE 0x6e33
#define E2_HVC_DONE 0x6e34
#define E2_HVC_SIGNATURE 0x6e37
#define E2_SIGNATURE_REPLY 0x6e3
#define E2_HVC_STRAY 0x6e35      /* only reached if EL2 ever resumed after DONE */
#define E2_HVC_FAULT_BASE 0x6e40 /* guest vector n issues HVC #(base + n) */
#define E2_HVC_REPLY 0x5a5       /* EL2 writes this into guest x0 at the probe */
#define E2_GUEST_PATTERN 0x200   /* guest seeds xN = 0x200 + N */
#define E2_GUEST_SPSR 0x3c5      /* EL1h, D/A/I/F masked */
#define E2_GUEST_SCTLR 0x30d00800 /* RES1 bits only: stage 1 off, caches off, LE */
/* Cleared for the guest run: TGE, HCD, AMO, IMO, FMO, VM. */
#define E2_GUEST_HCR_CLEAR 0x28000039
#define E2_GF_HCR_WRITTEN 1
#define E2_GF_EL12_SAVED 2
#define E2_GF_ENTERED 4
#define E2_GF_RESTORING 8 /* restore pass started; a re-entry skips guest writes */
#define E2_GF_PREPARING 16
#define E2_GF_PAR_SAVED 32
#define E2_VECTOR_ALIGN 0x800
#define E2_VECTOR_STRIDE 0x80
#define E2_VECTOR_COUNT 16
#define E2_FRAME_SIZE 384
#define E2_HANDLER_C_BUDGET 1024
#define E2_EXCEPTION_CAPACITY 4
#define E2_EXCEPTION_RESULT_SIZE 2048
#define E2_EXCEPTION_VERSION 3
#define E2_EXCEPTION_HEADER_SIZE 272
#define E2_EXCEPTION_TRACE_BYTES 1600
#define E2_OVERFLOW_OFFSET 1856
#define E2_OVERFLOW_SIZE 64

/* Assembly frame offsets; x0..x30 occupy 0..247. */
#define EF_SP 248
#define EF_ESR 256
#define EF_ELR 264
#define EF_SPSR 272
#define EF_FAR 280
#define EF_DAIF 288
#define EF_HCR 296
#define EF_VBAR 304
#define EF_MPIDR 312
#define EF_VECTOR 320
#define EF_DEPTH 328
#define EF_STATUS 336
#define EF_RESUME_ELR 344
#define EF_RESUME_SPSR 352
#define EF_SP_EL0 360
#define EF_FLAGS 368
#define EF_SP_EL1 376

#define ES_VBAR 0
#define ES_DAIF 8
#define ES_ELR 16
#define ES_SPSR 24
#define ES_ESR 32
#define ES_FAR 40
#define ES_HCR 48
#define ES_SP_EL0 56
#define ES_CALL_SP 64
#define ES_HOST_SPSR 72
#define ES_EXPECTED_PC 80
#define ES_RESUME_PC 88
#define ES_ABORT_PC 96
#define ES_STATUS 104
#define ES_ACTIVE 112
#define ES_DEPTH 120
#define ES_RESTORE_MASK 128
#define ES_HOST_VALID 136
#define ES_HANDLER_VBAR 144
#define ES_LAST_VECTOR 152
#define ES_ERET_PLANNED 160
#define ES_INSTALLED 168
#define ES_RETURNED 176
#define ES_SP_EL1 184
#define ES_GUEST_REQUESTED 192
#define ES_GUEST_STATUS 200
#define ES_GUEST_HCR 208
#define ES_OLD_SCTLR12 216
#define ES_OLD_VBAR12 224
#define ES_OLD_ELR12 232
#define ES_OLD_SPSR12 240
#define ES_GUEST_PROBE_PC 248
#define ES_GUEST_DONE_PC 256
#define ES_GUEST_RETURN_PC 264
#define ES_HVC_COUNT 272
#define ES_GUEST_FLAGS 280
#define ES_PRE_BRK_SP 288
#define ES_GUEST_VERDICT 296
#define ES_GUEST_ACTIVE 304
#define ES_SIGNATURE_SEEN 312
#define ES_OLD_PAR 320
#define ES_GUEST_SIGNATURE_PC 328
#define ES_OLD_ESR12 336
#define ES_OLD_FAR12 344
#define E2_SESSION_SIZE 352

#define E2_R_VBAR 1
#define E2_R_DAIF 2
#define E2_R_ELR 4
#define E2_R_SPSR 8
#define E2_R_ESR 16
#define E2_R_FAR 32
#define E2_R_UNCHANGED 64
#define E2_R_PSTATE 128
#define E2_R_STACK 256
#define E2_R_GUEST 512 /* EL1 banked state restored, or never changed */
#define E2_R_ALL 1023
#define E2_ACTION_ERET 0
#define E2_ACTION_DIRECT_ABORT 1

#ifndef __ASSEMBLER__
#define E2_EXCEPTION_MAGIC UINT32_C(0x45324558) /* E2EX, LE */
#define E2_OVERFLOW_MAGIC UINT32_C(0x45324f46)
#define E2_F_ESR_VALID 1u
#define E2_F_FAR_VALID 2u
#define E2_X_INSTALLED 1u
#define E2_X_RESTORED 2u
#define E2_X_EXPECTED 4u
#define E2_X_GUEST_DONE 8u
#define E2_X_SIGNATURE 16u

struct e2_exception_frame {
    uint64_t x[31], el2_sp, esr, elr, spsr, far;
    uint64_t daif, hcr, vbar, mpidr, vector, depth, status;
    uint64_t resume_elr, resume_spsr, sp_el0, flags, sp_el1;
};
struct e2_exception_session {
    uint64_t old_vbar, old_daif, old_elr, old_spsr, old_esr, old_far;
    uint64_t old_hcr, old_sp_el0, call_sp, host_spsr, expected_pc, resume_pc;
    uint64_t abort_pc, status, active, depth, restore_mask, host_valid;
    uint64_t handler_vbar, last_vector, eret_planned, installed, returned, old_sp_el1;
    uint64_t guest_requested, guest_status, guest_hcr, old_sctlr12, old_vbar12;
    uint64_t old_elr12, old_spsr12, guest_probe_pc, guest_done_pc, guest_return_pc;
    uint64_t hvc_count, guest_flags, pre_brk_sp, guest_verdict, guest_active, signature_seen;
    uint64_t old_par, guest_signature_pc, old_esr12, old_far12;
};
struct e2_exception_trace {
    uint64_t count, dropped;
    uint64_t overflow_esr, overflow_elr, overflow_spsr, overflow_far, overflow_vector, overflow_flags;
    struct e2_exception_frame frame[E2_EXCEPTION_CAPACITY];
};
#define E2_ASSERT(type, field, off) \
    _Static_assert(offsetof(struct type, field) == off, #type ": " #field)
E2_ASSERT(e2_exception_frame, el2_sp, EF_SP);
E2_ASSERT(e2_exception_frame, esr, EF_ESR);
E2_ASSERT(e2_exception_frame, elr, EF_ELR);
E2_ASSERT(e2_exception_frame, spsr, EF_SPSR);
E2_ASSERT(e2_exception_frame, far, EF_FAR);
E2_ASSERT(e2_exception_frame, daif, EF_DAIF);
E2_ASSERT(e2_exception_frame, hcr, EF_HCR);
E2_ASSERT(e2_exception_frame, vbar, EF_VBAR);
E2_ASSERT(e2_exception_frame, mpidr, EF_MPIDR);
E2_ASSERT(e2_exception_frame, vector, EF_VECTOR);
E2_ASSERT(e2_exception_frame, depth, EF_DEPTH);
E2_ASSERT(e2_exception_frame, status, EF_STATUS);
E2_ASSERT(e2_exception_frame, resume_elr, EF_RESUME_ELR);
E2_ASSERT(e2_exception_frame, resume_spsr, EF_RESUME_SPSR);
E2_ASSERT(e2_exception_frame, sp_el0, EF_SP_EL0);
E2_ASSERT(e2_exception_frame, flags, EF_FLAGS);
E2_ASSERT(e2_exception_frame, sp_el1, EF_SP_EL1);
E2_ASSERT(e2_exception_session, old_vbar, ES_VBAR);
E2_ASSERT(e2_exception_session, old_daif, ES_DAIF);
E2_ASSERT(e2_exception_session, old_elr, ES_ELR);
E2_ASSERT(e2_exception_session, old_spsr, ES_SPSR);
E2_ASSERT(e2_exception_session, old_esr, ES_ESR);
E2_ASSERT(e2_exception_session, old_far, ES_FAR);
E2_ASSERT(e2_exception_session, old_hcr, ES_HCR);
E2_ASSERT(e2_exception_session, old_sp_el0, ES_SP_EL0);
E2_ASSERT(e2_exception_session, call_sp, ES_CALL_SP);
E2_ASSERT(e2_exception_session, host_spsr, ES_HOST_SPSR);
E2_ASSERT(e2_exception_session, expected_pc, ES_EXPECTED_PC);
E2_ASSERT(e2_exception_session, resume_pc, ES_RESUME_PC);
E2_ASSERT(e2_exception_session, abort_pc, ES_ABORT_PC);
E2_ASSERT(e2_exception_session, status, ES_STATUS);
E2_ASSERT(e2_exception_session, active, ES_ACTIVE);
E2_ASSERT(e2_exception_session, depth, ES_DEPTH);
E2_ASSERT(e2_exception_session, restore_mask, ES_RESTORE_MASK);
E2_ASSERT(e2_exception_session, host_valid, ES_HOST_VALID);
E2_ASSERT(e2_exception_session, handler_vbar, ES_HANDLER_VBAR);
E2_ASSERT(e2_exception_session, last_vector, ES_LAST_VECTOR);
E2_ASSERT(e2_exception_session, eret_planned, ES_ERET_PLANNED);
E2_ASSERT(e2_exception_session, installed, ES_INSTALLED);
E2_ASSERT(e2_exception_session, returned, ES_RETURNED);
E2_ASSERT(e2_exception_session, old_sp_el1, ES_SP_EL1);
E2_ASSERT(e2_exception_session, guest_requested, ES_GUEST_REQUESTED);
E2_ASSERT(e2_exception_session, guest_status, ES_GUEST_STATUS);
E2_ASSERT(e2_exception_session, guest_hcr, ES_GUEST_HCR);
E2_ASSERT(e2_exception_session, old_sctlr12, ES_OLD_SCTLR12);
E2_ASSERT(e2_exception_session, old_vbar12, ES_OLD_VBAR12);
E2_ASSERT(e2_exception_session, old_elr12, ES_OLD_ELR12);
E2_ASSERT(e2_exception_session, old_spsr12, ES_OLD_SPSR12);
E2_ASSERT(e2_exception_session, guest_probe_pc, ES_GUEST_PROBE_PC);
E2_ASSERT(e2_exception_session, guest_done_pc, ES_GUEST_DONE_PC);
E2_ASSERT(e2_exception_session, guest_return_pc, ES_GUEST_RETURN_PC);
E2_ASSERT(e2_exception_session, hvc_count, ES_HVC_COUNT);
E2_ASSERT(e2_exception_session, guest_flags, ES_GUEST_FLAGS);
E2_ASSERT(e2_exception_session, pre_brk_sp, ES_PRE_BRK_SP);
E2_ASSERT(e2_exception_session, guest_verdict, ES_GUEST_VERDICT);
E2_ASSERT(e2_exception_session, guest_active, ES_GUEST_ACTIVE);
E2_ASSERT(e2_exception_session, signature_seen, ES_SIGNATURE_SEEN);
E2_ASSERT(e2_exception_session, old_par, ES_OLD_PAR);
E2_ASSERT(e2_exception_session, guest_signature_pc, ES_GUEST_SIGNATURE_PC);
E2_ASSERT(e2_exception_session, old_esr12, ES_OLD_ESR12);
E2_ASSERT(e2_exception_session, old_far12, ES_OLD_FAR12);
_Static_assert(E2_EXCEPTION_HEADER_SIZE + E2_EXCEPTION_CAPACITY * E2_FRAME_SIZE <= E2_OVERFLOW_OFFSET, "trace/overflow disjoint");
_Static_assert(E2_OVERFLOW_OFFSET + E2_OVERFLOW_SIZE <= E2_EXCEPTION_RESULT_SIZE, "overflow fits");
#undef E2_ASSERT
_Static_assert(sizeof(struct e2_exception_frame) == E2_FRAME_SIZE, "frame ABI");
_Static_assert(sizeof(struct e2_exception_session) == E2_SESSION_SIZE, "session ABI");
_Static_assert(sizeof(struct e2_exception_trace) == E2_EXCEPTION_TRACE_BYTES, "trace ABI");
_Static_assert(sizeof(struct e2_exception_trace) <= E2_PAGE_SIZE, "trace page");
_Static_assert(E2_FRAME_SIZE % 16 == 0, "exception SP alignment");
_Static_assert(E2_OVERFLOW_OFFSET + E2_OVERFLOW_SIZE <= E2_EXCEPTION_RESULT_SIZE, "overflow result bounds");

/* Bounded leaf dispatcher: only preallocated frame/session/trace RAM. It has
 * no device accesses, allocation, logging, library calls or recursion.
 * Same function is tested on host and called by the firmware handler. */
int e2_exception_dispatch(struct e2_exception_frame *frame,
                          struct e2_exception_session *session,
                          struct e2_exception_trace *trace);
int e2_exception_prepare(const void *request, size_t size, void *result,
                         size_t capacity, const struct e2_snapshot *snapshot,
                         uint64_t image_base);
int e2_exception_encode(void *result, size_t capacity,
                        const struct e2_exception_session *session,
                        const struct e2_exception_trace *trace);
int sixboot_el2_exception_entry(const void *request, size_t request_size,
                               void *result, size_t result_capacity);
/* BRK stage, then (only if it passed) one EL1 guest with a probe HVC. */
int sixboot_el2_guest_entry(const void *request, size_t request_size,
                           void *result, size_t result_capacity);
#endif
#endif
