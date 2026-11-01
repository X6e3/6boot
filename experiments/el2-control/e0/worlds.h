/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SIXBOOT_E0_WORLDS_H
#define SIXBOOT_E0_WORLDS_H
#include "e0.h"
#define EW_PREPARE 5
#define EW_SINGLE 6
#define EW_DUAL 7
#define EW_VMID_A 0x7e
#define EW_VMID_B 0x7f
#define EW_ROOT_B 0x24000
#define EW_STACK_B 0x2c000
#define EW_DATA_B 0x30000
#define EW_RO_B 0x34000
#define EW_CODE_B 0x38000
#define EW_PATTERN_A 0xa00
#define EW_PATTERN_B 0xb00
#define EW_HVC_YIELD 0x6e38
#define EW_HVC_DONE 0x6e39
#define EW_FOREIGN_IPA 0x20000000
#define EW_STACK_IPA 0x10014000 /* one unmapped page on each side */
#define EW_HVC_FAIL 0x6e3a
#define EW_EVENT_YIELD 1
#define EW_EVENT_PERMISSION 2
#define EW_EVENT_FOREIGN 3
#define EW_EVENT_DONE 4
#define EW_EVENT_UNEXPECTED 5
#define EW_EVENTS 12
#define EW_TRACE_HOST_RESTORED 48
#ifndef __ASSEMBLER__
/* This is a two-slot pool, not a dynamic allocator. Each slot owns six
 * distinct 16 KiB pages (root, L3, code, stack, data, read-only data). */
struct ew_domain {
    uint64_t vmid, root, vtcr, code, stack, data, ro, signature, completion, context_ok;
    uint64_t observed, guard_observed;
};
struct ew_event {
    uint64_t world, kind, vmid, root, hcr, vtcr, esr, elr, spsr, far, hpfar;
    uint64_t ipa, validity, expected, context_ok, tlbis;
};
struct ew_trace {
    uint64_t magic, version, size, build, mode, status, host_restored, tlbis, count, refusal;
    uint64_t mmfr0, mmfr1, mmfr2, pfr0, hcr, observer_ok;
    struct ew_domain domain[2];
    struct ew_event event[EW_EVENTS];
};
_Static_assert(offsetof(struct ew_trace, host_restored) == EW_TRACE_HOST_RESTORED, "trace assembly offset");
_Static_assert(sizeof(struct ew_event) == 128 && sizeof(struct ew_trace) == 1856, "E0W1 wire ABI");
extern struct ew_trace ew_trace;
int ew_build(struct e0_tables *t, uint64_t mmfr0, uint64_t base, unsigned world,
              unsigned vmid, struct ew_domain *d);
int ew_validate(const struct e0_tables *t, uint64_t base, unsigned world,
                 const struct ew_domain *d);
int ew_pair(const struct ew_domain d[2]);
int ew_capable(uint64_t mmfr1, uint64_t mmfr2, uint64_t pfr0);
int ew_fault(uint64_t esr, uint64_t far, uint64_t hp, uint64_t pc,
              uint64_t expected_pc, uint64_t expected_ipa, unsigned kind, uint64_t *ipa);
int ew_prepare(unsigned mode);
int ew_restore_host(void);
int ew_dispatch(struct e2_exception_frame *f, struct e2_exception_session *s);
#ifdef E0_VISUAL
void ew_visual_init(void);
int ew_visual_smoke_test(void);
void ew_visual_event(unsigned world, unsigned kind);
int ew_visual_finish(void);
#endif
#endif
#endif
