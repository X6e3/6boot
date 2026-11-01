/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "worlds.h"
#include "worlds_build.h"

/* Deliberately scoped to these integer-only, interrupt-masked microguests.
 * Not a Linux/XNU context: no SIMD/SVE/SME, debug, PMU, PAC or guest timers. */
#define BANKS(X) \
    X(SP_EL0) X(SP_EL1) X(SCTLR_EL12) X(TCR_EL12) X(TTBR0_EL12) X(TTBR1_EL12) \
    X(MAIR_EL12) X(AMAIR_EL12) X(VBAR_EL12) X(ELR_EL12) X(SPSR_EL12) \
    X(ESR_EL12) X(FAR_EL12) X(CONTEXTIDR_EL12) X(TPIDR_EL1) X(TPIDR_EL0) X(TPIDRRO_EL0)
#define FIELD(reg) uint64_t reg;
struct ew_bank { BANKS(FIELD) };
#undef FIELD
struct ew_cpu { uint64_t x[31], pc, pstate; struct ew_bank bank; };
static struct ew_cpu cpu[2];
static struct ew_bank host;
static unsigned active, phase[2];
struct ew_trace ew_trace;
extern struct e0_tables e0_tables, ew_tables_b;
extern uint8_t ew_payload[], ew_after_yield[], ew_permission_pc[], ew_after_permission[];
extern uint8_t ew_foreign_pc[], ew_after_foreign[], ew_after_done[];
extern uint8_t e0_guest_return[], e0_seed_brk[], e2_vectors[];
#define READ(reg) ({ uint64_t v; __asm__ volatile("mrs %0, " #reg : "=r"(v)); v; })

static void save_bank(struct ew_bank *b)
{
#define SAVE(reg) b->reg = READ(reg);
    BANKS(SAVE)
#undef SAVE
}

static int load_bank(const struct ew_bank *b)
{
    /* Stage1 is off throughout. No live TTBR replacement or ASID reuse. */
#define LOAD(reg) do { \
    __asm__ volatile("msr " #reg ", %0; isb" :: "r"(b->reg) : "memory"); \
    if (READ(reg) != b->reg) return E0_RESTORE_FAILED; \
} while (0);
    BANKS(LOAD)
#undef LOAD
    return E0_OK;
}

static uint64_t pc(const uint8_t *p)
{
    return E0_CODE_IPA + (uint64_t)((uintptr_t)p - (uintptr_t)e0_guest_vectors);
}

int ew_prepare(unsigned mode)
{
    for (unsigned i = 0; i < sizeof(ew_trace); i++) ((volatile uint8_t *)&ew_trace)[i] = 0;
    ew_trace.magic = UINT64_C(0x31573045); /* E0W1, little endian */
    ew_trace.version = 1; ew_trace.size = sizeof(ew_trace); ew_trace.build = EW_BUILD_ID;
    ew_trace.mode = mode; ew_trace.status = E0_ENVIRONMENT;
    ew_trace.mmfr0 = READ(ID_AA64MMFR0_EL1); ew_trace.mmfr1 = READ(ID_AA64MMFR1_EL1);
    ew_trace.mmfr2 = READ(ID_AA64MMFR2_EL1); ew_trace.pfr0 = READ(ID_AA64PFR0_EL1);
    ew_trace.hcr = READ(HCR_EL2);
    save_bank(&host);
    uint64_t base = (uint64_t)e0_arena_start;
    unsigned inherited_vmid = (unsigned)(READ(VTTBR_EL2) >> 48);
    if (inherited_vmid == EW_VMID_A || inherited_vmid == EW_VMID_B) {
        ew_trace.refusal = 6; return E0_ENVIRONMENT;
    }
    if (base > (UINT64_C(1) << 32) - E0_ARENA_SIZE) {
        /* Numeric foreign-PA-as-IPA test is QEMU-specific. High native PA
         * needs a different negative probe, not a larger T0SZ by default. */
        ew_trace.refusal = 7; return E0_ENVIRONMENT;
    }
    if (mode < EW_PREPARE || mode > EW_DUAL ||
        !ew_capable(ew_trace.mmfr1, ew_trace.mmfr2, ew_trace.pfr0) ||
        ew_build(&e0_tables, ew_trace.mmfr0, base, 0, EW_VMID_A, &ew_trace.domain[0]) ||
        ew_build(&ew_tables_b, ew_trace.mmfr0, base, 1, EW_VMID_B, &ew_trace.domain[1]) ||
        ew_pair(ew_trace.domain)) {
        ew_trace.refusal = 1; return E0_ENVIRONMENT;
    }
    /* This preparation is callable only in the diskless harness's owned
     * arena. Native ownership, PE and VMID reservation are NOT established. */
    for (unsigned i = 0; i < E2_PAGE_SIZE; i++)
        ((volatile uint8_t *)(base + EW_CODE_B))[i] = e0_guest_vectors[i];
    for (unsigned w = 0; w < 2; w++) {
        phase[w] = 0;
        for (unsigned i = 0; i < 31; i++) cpu[w].x[i] = ew_trace.domain[w].signature + i;
        cpu[w].pc = pc(ew_payload); cpu[w].pstate = E2_GUEST_SPSR;
        cpu[w].bank = (struct ew_bank){.SP_EL0 = EW_STACK_IPA + 0x2000,
            .SP_EL1 = EW_STACK_IPA + E2_PAGE_SIZE, .SCTLR_EL12 = E2_GUEST_SCTLR,
            .VBAR_EL12 = E0_CODE_IPA, .SPSR_EL12 = E2_GUEST_SPSR,
            .CONTEXTIDR_EL12 = 0x61 + w, .TPIDR_EL1 = 0x71 + w,
            .TPIDR_EL0 = 0x81 + w, .TPIDRRO_EL0 = 0x91 + w};
        const uint64_t pages[] = {ew_trace.domain[w].stack, ew_trace.domain[w].data, ew_trace.domain[w].ro};
        for (unsigned j = 0; j < 3; j++) for (unsigned i = 0; i < E2_PAGE_SIZE / 8; i++)
            ((volatile uint64_t *)(uintptr_t)pages[j])[i] = 0;
        *(volatile uint64_t *)(uintptr_t)ew_trace.domain[w].ro = E0_GUARD_VALUE;
        /* The QEMU arena lies below 4 GiB. This numeric PA-as-IPA probe
         * intentionally hits an invalid L2 entry, not a hidden identity map. */
        ((volatile uint64_t *)(uintptr_t)ew_trace.domain[w].ro)[1] = ew_trace.domain[1-w].data;
    }
    active = 0; ew_trace.status = E0_OK;
    e0_state.config = (struct e0_config){ew_trace.domain[0].vtcr,
        ew_trace.domain[0].root | ew_trace.domain[0].vmid << 48};
    return E0_OK;
}

static int select_world(unsigned w, struct e2_exception_frame *f)
{
    struct ew_domain *d = &ew_trace.domain[w];
    struct e0_tables *t = w ? &ew_tables_b : &e0_tables;
    if (ew_validate(t, (uint64_t)e0_arena_start, w, d) || ew_pair(ew_trace.domain)) {
        ew_trace.refusal = 3;
        return E0_CONTEXT_LOST;
    }
    uint64_t hcr = READ(HCR_EL2) & ~((uint64_t)E2_GUEST_HCR_CLEAR);
    e0_restoring = 1; /* A control-write trap cannot retry a half switch. */
    __asm__ volatile("msr HCR_EL2, %0; isb" :: "r"(hcr) : "memory");
    if (ew_trace.tlbis) {
        __asm__ volatile("dsb ish; tlbi vmalls12e1is; dsb ish; isb" ::: "memory");
        ew_trace.tlbis++;
    }
    uint64_t vttbr = d->root | d->vmid << 48;
    __asm__ volatile("msr VTCR_EL2, %0; msr VTTBR_EL2, %1; isb; dsb ishst; "
                     "tlbi vmalls12e1is; dsb ish; isb" :: "r"(d->vtcr), "r"(vttbr) : "memory");
    if (READ(HCR_EL2) != hcr || READ(VTCR_EL2) != d->vtcr || READ(VTTBR_EL2) != vttbr) {
        ew_trace.refusal = 4; return E0_CONTEXT_LOST;
    }
    if (load_bank(&cpu[w].bank)) { ew_trace.refusal = 5; return E0_CONTEXT_LOST; }
    hcr |= 1;
    __asm__ volatile("msr HCR_EL2, %0; isb" :: "r"(hcr) : "memory");
    if (READ(HCR_EL2) != hcr) { ew_trace.refusal = 4; return E0_CONTEXT_LOST; }
    ew_trace.tlbis++;
    for (unsigned i = 0; i < 31; i++) f->x[i] = cpu[w].x[i];
    f->resume_elr = cpu[w].pc; f->resume_spsr = cpu[w].pstate;
    e0_restoring = 0; active = w;
    return E0_OK;
}

int ew_restore_host(void)
{
    int ret = load_bank(&host);
    if (ret) return ret;
    /* entry.S reached here only with VM=0 and after flushing the last test
     * VMID (or PREPARE never entered). Earlier domains were flushed on exit.
     * Retire both fixed slots before releasing this invocation to its caller. */
    for (unsigned i = 0; i < 2048; i++) {
        ((volatile uint64_t *)e0_tables.l2)[i] = 0;
        ((volatile uint64_t *)e0_tables.l3)[i] = 0;
        ((volatile uint64_t *)ew_tables_b.l2)[i] = 0;
        ((volatile uint64_t *)ew_tables_b.l3)[i] = 0;
    }
    __asm__ volatile("dsb ishst" ::: "memory");
    return E0_OK;
}

static int finish(struct e2_exception_frame *f, struct e2_exception_session *s, unsigned status)
{
    e0_status = status; ew_trace.status = status;
    f->resume_elr = (uint64_t)e0_guest_return; f->resume_spsr = s->host_spsr;
    return E2_ACTION_ERET;
}

int ew_dispatch(struct e2_exception_frame *f, struct e2_exception_session *s)
{
    if (e0_restoring || !s->host_valid || f->depth != 1 || ew_trace.count >= EW_EVENTS) {
        /* The same fixed emergency record used by the original E0 observer.
         * It remains usable even when the new world trace is full. */
        struct e2_exception_trace *t = &e2_exception_trace;
        if (t->dropped != UINT64_MAX) t->dropped++;
        t->overflow_esr = f->esr; t->overflow_elr = f->elr;
        t->overflow_spsr = f->spsr; t->overflow_far = f->far;
        t->overflow_vector = f->vector; t->overflow_flags = 0;
        ew_trace.refusal = 2;
        return E2_ACTION_DIRECT_ABORT;
    }
    if (f->vector == 4 && e0_state.phase == 0 && f->elr == (uint64_t)e0_seed_brk &&
        f->esr == ((UINT64_C(0x3c) << 26) | (1u << 25) | E2_BRK_IMM)) {
        e0_state.phase = 1;
        if (ew_trace.mode == EW_PREPARE) return finish(f, s, E0_OK);
        return select_world(0, f) ? E2_ACTION_DIRECT_ABORT : E2_ACTION_ERET;
    }
    struct ew_domain *d = &ew_trace.domain[active];
    struct ew_event *e = &ew_trace.event[ew_trace.count++];
    *e = (struct ew_event){.world = active, .kind = EW_EVENT_UNEXPECTED,
        .vmid = d->vmid, .root = READ(VTTBR_EL2), .hcr = f->hcr, .vtcr = READ(VTCR_EL2),
        .esr = f->esr, .elr = f->elr, .spsr = f->spsr, .far = f->far,
        .hpfar = READ(HPFAR_EL2), .tlbis = ew_trace.tlbis};
    uint64_t expected_hcr = (ew_trace.hcr & ~((uint64_t)E2_GUEST_HCR_CLEAR)) | 1;
    if (f->vector != 8 || (f->spsr & 0x1003df) != E2_GUEST_SPSR ||
        f->hcr != expected_hcr || READ(SCTLR_EL12) != E2_GUEST_SCTLR ||
        f->vbar != (uint64_t)e2_vectors ||
        e->root != (d->root | d->vmid << 48) || e->vtcr != d->vtcr)
        return finish(f, s, E0_UNEXPECTED);
    uint64_t expected_pc = phase[active] == 1 ? pc(ew_permission_pc) : pc(ew_foreign_pc);
    uint64_t address = phase[active] == 1 ? E0_DENIED_IPA : ew_trace.domain[1-active].data;
    unsigned kind = phase[active] == 1 ? EW_EVENT_PERMISSION : EW_EVENT_FOREIGN;
    if ((phase[active] == 1 || phase[active] == 2) &&
        ew_fault(f->esr, f->far, e->hpfar, f->elr, expected_pc, address, kind, &e->ipa)) {
        e->kind = kind; e->validity = kind == EW_EVENT_PERMISSION ? 5 : 7;
        e->expected = 1; phase[active]++;
#ifdef E0_VISUAL
        ew_visual_event(active, kind);
#endif
        f->resume_elr = pc(kind == EW_EVENT_PERMISSION ? ew_after_permission : ew_after_foreign);
        f->resume_spsr = f->spsr;
        return E2_ACTION_ERET;
    }
    if ((f->esr >> 26) != 0x16 || !(f->esr & (1u << 25))) return finish(f, s, E0_UNEXPECTED);
    unsigned imm = (unsigned)(f->esr & 0x1ffffff);
    if (phase[active] == 0 && imm == EW_HVC_YIELD && f->elr == pc(ew_after_yield)) {
        for (unsigned i = 0; i < 31; i++) if (f->x[i] != d->signature + i)
            return finish(f, s, E0_UNEXPECTED);
        if (f->sp_el1 != EW_STACK_IPA + E2_PAGE_SIZE || f->sp_el0 != EW_STACK_IPA + 0x2000)
            return finish(f, s, E0_UNEXPECTED);
        for (unsigned i = 0; i < 31; i++) cpu[active].x[i] = f->x[i];
        cpu[active].pc = f->elr; cpu[active].pstate = f->spsr;
        save_bank(&cpu[active].bank);
        e->kind = EW_EVENT_YIELD; e->expected = e->context_ok = 1;
#ifdef E0_VISUAL
        ew_visual_event(active, EW_EVENT_YIELD);
#endif
        phase[active] = 1;
        if (ew_trace.mode == EW_DUAL && active == 0)
            return select_world(1, f) ? E2_ACTION_DIRECT_ABORT : E2_ACTION_ERET;
        /* A self switch in SINGLE/B tests restoration of the complete bank
         * too. A resumes only after B has terminated. */
        return select_world(active, f) ? E2_ACTION_DIRECT_ABORT : E2_ACTION_ERET;
    }
    if (phase[active] == 3 && imm == EW_HVC_DONE && f->elr == pc(ew_after_done) && !f->x[0]) {
        struct ew_bank now;
        save_bank(&now);
#define CHECK(reg) if (now.reg != cpu[active].bank.reg) return finish(f, s, E0_UNEXPECTED);
        BANKS(CHECK)
#undef CHECK
        d->completion = d->context_ok = 1;
        e->kind = EW_EVENT_DONE; e->expected = e->context_ok = 1; phase[active] = 4;
        if (ew_trace.mode == EW_DUAL && active == 1)
            return select_world(0, f) ? E2_ACTION_DIRECT_ABORT : E2_ACTION_ERET;
        return finish(f, s, E0_OK);
    }
    return finish(f, s, E0_UNEXPECTED);
}
