/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "e0.h"
#ifdef E0_WORLDS
#include "worlds.h"
#endif

struct e0_state e0_state;
uint64_t e0_state_canary;
uint64_t e0_status, e0_restoring, e0_saved[EO_SIZE / 8];
struct e2_exception_session e2_exception_session;
struct e2_exception_trace e2_exception_trace;
_Static_assert(offsetof(struct e0_state, config) == E0_S_CONFIG, "assembly config offset");
_Static_assert(offsetof(struct e0_state, mode) == E0_S_MODE, "assembly mode offset");
_Static_assert(offsetof(struct e0_state, returned) == E0_S_RETURNED, "assembly returned offset");
extern struct e0_tables e0_tables;
extern uint8_t e2_vectors[], e0_seed_brk[], e0_seed_after[], e0_guest_return[];
extern uint8_t e2_exception_abort[], e0_after_probe[], e0_fault_pc[], e0_after_fault[], e0_after_done[];
#define READ(reg) ({ uint64_t v; __asm__ volatile("mrs %0, " #reg : "=r"(v)); v; })

#ifndef E0_WORLDS
static uint64_t guest_pc(const uint8_t *p)
{
    return E0_CODE_IPA + (uint64_t)((uintptr_t)p - (uintptr_t)e0_guest_vectors);
}
#endif

int e0_prepare(unsigned mode)
{
    e0_status = E0_ENVIRONMENT;
    e0_restoring = 0;
    /* Volatile byte clears keep freestanding initialization free of libc. */
    for (unsigned i = 0; i < sizeof(e0_state); i++) ((volatile uint8_t *)&e0_state)[i] = 0;
    for (unsigned i = 0; i < sizeof(e2_exception_session); i++) ((volatile uint8_t *)&e2_exception_session)[i] = 0;
    for (unsigned i = 0; i < sizeof(e2_exception_trace); i++) ((volatile uint8_t *)&e2_exception_trace)[i] = 0;
    e0_state.mode = mode;
    e0_state_canary = E0_CANARY;
    e0_state.mmfr0 = READ(ID_AA64MMFR0_EL1);
#ifdef E0_WORLDS
    if (mode < EW_PREPARE || mode > EW_DUAL) return E0_ENVIRONMENT;
#else
    if (mode > E0_STATE_DENY || e0_s2_build(&e0_tables, e0_state.mmfr0,
        (uint64_t)e0_arena_start, mode == E0_ALLOW || mode == E0_MISSING_DENIAL, &e0_state.config))
        return E0_ENVIRONMENT;
#endif
    /* QEMU owns this arena. Native allocation/exclusion proof is NOT supplied.
     * Require identity-mapped Normal-WB RAM for every page before using PA. */
    uint64_t old_par = READ(PAR_EL1);
    int identity = 1;
    for (uint64_t p = (uint64_t)e0_arena_start; p < (uint64_t)e0_arena_start + E0_ARENA_SIZE; p += E2_PAGE_SIZE) {
        __asm__ volatile("at s1e2r, %0; isb" :: "r"(p) : "memory");
        uint64_t par = READ(PAR_EL1);
        if ((par & 1) || ((par >> 56) != 0xff) ||
            (par & UINT64_C(0xfffffffff000)) != p) identity = 0;
    }
    __asm__ volatile("msr PAR_EL1, %0; isb" :: "r"(old_par) : "memory");
    if (!identity) return E0_ENVIRONMENT;
#ifdef E0_WORLDS
    if (mode < EW_PREPARE || ew_prepare(mode)) return E0_ENVIRONMENT;
#endif
    *(volatile uint64_t *)(e0_arena_start + E0_ALLOWED_PA) = 0;
    *(volatile uint64_t *)(e0_arena_start + E0_GUARD_PA) = E0_GUARD_VALUE;
    unsigned line = 4u << ((READ(CTR_EL0) >> 16) & 15);
    /* Dedicated RAM only. Guest stage1/caches are off: publish code/tables and
     * remove EL2 dirty aliases before the noncacheable guest accesses. */
    for (uint64_t p = (uint64_t)e0_arena_start; p < (uint64_t)e0_arena_start + E0_ARENA_SIZE; p += line)
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
    struct e2_exception_session *s = &e2_exception_session;
    s->handler_vbar = (uint64_t)e2_vectors;
    s->call_sp = (uint64_t)__e2_stack_end;
    s->abort_pc = (uint64_t)e2_exception_abort;
    s->active = 1;
    e0_status = E0_OK;
    return E0_OK;
}

/* One bounded helper call, no I/O/heap/printf. This replaces only the C
 * dispatcher in the separate E0 ELF; validated vectors.S is linked unchanged. */
int e2_exception_dispatch(struct e2_exception_frame *f,
                          struct e2_exception_session *s, struct e2_exception_trace *t)
{
#ifdef E0_WORLDS
    (void)t;
    return ew_dispatch(f, s);
#else
    uint64_t hp = READ(HPFAR_EL2);
    f->flags = 0;
    unsigned ec = (unsigned)((f->esr >> 26) & 63);
    if ((f->vector & 3) == 0 || (f->vector & 3) == 3) f->flags |= E2_F_ESR_VALID;
    if ((f->vector & 3) == 0 && (ec == 0x20 || ec == 0x21 || ec == 0x24 || ec == 0x25) && !(f->esr & (1u << 10))) f->flags |= E2_F_FAR_VALID;
    if (e0_restoring || !s->host_valid || f->depth != 1 || t->count >= 4) {
        if (t->dropped != UINT64_MAX) t->dropped++;
        t->overflow_esr = f->esr; t->overflow_elr = f->elr;
        t->overflow_spsr = f->spsr; t->overflow_far = f->far;
        t->overflow_vector = f->vector; t->overflow_flags = f->flags;
        e0_status = E0_CONTEXT_LOST;
        return E2_ACTION_DIRECT_ABORT;
    }
    /* Architectural validity is independent of whether the address was
     * expected. In particular, the wrong-address negative still has HPFAR. */
    if (f->vector == 8 && (ec == 0x20 || ec == 0x24) && ((f->esr & 63) == 7 || (f->esr & 63) == 6) &&
        !(f->esr & ((1u << 10) | (1u << 7))))
        e0_state.hpfar_valid |= UINT64_C(1) << t->count;
    int valid = 0, finish = 0;
    if (f->vector == 4 && e0_state.phase == 0 && f->elr == (uint64_t)e0_seed_brk &&
        f->esr == (UINT64_C(0x3c) << 26 | UINT64_C(1) << 25 | E2_BRK_IMM)) {
        e0_state.phase = 1;
        f->resume_elr = (uint64_t)e0_seed_after;
        valid = 1;
    } else if (f->vector == 8 && (f->spsr & (UINT64_C(1) << 20 | 0x3df)) == E2_GUEST_SPSR &&
               (f->hcr & 1) && f->vbar == s->handler_vbar) {
        if (ec == 0x16 && (f->esr & 0x1ffffff) == E2_HVC_PROBE &&
            e0_state.phase == 1 && f->elr == guest_pc(e0_after_probe) && f->x[0] == E0_VALUE) {
            e0_state.phase = 2;
            f->resume_elr = f->elr;
            valid = 1;
        } else if (e0_state.phase == 2 && e0_state.mode != E0_ALLOW &&
                   (e0_state.mode == E0_STATE_DENY ?
                    e0_state_fault_valid(f->esr, f->far, hp, f->elr, guest_pc(e0_fault_pc), (uint64_t)&e0_state_canary) :
                    e0_fault_valid(f->esr, f->far, hp, f->elr, guest_pc(e0_fault_pc)))) {
            e0_state.phase = 3;
            f->resume_elr = guest_pc(e0_after_fault);
            valid = 1;
        } else if (ec == 0x16 && (f->esr & 0x1ffffff) == E2_HVC_DONE &&
                   f->elr == guest_pc(e0_after_done) && !f->x[0] &&
                   e0_state.phase == (e0_state.mode == E0_ALLOW ? 2u : 3u)) {
            e0_state.phase = 4;
            f->resume_elr = (uint64_t)e0_guest_return;
            valid = finish = 1;
        }
    }
    if (!valid) {
        e0_status = E0_UNEXPECTED;
        f->resume_elr = (uint64_t)e2_exception_abort;
        finish = 1;
    }
    f->status = e0_status;
    f->resume_spsr = finish ? s->host_spsr : f->spsr;
    const uint8_t *from = (const uint8_t *)f;
    uint8_t *to = (uint8_t *)&t->frame[t->count];
    for (unsigned i = 0; i < E2_FRAME_SIZE; i++) to[i] = from[i];
    e0_state.hpfar[t->count++] = hp;
    if (valid && ec == 0x16 && !finish) f->x[0] = E2_HVC_REPLY;
    return E2_ACTION_ERET;
#endif
}
