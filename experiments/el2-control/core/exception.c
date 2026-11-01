/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "el2_exception.h"

static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32));
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const uint8_t *p)
{
    return get32(p) | (uint64_t)get32(p + 4) << 32;
}

int e2_exception_prepare(const void *request, size_t size, void *result,
                         size_t capacity, const struct e2_snapshot *s, uint64_t base)
{
    int status = e2_buffers_check(request, size, result, capacity,
                                  E2_EXCEPTION_RESULT_SIZE, s, base);
    if (status) return status;
    status = e2_foundation_run(request, size, result, capacity, s, base);
    if (status) return status;
    /* A declared exclusion must cover the caller frame in a stack at least
     * as large as P3's 16 KiB stack. The actual mapping is still loader-owned. */
    const uint8_t *p = request;
    unsigned nr = get32(p + 48), nx = get32(p + 56);
    const uint8_t *r = p + E2_HEADER_SIZE + 16u * nr;
    for (unsigned i = 0; i < nx; i++, r += 16) {
        uint64_t lo = get64(r), bytes = get64(r + 8);
        if (bytes >= E2_PAGE_SIZE && lo <= s->active_sp - E2_CALL_FRAME_SIZE && s->active_sp <= lo + bytes)
            return E2_READY;
    }
    put32((uint8_t *)result + 8, E2_ENVIRONMENT);
    put32((uint8_t *)result + 12, E2_RESULT_PREFLIGHT_ONLY);
    return E2_ENVIRONMENT;
}

/* Forced inline: the dispatcher must stay a leaf (the ELF audit checks it). */
static inline __attribute__((always_inline)) int in_code(uint64_t pc, uint64_t base)
{
    return pc >= base && pc < base + E2_PAGE_SIZE && !(pc % 4);
}
static inline __attribute__((always_inline)) uint64_t hvc_esr(uint64_t imm)
{
    return UINT64_C(0x16) << 26 | UINT64_C(1) << 25 | imm; /* EC HVC64, IL */
}

/* This is the sole C call from vectors.S. The build audit requires it to be
 * a bounded leaf, use only GPRs, and stay within E2_HANDLER_C_BUDGET stack. */
__attribute__((noinline))
int e2_exception_dispatch(struct e2_exception_frame *f,
                          struct e2_exception_session *s, struct e2_exception_trace *t)
{
    if (!f || !s || !t) return E2_ACTION_DIRECT_ABORT;
    uint64_t base = s->handler_vbar & ~((uint64_t)E2_ARENA_ALIGN - 1);
    uint64_t mode = f->spsr & 0x1f;
    int host = mode == 9 && !(f->spsr & (UINT64_C(1) << 20)) &&
               (f->spsr & E2_DAIF_MASK) == E2_DAIF_MASK;
    int targets = base && base <= UINT64_MAX - E2_ARENA_SIZE &&
                  !(s->handler_vbar % E2_VECTOR_ALIGN) &&
                  s->handler_vbar <= base + E2_PAGE_SIZE - E2_VECTOR_ALIGN &&
                  s->expected_pc >= base && s->expected_pc < base + E2_PAGE_SIZE - 4 &&
                  !(s->expected_pc % 4) && s->resume_pc == s->expected_pc + 4 &&
                  s->abort_pc >= base && s->abort_pc < base + E2_PAGE_SIZE && !(s->abort_pc % 4);
    if (!s->host_valid && f->depth == 1 && host) {
        s->host_spsr = f->spsr;
        s->host_valid = 1;
    }
    f->flags = 0;
    unsigned kind = (unsigned)(f->vector & 3);
    unsigned ec = (unsigned)((f->esr >> 26) & 0x3f);
    if (f->vector < 16 && (kind == 0 || kind == 3)) f->flags |= E2_F_ESR_VALID;
    if (f->vector < 16 && kind == 0 &&
        (ec == 0x20 || ec == 0x21 || ec == 0x24 || ec == 0x25) && !(f->esr & (1u << 10)))
        f->flags |= E2_F_FAR_VALID;
    int stack_ok = !(f->el2_sp % 16) &&
                   f->el2_sp >= base + E2_STACK_OFFSET + E2_FRAME_SIZE + E2_HANDLER_C_BUDGET &&
                   f->el2_sp <= base + E2_STACK_OFFSET + E2_PAGE_SIZE;
    /* Any exception from a lower EL while the guest stage is active belongs
     * to the guest stage; its result never overwrites the BRK stage status. */
    int guest_running = s->guest_active == 1 && f->vector >= 8 && f->vector < 16;
    int guest_frame = guest_running || (s->guest_requested == 1 &&
                                       (s->guest_flags & E2_GF_PREPARING));
    f->status = E2_UNEXPECTED;
    if (guest_frame) {
        int el1h = (f->spsr & 0x1f) == 5 && !(f->spsr & (UINT64_C(1) << 20)) &&
                   (f->spsr & E2_DAIF_MASK) == E2_DAIF_MASK;
        int pcs = in_code(s->guest_probe_pc, base) && in_code(s->guest_done_pc, base) &&
                  in_code(s->guest_signature_pc, base) &&
                  in_code(s->guest_return_pc, base);
        int seeded = 1;
        int signature_regs = f->x[0] == E2_HVC_REPLY;
        for (unsigned i = 0; i < 31; i++)
            if (f->x[i] != E2_GUEST_PATTERN + i) seeded = 0;
        for (unsigned i = 1; i < 31; i++)
            if (f->x[i] != E2_GUEST_PATTERN + i) signature_regs = 0;
        f->status = E2_GUEST_UNEXPECTED;
        if (guest_running && f->depth == 1 && targets && stack_ok && pcs && el1h && f->vector == 8 &&
            f->hcr == s->guest_hcr && f->vbar == s->handler_vbar) {
            /* HVC's preferred return address is the next instruction, so
             * ELR already points past the HVC: compare, never adjust. */
            if (f->esr == hvc_esr(E2_HVC_PROBE) && s->hvc_count == 0 && t->count == 1 &&
                f->elr == s->guest_probe_pc && seeded)
                f->status = E2_GUEST_HVC;
            else if (f->esr == hvc_esr(E2_HVC_SIGNATURE) && s->hvc_count == 1 && t->count == 2 &&
                     s->signature_seen == 0 && f->elr == s->guest_signature_pc && signature_regs)
                f->status = E2_GUEST_SIGNATURE;
            else if (f->esr == hvc_esr(E2_HVC_DONE) && s->hvc_count == 2 && t->count == 3 &&
                     s->signature_seen == 1 &&
                     f->elr == s->guest_done_pc)
                f->status = f->x[0] == 0 ? E2_GUEST_DONE : E2_GUEST_MISMATCH;
        }
    } else if (f->depth != 1) f->status = E2_NESTED;
    else if (!targets || !stack_ok)
        f->status = E2_BAD_STACK;
    else if (s->active == 1 && f->vector == 4 && host && t->count == 0 &&
             (f->daif & E2_DAIF_MASK) == E2_DAIF_MASK &&
             f->esr == (UINT64_C(0x3c) << 26 | UINT64_C(1) << 25 | E2_BRK_IMM) &&
             f->elr == s->expected_pc && f->hcr == s->old_hcr && f->vbar == s->handler_vbar)
        f->status = E2_EXPECTED;

    int action = E2_ACTION_ERET;
    int saved_host = s->host_valid == 1 && (s->host_spsr & 0x1f) == 9 &&
                     !(s->host_spsr & (UINT64_C(1) << 20)) &&
                     (s->host_spsr & E2_DAIF_MASK) == E2_DAIF_MASK;
    if (t->count >= E2_EXCEPTION_CAPACITY) {
        if (t->dropped != UINT64_MAX) t->dropped++;
        /* Keep a bounded last-overflow syndrome even after all four normal
         * frames are occupied. Do not destroy the successful guest history. */
        t->overflow_esr = f->esr; t->overflow_elr = f->elr;
        t->overflow_spsr = f->spsr; t->overflow_far = f->far;
        t->overflow_vector = f->vector; t->overflow_flags = f->flags;
        f->status = E2_TRACE_FULL;
    }
    if (!targets || !saved_host || s->active != 1) {
        f->status = E2_CONTEXT_LOST;
        f->resume_elr = f->resume_spsr = 0;
        s->eret_planned = 0;
        action = E2_ACTION_DIRECT_ABORT;
    } else if (f->status == E2_EXPECTED || f->status == E2_GUEST_HVC || f->status == E2_GUEST_SIGNATURE) {
        /* Resume where the exception was taken: the BRK skips itself, the HVC
         * already points past itself. Same PSTATE as at the exception. */
        f->resume_elr = f->status == E2_EXPECTED ? s->resume_pc : f->elr;
        f->resume_spsr = f->spsr;
        s->eret_planned = 1;
    } else if (f->status == E2_GUEST_DONE || f->status == E2_GUEST_MISMATCH) {
        f->resume_elr = s->guest_return_pc;
        f->resume_spsr = s->host_spsr;
        s->eret_planned = 1;
    } else {
        /* Everything else leaves through the host abort path, never back into
         * the interrupted code and never into the guest. */
        f->resume_elr = s->abort_pc;
        f->resume_spsr = s->host_spsr;
        s->eret_planned = 1;
    }
    if (t->count < E2_EXCEPTION_CAPACITY) {
        /* Fixed-size copy, no memcpy helper or external code in the handler. */
        const uint8_t *from = (const uint8_t *)f;
        uint8_t *to = (uint8_t *)&t->frame[t->count];
        for (unsigned i = 0; i < E2_FRAME_SIZE; i++) to[i] = from[i];
        t->count++;
    }
    s->last_vector = f->vector;
    if (guest_frame) {
        s->guest_status = f->status;
        if (f->status == E2_GUEST_HVC) {
            s->hvc_count = 1;
            /* After the trace copy, so the trace keeps the guest's own x0. */
            f->x[0] = E2_HVC_REPLY;
        } else if (f->status == E2_GUEST_SIGNATURE) {
            s->hvc_count = 2;
            s->signature_seen = 1;
            f->x[0] = E2_SIGNATURE_REPLY;
        } else {
            if (f->status == E2_GUEST_DONE || f->status == E2_GUEST_MISMATCH) {
                s->hvc_count = 3;
                s->guest_verdict = f->x[0];
            }
            s->guest_active = 0;
        }
    } else {
        s->status = f->status;
    }
    return action;
}

int e2_exception_encode(void *result, size_t capacity,
                        const struct e2_exception_session *s,
                        const struct e2_exception_trace *t)
{
    uintptr_t out = (uintptr_t)result, a = (uintptr_t)s, b = (uintptr_t)t;
    if (!result || !s || !t || capacity < E2_EXCEPTION_RESULT_SIZE ||
        a % _Alignof(struct e2_exception_session) || b % _Alignof(struct e2_exception_trace) ||
        E2_EXCEPTION_RESULT_SIZE > UINTPTR_MAX - out ||
        sizeof(*s) > UINTPTR_MAX - a || sizeof(*t) > UINTPTR_MAX - b ||
        (out < a + sizeof(*s) && a < out + E2_EXCEPTION_RESULT_SIZE) ||
        (out < b + sizeof(*t) && b < out + E2_EXCEPTION_RESULT_SIZE)) return E2_ARGUMENT;
    uint32_t status = s->status <= E2_CONTEXT_LOST ? (uint32_t)s->status : E2_CONTEXT_LOST;
    int restored = s->restore_mask == E2_R_ALL && s->active == 0 && s->depth == 0 &&
                   s->installed == 1 && s->returned == 1;
    int entered = s->guest_requested == 1 && (s->guest_flags & E2_GF_ENTERED);
    /* Without a guest stage the BRK frame must be the only one. */
    if (status == E2_EXPECTED && (!restored || t->count < 1 || (!s->guest_requested && t->count != 1) ||
                                  t->frame[0].status != E2_EXPECTED))
        status = E2_RESTORE_FAILED;
    if (t->count > E2_EXCEPTION_CAPACITY) status = E2_TRACE_FULL;
    /* The guest stage only claims DONE with the exact four-frame history
     * BRK, probe HVC, signature HVC, DONE HVC and a complete restore. */
    uint32_t guest = s->guest_status >= E2_UNEXPECTED && s->guest_status <= E2_GUEST_SIGNATURE ?
                     (uint32_t)s->guest_status : E2_GUEST_UNEXPECTED;
    if (!s->guest_requested) guest = E2_GUEST_NOT_RUN;
    else if (status != E2_EXPECTED && guest == E2_GUEST_DONE) guest = E2_GUEST_UNEXPECTED;
    if (guest == E2_GUEST_DONE &&
        (!entered || t->count != 4 || s->hvc_count != 3 || s->guest_verdict != 0 ||
         s->signature_seen != 1 || t->frame[1].status != E2_GUEST_HVC ||
         t->frame[2].status != E2_GUEST_SIGNATURE || t->frame[3].status != E2_GUEST_DONE))
        guest = E2_GUEST_UNEXPECTED;
    uint8_t *p = result;
    for (unsigned i = 0; i < E2_EXCEPTION_RESULT_SIZE; i++) p[i] = 0;
    put32(p, E2_EXCEPTION_MAGIC);
    put32(p + 4, (uint32_t)E2_EXCEPTION_RESULT_SIZE << 16 | E2_EXCEPTION_VERSION);
    put32(p + 8, status);
    put32(p + 12, (s->installed ? E2_X_INSTALLED : 0) | (restored ? E2_X_RESTORED : 0) |
          (status == E2_EXPECTED ? E2_X_EXPECTED : 0) |
          (guest == E2_GUEST_DONE ? E2_X_GUEST_DONE | E2_X_SIGNATURE : 0));
    unsigned count = t->count <= E2_EXCEPTION_CAPACITY ? (unsigned)t->count : E2_EXCEPTION_CAPACITY;
    put32(p + 16, count); put32(p + 20, E2_FRAME_SIZE);
    put64(p + 24, t->dropped);
    const uint64_t header[] = {s->restore_mask, s->old_vbar, s->handler_vbar, s->old_daif,
        s->old_hcr, s->old_elr, s->old_spsr, s->old_esr, s->old_far, s->old_sp_el0,
        s->call_sp, s->host_spsr, s->expected_pc, s->resume_pc, s->abort_pc,
        s->last_vector, s->active, s->depth, s->old_sp_el1,
        /* Guest stage, reported separately from the BRK stage above. */
        s->guest_requested, guest, s->guest_verdict, s->hvc_count, s->guest_flags,
        s->guest_hcr, s->pre_brk_sp, s->old_sctlr12, s->signature_seen, s->old_esr12, s->old_far12};
    _Static_assert(32 + 8 * sizeof(header) / sizeof(header[0]) <= E2_EXCEPTION_HEADER_SIZE,
                   "result header fits");
    for (unsigned i = 0; i < sizeof(header) / sizeof(header[0]); i++) put64(p + 32 + 8u * i, header[i]);
    for (unsigned i = 0; i < count; i++) {
        const struct e2_exception_frame *f = &t->frame[i];
        uint8_t *q = p + E2_EXCEPTION_HEADER_SIZE + E2_FRAME_SIZE * i;
        for (unsigned j = 0; j < 31; j++) put64(q + 8u * j, f->x[j]);
        const uint64_t fields[] = {f->el2_sp, f->esr, f->elr, f->spsr, f->far,
            f->daif, f->hcr, f->vbar, f->mpidr, f->vector, f->depth, f->status,
            f->resume_elr, f->resume_spsr, f->sp_el0, f->flags, f->sp_el1};
        for (unsigned j = 0; j < sizeof(fields) / sizeof(fields[0]); j++) put64(q + 248 + 8u * j, fields[j]);
    }
    if (t->dropped) {
        uint8_t *q = p + E2_OVERFLOW_OFFSET;
        put32(q, E2_OVERFLOW_MAGIC); put32(q + 4, (uint32_t)E2_OVERFLOW_SIZE << 16 | E2_VERSION);
        const uint64_t fields[] = {t->overflow_esr,t->overflow_elr,t->overflow_spsr,
            t->overflow_far,t->overflow_vector,t->overflow_flags};
        for (unsigned i=0;i<6;i++) put64(q+8+8u*i,fields[i]);
    }
    /* A guest run answers with the guest result only once the BRK stage passed. */
    if (s->guest_requested && status == E2_EXPECTED) return (int)guest;
    return (int)status;
}
