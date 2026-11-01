/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "worlds.h"

int ew_capable(uint64_t mmfr1, uint64_t mmfr2, uint64_t pfr0)
{
    unsigned vmids = (unsigned)((mmfr1 >> 4) & 15);
    unsigned vh = (unsigned)((mmfr1 >> 8) & 15);
    unsigned nv = (unsigned)((mmfr2 >> 24) & 15);
    unsigned el1 = (unsigned)((pfr0 >> 4) & 15), el2 = (unsigned)((pfr0 >> 8) & 15);
    /* VS=0 deliberately uses 8-bit VMIDs even on a 16-bit implementation.
     * NV support is recorded, never enabled. Unknown encodings are refused. */
    return (vmids == 0 || vmids == 2) && vh == 1 && nv <= 2 &&
           (el1 == 1 || el1 == 2) && (el2 == 1 || el2 == 2);
}

static void layout(uint64_t base, unsigned world, unsigned vmid, struct ew_domain *d)
{
    *d = (struct ew_domain){.vmid = vmid,
        .root = base + (world ? EW_ROOT_B : E0_L2_PA),
        .code = base + (world ? EW_CODE_B : E0_CODE_PA),
        .stack = base + (world ? EW_STACK_B : E0_STACK_PA),
        .data = base + (world ? EW_DATA_B : E0_ALLOWED_PA),
        .ro = base + (world ? EW_RO_B : E0_GUARD_PA),
        .signature = world ? EW_PATTERN_B : EW_PATTERN_A};
}

int ew_build(struct e0_tables *t, uint64_t id, uint64_t base, unsigned world,
              unsigned vmid, struct ew_domain *d)
{
    struct e0_config cfg;
    if (!t || !d || world > 1 || e0_s2_config(id, base,
        world ? EW_ROOT_B : E0_L2_PA, vmid, &cfg)) return E0_ENVIRONMENT;
    layout(base, world, vmid, d);
    d->vtcr = cfg.vtcr;
    for (unsigned i = 0; i < 2048; i++) t->l2[i] = t->l3[i] = 0;
    t->l2[E0_CODE_IPA >> 25] = (d->root + E2_PAGE_SIZE) | 3;
    if (e0_s2_page(t, E0_CODE_IPA, d->code, E0_READ | E0_EXEC, 48) ||
        e0_s2_page(t, EW_STACK_IPA, d->stack, E0_READ | E0_WRITE, 48) ||
        e0_s2_page(t, E0_ALLOWED_IPA, d->data, E0_READ | E0_WRITE, 48) ||
        e0_s2_page(t, E0_DENIED_IPA, d->ro, E0_READ, 48)) return E0_ENVIRONMENT;
    return ew_validate(t, base, world, d);
}

int ew_validate(const struct e0_tables *t, uint64_t base, unsigned world,
                 const struct ew_domain *d)
{
    if (!t || !d || world > 1 || !d->vmid || d->vmid > 255 ||
        base % E2_ARENA_ALIGN || base > (UINT64_C(1) << 48) - E0_ARENA_SIZE)
        return E0_ENVIRONMENT;
    struct ew_domain expected;
    layout(base, world, (unsigned)d->vmid, &expected);
    static const unsigned bits[] = {32, 36, 40, 42, 44, 48};
    unsigned ps = (unsigned)((d->vtcr >> 16) & 7);
    if ((d->vtcr & ~UINT64_C(0x70000)) != UINT64_C(0x8000b560) || ps > 5 ||
        base > (UINT64_C(1) << bits[ps]) - E0_ARENA_SIZE) return E0_ENVIRONMENT;
    if (d->root != expected.root || d->code != expected.code || d->stack != expected.stack ||
        d->data != expected.data || d->ro != expected.ro || d->signature != expected.signature)
        return E0_ENVIRONMENT;
    /* Exact descriptors reject hidden mappings, cross-world aliases and
     * monitor pages, including attributes/permissions beyond the four slots. */
    const uint64_t attr = 3 | (UINT64_C(15) << 2) | (UINT64_C(3) << 8) | (UINT64_C(1) << 10);
    const uint64_t leaf[] = {d->code | attr | (1u << 6),
        d->stack | attr | (3u << 6) | (UINT64_C(1) << 54),
        d->data | attr | (3u << 6) | (UINT64_C(1) << 54),
        d->ro | attr | (1u << 6) | (UINT64_C(1) << 54)};
    for (unsigned i = 0; i < 2048; i++) {
        uint64_t expected_leaf = 0;
        if (i == 0) expected_leaf = leaf[0];
        else if (i == 2) expected_leaf = leaf[2];
        else if (i == 3) expected_leaf = leaf[3];
        else if (i == ((EW_STACK_IPA >> 14) & 2047)) expected_leaf = leaf[1];
        if (t->l2[i] != (i == (E0_CODE_IPA >> 25) ? (d->root + E2_PAGE_SIZE) | 3 : 0) ||
            t->l3[i] != expected_leaf) return E0_ENVIRONMENT;
    }
    return E0_OK;
}

int ew_pair(const struct ew_domain d[2])
{
    if (!d || !d[0].vmid || !d[1].vmid || d[0].vmid > 255 || d[1].vmid > 255 ||
        d[0].vmid == d[1].vmid || d[0].vtcr != d[1].vtcr) return E0_ENVIRONMENT;
    const uint64_t a[] = {d[0].root, d[0].root + E2_PAGE_SIZE, d[0].code, d[0].stack, d[0].data, d[0].ro};
    const uint64_t b[] = {d[1].root, d[1].root + E2_PAGE_SIZE, d[1].code, d[1].stack, d[1].data, d[1].ro};
    for (unsigned i = 0; i < 6; i++) for (unsigned j = 0; j < 6; j++)
        if (a[i] % E2_PAGE_SIZE || b[j] % E2_PAGE_SIZE || a[i] == b[j]) return E0_ENVIRONMENT;
    return E0_OK;
}

int ew_fault(uint64_t esr, uint64_t far, uint64_t hp, uint64_t pc,
              uint64_t expected_pc, uint64_t expected_ipa, unsigned kind, uint64_t *ipa)
{
    if (!ipa) return 0;
    *ipa = 0;
    if ((esr >> 26) != 0x24 || !(esr & (1u << 25)) || far != expected_ipa || pc != expected_pc)
        return 0;
    if (kind == EW_EVENT_PERMISSION) {
        /* Ordinary permission fault, S1 off: HPFAR is UNKNOWN. FAR is the
         * IPA only because the experiment fixes Stage1 M=0, DC=0, NV=0. */
        if ((esr & 0x7ff) != 0x4f) return 0; /* write, L3 permission */
        *ipa = far;
    } else if (kind == EW_EVENT_FOREIGN) {
        if ((esr & 0x7ff) != 0x46) return 0; /* write, L2 translation */
        *ipa = ((hp & UINT64_C(0xfffffffff0)) << 8) | (far & 0xfff);
        if (*ipa != expected_ipa) { *ipa = 0; return 0; }
    } else return 0;
    return 1;
}
