/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "el2_exception.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BASE UINT64_C(0x100000000)
static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static struct e2_exception_session s;
static struct e2_exception_frame f;
static struct e2_exception_trace t;
static void fixture(void)
{
    memset(&s, 0, sizeof(s)); memset(&f, 0, sizeof(f)); memset(&t, 0, sizeof(t));
    s.handler_vbar = BASE + 0x2000; s.expected_pc = BASE + 0x100;
    s.resume_pc = s.expected_pc + 4; s.abort_pc = BASE + 0x120;
    s.old_hcr = E2_HCR_E2H | E2_HCR_TGE | E2_HCR_RW;
    s.old_daif = E2_DAIF_MASK; s.old_vbar = BASE - 0x2000;
    s.old_elr = 0x11223344; s.old_spsr = 0x55667788;
    s.old_esr = 0x1234; s.old_far = 0xabcdef;
    s.old_sp_el0 = 0x345600; s.old_sp_el1 = 0x789000;
    s.active = 1; s.status = E2_NOT_TAKEN; s.installed = 1;
    f.el2_sp = BASE + E2_STACK_OFFSET + E2_PAGE_SIZE - 112;
    f.esr = UINT64_C(0x3c) << 26 | UINT64_C(1) << 25 | E2_BRK_IMM;
    f.elr = s.expected_pc;
    /* Keep NZCV, PAN, UAO, DIT and SSBS intact across the modeled ERET. */
    f.spsr = UINT64_C(0xa0000000) | UINT64_C(1) << 22 | UINT64_C(1) << 23 |
             UINT64_C(1) << 24 | UINT64_C(1) << 12 | E2_DAIF_MASK | 9;
    f.far = 0xdeadbeef; f.daif = E2_DAIF_MASK; f.hcr = s.old_hcr;
    f.vbar = s.handler_vbar; f.mpidr = 0x80000000;
    f.vector = 4; f.depth = 1; f.sp_el0 = s.old_sp_el0; f.sp_el1 = s.old_sp_el1;
    for (unsigned i = 0; i < 31; i++) f.x[i] = UINT64_C(0x1234567800000000) + i;
}
static int dispatch(void) { return e2_exception_dispatch(&f, &s, &t); }
static void expected(void)
{
    fixture(); struct e2_exception_frame before = f;
    CHECK(dispatch() == E2_ACTION_ERET);
    CHECK(s.status == E2_EXPECTED && f.status == E2_EXPECTED);
    CHECK(t.count == 1 && t.dropped == 0 && s.eret_planned == 1);
    CHECK(f.resume_elr == s.expected_pc + 4 && f.resume_spsr == before.spsr);
    CHECK(s.host_valid == 1 && s.host_spsr == before.spsr);
    CHECK(f.flags == E2_F_ESR_VALID); /* FAR is stale for BRK. */
    CHECK(memcmp(f.x, before.x, sizeof(f.x)) == 0);
    CHECK(memcmp(&f, &t.frame[0], sizeof(f)) == 0);
    CHECK(f.sp_el0 == before.sp_el0 && f.sp_el1 == before.sp_el1);
}
static void unexpected(void)
{
    fixture(); f.esr ^= 1; CHECK(dispatch() == E2_ACTION_ERET);
    CHECK(s.status == E2_UNEXPECTED && f.resume_elr == s.abort_pc);
    CHECK(f.resume_elr != f.elr + 4 && f.resume_spsr == f.spsr);
    fixture(); f.elr += 4; CHECK(dispatch() == E2_ACTION_ERET);
    CHECK(s.status == E2_UNEXPECTED && f.resume_elr == s.abort_pc);
    fixture(); f.esr &= ~(UINT64_C(1) << 25); CHECK(dispatch() == E2_ACTION_ERET);
    CHECK(s.status == E2_UNEXPECTED);
    fixture(); f.hcr ^= 1; CHECK(dispatch() == E2_ACTION_ERET); CHECK(s.status == E2_UNEXPECTED);
    fixture(); f.vbar += 0x800; CHECK(dispatch() == E2_ACTION_ERET); CHECK(s.status == E2_UNEXPECTED);
    fixture(); f.daif = 0; CHECK(dispatch() == E2_ACTION_ERET); CHECK(s.status == E2_UNEXPECTED);
}
static void all_vectors(void)
{
    for (unsigned vector = 0; vector < 16; vector++) {
        fixture(); f.vector = vector; CHECK(dispatch() == E2_ACTION_ERET);
        CHECK(s.status == (vector == 4 ? E2_EXPECTED : E2_UNEXPECTED));
        CHECK(t.frame[0].vector == vector);
        CHECK(f.flags == ((vector % 4 == 0 || vector % 4 == 3) ? E2_F_ESR_VALID : 0));
    }
    fixture(); f.vector = 16; CHECK(dispatch() == E2_ACTION_ERET); CHECK(s.status == E2_UNEXPECTED);
    CHECK(f.flags == 0);
}
static void far_validity(void)
{
    const unsigned ec[] = {0x20, 0x21, 0x24, 0x25};
    for (unsigned i = 0; i < 4; i++) {
        fixture(); f.esr = (uint64_t)ec[i] << 26;
        CHECK(dispatch() == E2_ACTION_ERET);
        CHECK(f.flags == (E2_F_ESR_VALID | E2_F_FAR_VALID));
        fixture(); f.esr = (uint64_t)ec[i] << 26 | (1u << 10);
        CHECK(dispatch() == E2_ACTION_ERET); CHECK(f.flags == E2_F_ESR_VALID);
    }
    fixture(); f.vector = 5; CHECK(dispatch() == E2_ACTION_ERET); CHECK(f.flags == 0);
}
static void host_context(void)
{
    fixture(); f.spsr = 5 | E2_DAIF_MASK; CHECK(dispatch() == E2_ACTION_DIRECT_ABORT);
    CHECK(s.status == E2_CONTEXT_LOST && s.eret_planned == 0);
    CHECK(f.resume_elr == 0 && f.resume_spsr == 0);
    fixture(); s.host_valid = 1; s.host_spsr = f.spsr; uint64_t host = s.host_spsr;
    f.spsr = 5 | E2_DAIF_MASK; f.vector = 8;
    CHECK(dispatch() == E2_ACTION_ERET); CHECK(s.status == E2_UNEXPECTED);
    CHECK(f.resume_spsr == host && f.resume_elr == s.abort_pc);
    fixture(); f.spsr |= UINT64_C(1) << 20; CHECK(dispatch() == E2_ACTION_DIRECT_ABORT);
    CHECK(s.status == E2_CONTEXT_LOST); /* Illegal-return state never replayed. */
    fixture(); f.spsr &= ~(UINT64_C(1) << 7); CHECK(dispatch() == E2_ACTION_DIRECT_ABORT);
    fixture(); s.active = 0; CHECK(dispatch() == E2_ACTION_DIRECT_ABORT);
    CHECK(s.status == E2_CONTEXT_LOST);
    fixture(); s.host_valid = 1; s.host_spsr = 5 | E2_DAIF_MASK;
    CHECK(dispatch() == E2_ACTION_DIRECT_ABORT); CHECK(s.status == E2_CONTEXT_LOST);
}
static void stack_and_targets(void)
{
    const uint64_t bad[] = {BASE + E2_STACK_OFFSET, BASE + E2_STACK_OFFSET + E2_PAGE_SIZE + 16,
                           BASE + E2_STACK_OFFSET + E2_PAGE_SIZE - 1};
    for (unsigned i = 0; i < 3; i++) {
        fixture(); f.el2_sp = bad[i]; CHECK(dispatch() == E2_ACTION_ERET);
        CHECK(s.status == E2_BAD_STACK && f.resume_elr == s.abort_pc);
    }
    fixture(); f.el2_sp = BASE + E2_STACK_OFFSET + E2_FRAME_SIZE + E2_HANDLER_C_BUDGET;
    CHECK(dispatch() == E2_ACTION_ERET); CHECK(s.status == E2_EXPECTED);
    fixture(); s.abort_pc = BASE + E2_STATE_OFFSET; CHECK(dispatch() == E2_ACTION_DIRECT_ABORT);
    fixture(); s.resume_pc++; CHECK(dispatch() == E2_ACTION_DIRECT_ABORT);
    fixture(); s.expected_pc = UINT64_MAX; CHECK(dispatch() == E2_ACTION_DIRECT_ABORT);
    fixture(); s.handler_vbar++; CHECK(dispatch() == E2_ACTION_DIRECT_ABORT);
}
static void nested_and_capacity(void)
{
    fixture(); s.host_valid = 1; s.host_spsr = f.spsr; f.depth = 2;
    CHECK(dispatch() == E2_ACTION_ERET); CHECK(s.status == E2_NESTED);
    CHECK(f.resume_elr == s.abort_pc && t.frame[0].depth == 2);
    fixture(); s.host_valid = 1; s.host_spsr = f.spsr;
    t.count = E2_EXCEPTION_CAPACITY; struct e2_exception_trace before = t;
    CHECK(dispatch() == E2_ACTION_ERET); CHECK(s.status == E2_TRACE_FULL);
    CHECK(t.count == E2_EXCEPTION_CAPACITY && t.dropped == 1);
    CHECK(memcmp(t.frame, before.frame, sizeof(t.frame)) == 0);
    CHECK(f.resume_elr == s.abort_pc);
    t.dropped = UINT64_MAX;
    CHECK(dispatch() == E2_ACTION_ERET); CHECK(t.dropped == UINT64_MAX);
    fixture(); t.count = UINT64_MAX; CHECK(dispatch() == E2_ACTION_ERET);
    CHECK(s.status == E2_TRACE_FULL);
}
static void put32(uint8_t *p, uint32_t v)
{ for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v)
{ for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get64(const uint8_t *p)
{ return get32(p) | (uint64_t)get32(p + 4) << 32; }
static void encoding(void)
{
    fixture(); CHECK(dispatch() == E2_ACTION_ERET);
    s.active = s.depth = 0; s.restore_mask = E2_R_ALL; s.returned = 1;
    uint8_t output[E2_EXCEPTION_RESULT_SIZE + 16]; memset(output, 0xa5, sizeof(output));
    CHECK(e2_exception_encode(output + 1, E2_EXCEPTION_RESULT_SIZE, &s, &t) == E2_EXPECTED);
    CHECK(get32(output + 1) == E2_EXCEPTION_MAGIC);
    CHECK(get32(output + 5) == ((uint32_t)E2_EXCEPTION_RESULT_SIZE << 16 | E2_EXCEPTION_VERSION));
    CHECK(get32(output + 9) == E2_EXPECTED && get32(output + 13) == 7);
    CHECK(get32(output + 17) == 1 && get32(output + 21) == E2_FRAME_SIZE);
    CHECK(get64(output + 33) == E2_R_ALL && get64(output + 41) == s.old_vbar);
    const uint8_t *frame = output + 1 + E2_EXCEPTION_HEADER_SIZE;
    for (unsigned i = 0; i < 31; i++) CHECK(get64(frame + 8u * i) == f.x[i]);
    CHECK(get64(frame + EF_SPSR) == f.spsr && get64(frame + EF_SP_EL1) == f.sp_el1);
    CHECK(output[0] == 0xa5 && output[E2_EXCEPTION_RESULT_SIZE + 1] == 0xa5);
    for (unsigned i = E2_EXCEPTION_HEADER_SIZE + E2_FRAME_SIZE; i < E2_EXCEPTION_RESULT_SIZE; i++)
        CHECK(output[i + 1] == 0);
    s.restore_mask &= ~((uint64_t)E2_R_VBAR);
    CHECK(e2_exception_encode(output, sizeof(output), &s, &t) == E2_RESTORE_FAILED);
    CHECK(!(get32(output + 12) & (E2_X_RESTORED | E2_X_EXPECTED)));
    s.restore_mask = E2_R_ALL; s.active = 1;
    CHECK(e2_exception_encode(output, sizeof(output), &s, &t) == E2_RESTORE_FAILED);
    s.active = 0; s.returned = 0;
    CHECK(e2_exception_encode(output, sizeof(output), &s, &t) == E2_RESTORE_FAILED);
    s.returned = 1; t.count = UINT64_MAX;
    CHECK(e2_exception_encode(output, sizeof(output), &s, &t) == E2_TRACE_FULL);
    CHECK(get32(output + 16) == E2_EXCEPTION_CAPACITY);
}
static void invalid_encoding_arguments(void)
{
    fixture(); uint8_t output[E2_EXCEPTION_RESULT_SIZE]; memset(output, 0xa5, sizeof(output));
    CHECK(e2_exception_encode(output, sizeof(output) - 1, &s, &t) == E2_ARGUMENT);
    CHECK(e2_exception_encode(output, sizeof(output), NULL, &t) == E2_ARGUMENT);
    CHECK(e2_exception_encode(output, sizeof(output), &s, NULL) == E2_ARGUMENT);
    CHECK(e2_exception_encode((void *)(UINTPTR_MAX - 16), sizeof(output), &s, &t) == E2_ARGUMENT);
    CHECK(e2_exception_encode(&s, sizeof(output), &s, &t) == E2_ARGUMENT);
    CHECK(e2_exception_encode(&t, sizeof(output), &s, &t) == E2_ARGUMENT);
    for (unsigned i = 0; i < sizeof(output); i++) CHECK(output[i] == 0xa5);
}
/* ---- EL1 guest / HVC stage (synthetic frames; no EL1 or EL2 execution) ---- */
#define HVC(imm) (UINT64_C(0x16) << 26 | UINT64_C(1) << 25 | (uint64_t)(imm))
static void guest_fixture(void)
{
    fixture(); /* the BRK stage has passed and its frame is in the trace */
    CHECK(dispatch() == E2_ACTION_ERET && s.status == E2_EXPECTED && t.count == 1);
    s.guest_requested = 1; s.guest_active = 1;
    s.guest_hcr = (s.old_hcr & ~(uint64_t)E2_GUEST_HCR_CLEAR) | E2_HCR_RW | E2_HCR_E2H;
    s.guest_probe_pc = BASE + 0x2904; s.guest_done_pc = BASE + 0x2a08;
    s.guest_signature_pc = BASE + 0x29f0;
    s.guest_return_pc = BASE + 0x140;
    s.guest_flags = E2_GF_PREPARING | E2_GF_PAR_SAVED | E2_GF_EL12_SAVED | E2_GF_HCR_WRITTEN | E2_GF_ENTERED;
    memset(&f, 0, sizeof(f));
    f.el2_sp = BASE + E2_STACK_OFFSET + E2_PAGE_SIZE - 112;
    f.esr = HVC(E2_HVC_PROBE); f.elr = s.guest_probe_pc;
    f.spsr = E2_GUEST_SPSR; f.daif = E2_DAIF_MASK; f.hcr = s.guest_hcr; f.vbar = s.handler_vbar;
    f.vector = 8; f.depth = 1;
    for (unsigned i = 0; i < 31; i++) f.x[i] = E2_GUEST_PATTERN + i;
}
static void accept_signature(void)
{
    f.esr = HVC(E2_HVC_SIGNATURE); f.elr = s.guest_signature_pc;
    f.x[0] = E2_HVC_REPLY;
    CHECK(dispatch() == E2_ACTION_ERET && s.guest_status == E2_GUEST_SIGNATURE);
    CHECK(f.x[0] == E2_SIGNATURE_REPLY && t.frame[2].x[0] == E2_HVC_REPLY);
    CHECK(s.hvc_count == 2 && s.signature_seen == 1 && t.count == 3);
}
static void guest_probe_and_done(void)
{
    guest_fixture(); struct e2_exception_frame before = f;
    CHECK(dispatch() == E2_ACTION_ERET);
    CHECK(f.status == E2_GUEST_HVC && s.guest_status == E2_GUEST_HVC);
    CHECK(s.status == E2_EXPECTED); /* the BRK stage result is not overwritten */
    /* HVC returns to ELR as taken: it already points past the HVC. */
    CHECK(f.resume_elr == before.elr && f.resume_spsr == before.spsr);
    CHECK(f.x[0] == E2_HVC_REPLY && t.frame[1].x[0] == E2_GUEST_PATTERN);
    for (unsigned i = 1; i < 31; i++) CHECK(f.x[i] == before.x[i]);
    CHECK(s.hvc_count == 1 && s.guest_active == 1 && t.count == 2);
    accept_signature();
    f = before; f.esr = HVC(E2_HVC_DONE); f.elr = s.guest_done_pc; f.x[0] = 0;
    CHECK(dispatch() == E2_ACTION_ERET);
    CHECK(s.guest_status == E2_GUEST_DONE && f.resume_elr == s.guest_return_pc);
    CHECK(f.resume_spsr == s.host_spsr && (f.resume_spsr & 0x1f) == 9);
    CHECK(s.hvc_count == 3 && s.guest_active == 0 && s.guest_verdict == 0 && t.count == 4);
    CHECK(s.status == E2_EXPECTED);
}
static void guest_mismatch(void)
{
    guest_fixture(); CHECK(dispatch() == E2_ACTION_ERET);
    accept_signature();
    f.esr = HVC(E2_HVC_DONE); f.elr = s.guest_done_pc; f.x[0] = 1;
    CHECK(dispatch() == E2_ACTION_ERET);
    CHECK(s.guest_status == E2_GUEST_MISMATCH && f.resume_elr == s.guest_return_pc);
    CHECK(f.resume_spsr == s.host_spsr && s.guest_verdict == 1 && s.guest_active == 0);
}
static void guest_rejections(void)
{
    for (unsigned c = 0; c < 14; c++) {
        guest_fixture();
        switch (c) {
        case 0: f.esr = HVC(E2_HVC_FAULT_BASE + 3); break;  /* guest took an EL1 exception */
        case 1: f.esr = HVC(E2_HVC_STRAY); break;
        case 2: f.elr += 4; break;                           /* ELR adjusted by someone */
        case 3: f.elr -= 4; break;                           /* ELR at the HVC itself */
        case 4: f.esr = HVC(E2_HVC_DONE); f.elr = s.guest_done_pc; f.x[0] = 0; break; /* DONE first */
        case 5: f.spsr = E2_DAIF_MASK | 4; break;           /* EL1t */
        case 6: f.spsr = 5; break;                          /* EL1h, interrupts unmasked */
        case 7: f.vector = 9; break;                        /* IRQ from EL1 */
        case 8: f.hcr |= E2_HCR_TGE; break;                 /* HCR is not the guest value */
        case 9: f.x[17] ^= 1; break;                        /* seeds changed on the way in */
        case 10: f.depth = 2; break;
        case 11: t.count = 2; break;                        /* history out of order */
        case 12: s.guest_probe_pc = BASE + E2_PAGE_SIZE; f.elr = s.guest_probe_pc; break;
        case 13: f.esr = UINT64_C(0x17) << 26 | UINT64_C(1) << 25 | E2_HVC_PROBE; break; /* SMC64 */
        }
        CHECK(dispatch() == E2_ACTION_ERET);
        CHECK(s.guest_status == E2_GUEST_UNEXPECTED);
        /* Out through the host abort path: never back into the guest. */
        CHECK(f.resume_elr == s.abort_pc && f.resume_spsr == s.host_spsr);
        CHECK(s.guest_active == 0 && s.status == E2_EXPECTED);
    }
}
static void guest_context(void)
{
    guest_fixture(); s.host_valid = 0;
    CHECK(dispatch() == E2_ACTION_DIRECT_ABORT);
    CHECK(s.guest_status == E2_CONTEXT_LOST && s.guest_active == 0 && s.eret_planned == 0);
    guest_fixture(); s.guest_active = 0; s.guest_flags &= ~(uint64_t)E2_GF_PREPARING;
    CHECK(dispatch() == E2_ACTION_ERET);
    CHECK(s.status == E2_UNEXPECTED && f.resume_elr == s.abort_pc && f.resume_spsr == s.host_spsr);
}
static void guest_encoding(void)
{
    guest_fixture(); CHECK(dispatch() == E2_ACTION_ERET);
    accept_signature();
    f.esr = HVC(E2_HVC_DONE); f.elr = s.guest_done_pc; f.x[0] = 0;
    CHECK(dispatch() == E2_ACTION_ERET && s.guest_status == E2_GUEST_DONE);
    s.active = s.depth = 0; s.restore_mask = E2_R_ALL; s.returned = 1;
    uint8_t out[E2_EXCEPTION_RESULT_SIZE];
    CHECK(e2_exception_encode(out, sizeof(out), &s, &t) == E2_GUEST_DONE);
    CHECK(get32(out + 8) == E2_EXPECTED); /* the BRK stage is reported on its own */
    CHECK(get32(out + 12) == (E2_X_INSTALLED | E2_X_RESTORED | E2_X_EXPECTED | E2_X_GUEST_DONE | E2_X_SIGNATURE));
    CHECK(get32(out + 16) == 4);
    CHECK(get64(out + 248) == 1);
    CHECK(get64(out + 32 + 8 * 19) == 1 && get64(out + 32 + 8 * 20) == E2_GUEST_DONE);
    /* Any gap in the restore or in the history withdraws the guest claim. */
    s.restore_mask &= ~(uint64_t)E2_R_GUEST;
    CHECK(e2_exception_encode(out, sizeof(out), &s, &t) == E2_RESTORE_FAILED);
    CHECK(!(get32(out + 12) & E2_X_GUEST_DONE));
    s.restore_mask = E2_R_ALL; s.hvc_count = 2;
    CHECK(e2_exception_encode(out, sizeof(out), &s, &t) == E2_GUEST_UNEXPECTED);
    s.hvc_count = 3; s.guest_flags &= ~(uint64_t)E2_GF_ENTERED;
    CHECK(e2_exception_encode(out, sizeof(out), &s, &t) == E2_GUEST_UNEXPECTED);
    s.guest_flags |= E2_GF_ENTERED; s.guest_requested = 0;
    CHECK(e2_exception_encode(out, sizeof(out), &s, &t) == E2_RESTORE_FAILED);
    CHECK(get64(out + 32 + 8 * 20) == E2_GUEST_NOT_RUN);
}
static void signature_rejections(void)
{
    for (unsigned c = 0; c < 6; c++) {
        guest_fixture(); CHECK(dispatch() == E2_ACTION_ERET);
        f.esr = HVC(E2_HVC_SIGNATURE); f.elr = s.guest_signature_pc;
        switch(c) {
        case 0: s.hvc_count = 0; break;
        case 1: f.elr += 4; break;
        case 2: f.x[0] = 0; break;
        case 3: f.x[19] ^= 1; break;
        case 4: s.signature_seen = 1; break;
        case 5: f.esr = HVC(E2_HVC_DONE); f.elr = s.guest_done_pc; f.x[0] = 0; break;
        }
        CHECK(dispatch() == E2_ACTION_ERET);
        CHECK(s.guest_status == E2_GUEST_UNEXPECTED && s.status == E2_EXPECTED);
        CHECK(f.resume_elr == s.abort_pc && s.guest_active == 0);
    }
}
static void guest_setup_restore_faults(void)
{
    for (unsigned restoring = 0; restoring < 2; restoring++) {
        fixture(); CHECK(dispatch() == E2_ACTION_ERET);
        uint64_t saved = s.host_spsr;
        s.guest_requested = 1;
        s.guest_flags = E2_GF_PREPARING | (restoring ? E2_GF_RESTORING : 0);
        f.vector = 4; f.elr = BASE + 0x180; f.esr = 0;
        f.spsr ^= UINT64_C(1) << 22;
        CHECK(dispatch() == E2_ACTION_ERET);
        CHECK(s.status == E2_EXPECTED && s.guest_status == E2_GUEST_UNEXPECTED);
        CHECK(s.host_spsr == saved && f.resume_spsr == saved && f.resume_elr == s.abort_pc);
    }
}
static void guest_context_lost_encoding(void)
{
    guest_fixture(); s.host_valid = 0;
    CHECK(dispatch() == E2_ACTION_DIRECT_ABORT);
    s.restore_mask = E2_R_ALL & ~(uint64_t)E2_R_PSTATE;
    s.active = s.depth = s.returned = 0;
    uint8_t out[E2_EXCEPTION_RESULT_SIZE];
    CHECK(e2_exception_encode(out, sizeof(out), &s, &t) == E2_RESTORE_FAILED);
    CHECK(get64(out + 192) == E2_CONTEXT_LOST);
    CHECK(!(get32(out + 12) & (E2_X_RESTORED | E2_X_GUEST_DONE | E2_X_SIGNATURE)));
}
static void overflow_syndrome(void)
{
    guest_fixture(); CHECK(dispatch() == E2_ACTION_ERET); accept_signature();
    f.esr=HVC(E2_HVC_DONE); f.elr=s.guest_done_pc; f.x[0]=0;
    CHECK(dispatch() == E2_ACTION_ERET && t.count==E2_EXCEPTION_CAPACITY);
    struct e2_exception_frame history[E2_EXCEPTION_CAPACITY];
    memcpy(history,t.frame,sizeof(history));
    f.vector=4; f.esr=UINT64_C(0x02000000); f.elr=BASE+0x200; f.spsr=s.host_spsr;
    CHECK(dispatch() == E2_ACTION_ERET);
    CHECK(t.dropped==1 && t.overflow_esr==f.esr && t.overflow_elr==f.elr);
    CHECK(t.overflow_vector==4 && t.overflow_flags==E2_F_ESR_VALID);
    CHECK(memcmp(history,t.frame,sizeof(history))==0);
    s.active=s.depth=0; s.returned=1; s.restore_mask=E2_R_ALL & ~(uint64_t)E2_R_GUEST;
    uint8_t out[E2_EXCEPTION_RESULT_SIZE];
    CHECK(e2_exception_encode(out,sizeof(out),&s,&t)==E2_RESTORE_FAILED);
    CHECK(get32(out+E2_OVERFLOW_OFFSET)==E2_OVERFLOW_MAGIC);
    CHECK(get64(out+E2_OVERFLOW_OFFSET+8)==f.esr && get64(out+E2_OVERFLOW_OFFSET+16)==f.elr);
}
static void prepare_buffers_and_p3_stack(void)
{
    uint8_t req[112] = {0}, output[E2_EXCEPTION_RESULT_SIZE + 128];
    struct e2_snapshot snap = {8, 1, BASE - 0x1000, 0, BASE - 0x8000,
                              E2_HCR_E2H | E2_HCR_TGE | E2_HCR_RW, E2_DAIF_MASK, 0x80000000};
    put32(req, E2_REQUEST_MAGIC); put32(req + 4, E2_HEADER_SIZE << 16 | E2_VERSION);
    put32(req + 8, sizeof(req)); put32(req + 12, E2_PREFLIGHT); put32(req + 16, E2_CHIP);
    req[20] = 'J'; req[21] = '7'; req[22] = '1'; req[23] = '3';
    put32(req + 24, E2_REQUIRED_FLAGS); put64(req + 32, BASE); put64(req + 40, E2_ARENA_SIZE);
    put32(req + 48, 1); put32(req + 56, 1); put64(req + 64, snap.mpidr_el1);
    put64(req + 80, BASE - 0x100000); put64(req + 88, 0x200000);
    put64(req + 96, BASE - E2_PAGE_SIZE); put64(req + 104, E2_PAGE_SIZE);
    memset(output, 0xa5, sizeof(output));
    CHECK(e2_exception_prepare(req, sizeof(req), output, 128, &snap, BASE) == E2_ARGUMENT);
    for (unsigned i = 0; i < sizeof(output); i++) CHECK(output[i] == 0xa5);
    CHECK(e2_exception_prepare(req, sizeof(req), output, sizeof(output), &snap, BASE) == E2_READY);
    put64(req + 104, E2_PAGE_SIZE - 1);
    CHECK(e2_exception_prepare(req, sizeof(req), output, sizeof(output), &snap, BASE) == E2_ENVIRONMENT);
    CHECK(get32(output + 8) == E2_ENVIRONMENT && get32(output + 12) == E2_RESULT_PREFLIGHT_ONLY);
    put64(req + 104, E2_PAGE_SIZE); snap.active_sp = BASE + E2_STACK_OFFSET;
    CHECK(e2_exception_prepare(req, sizeof(req), output, sizeof(output), &snap, BASE) == E2_ENVIRONMENT);
    snap.active_sp = BASE - 0x1000;
    /* Tail alias: the old 128-byte preflight check alone would miss this. */
    memset(output, 0xa5, sizeof(output)); memcpy(output + 1024, req, sizeof(req));
    uint8_t before[sizeof(output)]; memcpy(before, output, sizeof(output));
    CHECK(e2_exception_prepare(output + 1024, sizeof(req), output,
                               sizeof(output), &snap, BASE) == E2_ARGUMENT);
    CHECK(memcmp(before, output, sizeof(output)) == 0);
}
int main(void)
{
    expected(); unexpected(); all_vectors(); far_validity(); host_context();
    stack_and_targets(); nested_and_capacity(); encoding(); invalid_encoding_arguments();
    prepare_buffers_and_p3_stack();
    guest_probe_and_done(); guest_mismatch(); guest_rejections(); guest_context(); guest_encoding();
    signature_rejections(); guest_setup_restore_faults(); guest_context_lost_encoding();
    overflow_syndrome();
    printf("PASS: 19 exception host scenarios, %u assertions (synthetic frames; no EL1/EL2 execution)\n", checks);
    return 0;
}
