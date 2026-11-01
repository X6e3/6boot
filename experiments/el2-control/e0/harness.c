/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "e0.h"
extern void test_exit(uint64_t) __attribute__((noreturn));
extern uint64_t test_call(uintptr_t, uint64_t, uint64_t, uint64_t, uint64_t);
extern uint64_t test_observed[16];
static void text(const char *s)
{
    register uint64_t x0 __asm__("x0") = 4;
    register const char *x1 __asm__("x1") = s;
    __asm__ volatile("hlt #0xf000" : "+r"(x0) : "r"(x1) : "memory");
}
static void hex(uint64_t x)
{
    char b[19] = "0x";
    for (unsigned i = 0; i < 16; i++) b[2+i] = "0123456789abcdef"[(x >> (60-4*i)) & 15];
    b[18] = 0; text(b);
}
static void field(const char *name, uint64_t v) { text(name); hex(v); }
static void snapshot(uint64_t *p)
{
#define GET(reg) __asm__ volatile("mrs %0, " #reg : "=r"(*p++))
    GET(VBAR_EL2); GET(HCR_EL2); GET(VTCR_EL2); GET(VTTBR_EL2); GET(HPFAR_EL2);
    GET(ELR_EL2); GET(SPSR_EL2); GET(ESR_EL2); GET(FAR_EL2); GET(DAIF); GET(SP_EL1);
    GET(SCTLR_EL12); GET(VBAR_EL12); GET(ELR_EL12); GET(SPSR_EL12); GET(ESR_EL12); GET(FAR_EL12); GET(PAR_EL1);
#undef GET
}
void test_main(void)
{
    uint64_t vtcr = 0x80023560, vttbr = UINT64_C(3) << 48 | 0x40600000, hp = 0x123450;
    __asm__ volatile("msr VTCR_EL2, %0; msr VTTBR_EL2, %1; msr HPFAR_EL2, %2; isb" ::
                     "r"(vtcr), "r"(vttbr), "r"(hp) : "memory");
    const unsigned modes[] = {E0_ALLOW, E0_DENY, E0_ALLOW, E0_DENY, E0_WRONG_ADDRESS, E0_MISSING_DENIAL, E0_STATE_DENY};
    for (unsigned n = 0; n < sizeof(modes)/sizeof(modes[0]); n++) {
        unsigned mode = modes[n];
        uint64_t before[18], after[18];
        snapshot(before);
        uint64_t ret = test_call((uintptr_t)sixboot_e0_run, mode, 0, 0, 0);
        snapshot(after);
        int restored = 1, preserved = 1;
        for (unsigned i = 0; i < 18; i++) if (before[i] != after[i]) restored = 0;
        for (unsigned i = 0; i < 12; i++) if (test_observed[i] != 0x712u+i) preserved = 0;
        if (test_observed[12] != test_observed[13]) preserved = 0;
        volatile uint64_t *allowed = (volatile uint64_t *)(e0_arena_start + E0_ALLOWED_PA);
        volatile uint64_t *guard = (volatile uint64_t *)(e0_arena_start + E0_GUARD_PA);
        __asm__ volatile("dc ivac, %0; dc ivac, %1; dsb sy" :: "r"(allowed), "r"(guard) : "memory");
        field("CASE mode=", mode); field(" ret=",ret); field(" phase=",e0_state.phase);
        field(" restored=",(uint64_t)restored); field(" gpr_sp=",(uint64_t)preserved);
        field(" allowed=",*allowed); field(" guard=",*guard); field(" mmfr0=",e0_state.mmfr0);
        field(" monitor_canary=",e0_state_canary);
        field(" vtcr=",e0_state.config.vtcr); field(" vttbr=",e0_state.config.vttbr); text("\n");
        for (unsigned i = 0; i < e2_exception_trace.count && i < 4; i++) {
            struct e2_exception_frame *f = &e2_exception_trace.frame[i];
            field("TRACE vector=",f->vector); field(" esr=",f->esr); field(" far=",f->far);
            field(" hpfar=",e0_state.hpfar[i]); field(" hpfar_valid=",(e0_state.hpfar_valid >> i)&1);
            field(" elr=",f->elr); field(" spsr=",f->spsr); field(" flags=",f->flags);
            unsigned ec = (unsigned)((f->esr >> 26)&63);
            field(" ec=",ec);
            int abort = ec == 0x20 || ec == 0x21 || ec == 0x24 || ec == 0x25;
            field(" abort_syndrome_valid=",(uint64_t)abort);
            if (abort) {
                field(" dfsc=",f->esr&63); field(" wnr=",(f->esr >> 6)&1);
                field(" s1ptw=",(f->esr >> 7)&1); field(" fnv=",(f->esr >> 10)&1);
                if (ec == 0x24 || ec == 0x25) {
                    field(" isv=",(f->esr >> 24)&1);
                    if (f->esr & (UINT64_C(1) << 24)) {
                        field(" sas=",(f->esr >> 22)&3); field(" srt=",(f->esr >> 16)&31);
                        field(" sse=",(f->esr >> 21)&1); field(" sf=",(f->esr >> 15)&1);
                        field(" ar=",(f->esr >> 14)&1);
                    }
                }
            }
            text("\n");
        }
        if (e2_exception_trace.dropped) {
            field("OVERFLOW esr=",e2_exception_trace.overflow_esr);
            field(" elr=",e2_exception_trace.overflow_elr);
            field(" spsr=",e2_exception_trace.overflow_spsr);
            field(" far=",e2_exception_trace.overflow_far);
            field(" vector=",e2_exception_trace.overflow_vector);
            field(" flags=",e2_exception_trace.overflow_flags); text("\n");
        }
        int negative = mode == E0_WRONG_ADDRESS || mode == E0_MISSING_DENIAL;
        if (!restored || !preserved || e0_state_canary != E0_CANARY || *allowed != E0_VALUE ||
            ret != (uint64_t)(negative ? E0_UNEXPECTED : E0_OK) ||
            (!negative && (e0_state.phase != 4 || !e0_state.returned)) ||
            ((mode == E0_DENY || mode == E0_STATE_DENY) && (*guard != E0_GUARD_VALUE || e0_state.hpfar_valid != 4)) ||
            (mode == E0_WRONG_ADDRESS && e0_state.hpfar_valid != 4) ||
            (mode == E0_ALLOW && (*guard != E0_STORE_VALUE || e0_state.hpfar_valid))) {
            text("E0_CASE_FAIL\n"); test_exit(1);
        }
        text(negative ? "E0_NEGATIVE_PASS\n" : "E0_CASE_PASS\n");
    }
    text("E0_QEMU_PASS\n"); test_exit(0);
}
