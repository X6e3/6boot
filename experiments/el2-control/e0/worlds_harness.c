/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "worlds.h"
extern void test_exit(uint64_t) __attribute__((noreturn));
extern uint64_t test_call(uintptr_t, uint64_t, uint64_t, uint64_t, uint64_t);
extern uint64_t test_observed[16];
extern struct e0_tables e0_tables, ew_tables_b;
#ifndef E0_VISUAL_SMOKE
static void text(const char *s)
{
    register uint64_t x0 __asm__("x0") = 4;
    register const char *x1 __asm__("x1") = s;
    __asm__ volatile("hlt #0xf000" : "+r"(x0) : "r"(x1) : "memory");
}
static void snapshot(uint64_t *p)
{
#define GET(reg) __asm__ volatile("mrs %0, " #reg : "=r"(*p++))
    GET(VBAR_EL2); GET(HCR_EL2); GET(VTCR_EL2); GET(VTTBR_EL2); GET(HPFAR_EL2);
    GET(ELR_EL2); GET(SPSR_EL2); GET(ESR_EL2); GET(FAR_EL2); GET(DAIF); GET(PAR_EL1);
    GET(SP_EL0); GET(SP_EL1); GET(SCTLR_EL12); GET(TCR_EL12); GET(TTBR0_EL12);
    GET(TTBR1_EL12); GET(MAIR_EL12); GET(AMAIR_EL12); GET(VBAR_EL12); GET(ELR_EL12);
    GET(SPSR_EL12); GET(ESR_EL12); GET(FAR_EL12); GET(CONTEXTIDR_EL12);
    GET(TPIDR_EL1); GET(TPIDR_EL0); GET(TPIDRRO_EL0);
#undef GET
}
static void record(void)
{
    const uint8_t *p = (const uint8_t *)&ew_trace;
    text("E0W1 ");
    for (unsigned i = 0; i < sizeof(ew_trace); i++) {
        char b[] = {"0123456789abcdef"[p[i] >> 4], "0123456789abcdef"[p[i] & 15], 0};
        text(b);
    }
    text("\n");
}
#endif
void test_main(void)
{
#ifdef E0_VISUAL_SMOKE
    ew_visual_init();
    test_exit(ew_visual_smoke_test() ? 0 : 1);
#else
    /* Nonzero inherited controls make a zero/falsified restore readback a
     * real mismatch rather than an accidental match to QEMU reset state. */
    uint64_t vtcr = 0x80023560, vttbr = UINT64_C(3) << 48 | 0x40600000, hp = 0x123450;
    __asm__ volatile("msr VTCR_EL2, %0; msr VTTBR_EL2, %1; msr HPFAR_EL2, %2; isb"
                     :: "r"(vtcr), "r"(vttbr), "r"(hp) : "memory");
    /* These banks differ from both worlds; zero-initialized QEMU registers
     * would let an omitted host-bank restore hide behind a matching zero. */
    uint64_t ttbr0 = 0x40404000, ttbr1 = 0x40408000, mair = 0xff44;
    __asm__ volatile("msr TTBR0_EL12, %0; msr TTBR1_EL12, %1; msr MAIR_EL12, %2; isb"
                     :: "r"(ttbr0), "r"(ttbr1), "r"(mair) : "memory");
    uint64_t tp1 = 0x1111, tp0 = 0x2222, tpro = 0x3333, context = 0x4444;
    __asm__ volatile("msr TPIDR_EL1, %0; msr TPIDR_EL0, %1; msr TPIDRRO_EL0, %2; "
                     "msr CONTEXTIDR_EL12, %3; isb" :: "r"(tp1), "r"(tp0), "r"(tpro), "r"(context) : "memory");
#ifdef E0_VISUAL
    ew_visual_init();
    const unsigned modes[] = {EW_DUAL};
#else
    const unsigned modes[] = {EW_PREPARE, EW_SINGLE, EW_DUAL, EW_SINGLE, EW_DUAL};
#endif
    for (unsigned n = 0; n < sizeof(modes) / sizeof(modes[0]); n++) {
        uint64_t before[28], after[28];
        snapshot(before);
        uint64_t result = test_call((uintptr_t)sixboot_e0_run, modes[n], 0, 0, 0);
        snapshot(after);
        int good = result == 0 && ew_trace.status == 0 && ew_trace.host_restored == 1;
        for (unsigned i = 0; i < 28; i++) if (before[i] != after[i]) good = 0;
        for (unsigned i = 0; i < 12; i++) if (test_observed[i] != 0x712u + i) good = 0;
        if (test_observed[12] != test_observed[13]) good = 0;
        for (unsigned i = 0; i < 2048; i++)
            if (e0_tables.l2[i] || e0_tables.l3[i] || ew_tables_b.l2[i] || ew_tables_b.l3[i]) good = 0;
        for (unsigned w = 0; w < 2; w++) {
            struct ew_domain *d = &ew_trace.domain[w];
            __asm__ volatile("dc ivac, %0; dc ivac, %1; dsb sy" :: "r"(d->data), "r"(d->ro) : "memory");
            uint64_t want = modes[n] != EW_PREPARE && (w == 0 || modes[n] == EW_DUAL) ? d->signature : 0;
            d->observed = *(volatile uint64_t *)(uintptr_t)d->data;
            d->guard_observed = *(volatile uint64_t *)(uintptr_t)d->ro;
            if (d->observed != want || d->guard_observed != E0_GUARD_VALUE) good = 0;
        }
        ew_trace.observer_ok = (uint64_t)good;
#ifdef E0_VISUAL
        if (!ew_visual_finish()) good = 0;
#endif
        record();
        if (!good) { text("E0W_CASE_FAIL\n"); test_exit(1); }
        text("E0W_CASE_PASS host28_gpr_sp_private_ram\n");
    }
    text("E0W_QEMU_PASS\n"); test_exit(0);
#endif
}
