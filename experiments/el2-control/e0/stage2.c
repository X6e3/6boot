/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "e0.h"

/* 16KiB, L2 -> L3, 32-bit IPA; 48-bit output format, no LPA2/HA/HD/FWB. */
int e0_s2_config(uint64_t id, uint64_t base, unsigned root_offset,
                 unsigned vmid, struct e0_config *out)
{
    unsigned g2 = (unsigned)((id >> 32) & 15), g1 = (unsigned)((id >> 20) & 15);
    unsigned ps = (unsigned)(id & 15);
    static const unsigned bits[] = {32, 36, 40, 42, 44, 48, 52};
    if (!out || !vmid || vmid > 255 || root_offset % E2_PAGE_SIZE ||
        root_offset > E0_ARENA_SIZE - 2 * E2_PAGE_SIZE || ps > 6 ||
        (g2 != 2 && g2 != 3 && !(g2 == 0 && (g1 == 1 || g1 == 2))))
        return E0_ENVIRONMENT;
    if (ps > 5) ps = 5; /* DS=0: describe at most48-bit output addresses. */
    if ((base & (E2_ARENA_ALIGN - 1)) || base >= (UINT64_C(1) << bits[ps]) ||
        E0_ARENA_SIZE > (UINT64_C(1) << bits[ps]) - base)
        return E0_ENVIRONMENT;
    out->vtcr = (UINT64_C(1) << 31) | ((uint64_t)ps << 16) | (2u << 14) |
                (3u << 12) | (1u << 10) | (1u << 8) | (1u << 6) | 32u;
    out->vttbr = ((uint64_t)vmid << 48) | (base + root_offset);
    return E0_OK;
}

int e0_s2_page(struct e0_tables *t, uint64_t ipa, uint64_t pa,
               unsigned permissions, unsigned pa_bits)
{
    if (!t || pa_bits < 32 || pa_bits > 48 || pa >= (UINT64_C(1) << pa_bits) ||
        (pa | ipa) % E2_PAGE_SIZE || ipa < E0_CODE_IPA ||
        ipa >= (uint64_t)E0_CODE_IPA + (1u << 25) ||
        !(permissions & E0_READ) || permissions & ~(E0_READ | E0_WRITE | E0_EXEC) ||
        (permissions & (E0_WRITE | E0_EXEC)) == (E0_WRITE | E0_EXEC))
        return E0_ENVIRONMENT;
    unsigned index = (unsigned)((ipa >> 14) & 2047);
    if (t->l3[index]) return E0_ENVIRONMENT;
    t->l3[index] = pa | 3 | (UINT64_C(15) << 2) | (UINT64_C(3) << 8) |
        (UINT64_C(1) << 10) | ((uint64_t)(permissions & 3) << 6) |
        (permissions & E0_EXEC ? 0 : UINT64_C(1) << 54);
    return E0_OK;
}

int e0_s2_build(struct e0_tables *t, uint64_t id, uint64_t base,
                bool map_guard, struct e0_config *out)
{
    if (!t || e0_s2_config(id, base, E0_L2_PA, E0_VMID, out)) return E0_ENVIRONMENT;
    for (unsigned i = 0; i < 2048; i++) t->l2[i] = t->l3[i] = 0;
    t->l2[E0_CODE_IPA >> 25] = (base + E0_L3_PA) | 3;
    /* config already checked every address against PARange. */
    if (e0_s2_page(t, E0_CODE_IPA, base + E0_CODE_PA, E0_READ | E0_EXEC, 48) ||
        e0_s2_page(t, E0_STACK_IPA, base + E0_STACK_PA, E0_READ | E0_WRITE, 48) ||
        e0_s2_page(t, E0_ALLOWED_IPA, base + E0_ALLOWED_PA, E0_READ | E0_WRITE, 48) ||
        (map_guard && e0_s2_page(t, E0_DENIED_IPA, base + E0_GUARD_PA, E0_READ | E0_WRITE, 48)))
        return E0_ENVIRONMENT;
    return E0_OK;
}

int e0_fault_valid(uint64_t esr, uint64_t far, uint64_t hp,
                    uint64_t pc, uint64_t expected_pc)
{
    /* Lower-EL A64 data abort, IL=1, valid FAR, no EA/CM/S1PTW,
     * write, L3 translation fault. HPFAR is valid for this class.
     * FIPA is IPA[47:12], not the translation-granule page number: its
     * low two bits are valid for our ordinary STR with a 16 KiB granule.
     * Stage1 is disabled: VA == IPA. This does not cover MOPS, permission
     * or address-size faults. */
    uint64_t ipa = ((hp & UINT64_C(0xfffffffff0)) << 8) | (far & 0xfff);
    return ((esr >> 26) & 63) == 0x24 && (esr & (UINT64_C(1) << 25)) &&
           (esr & 0x7ff) == 0x47 && far == E0_DENIED_IPA &&
           ipa == E0_DENIED_IPA && pc == expected_pc;
}

/* The identity-looking IPA of the actual monitor canary must fault at L2:
 * its root entry is invalid. Never map monitor RAM even in the ALLOW case. */
int e0_state_fault_valid(uint64_t esr, uint64_t far, uint64_t hp,
                         uint64_t pc, uint64_t expected_pc, uint64_t state_ipa)
{
    uint64_t ipa = ((hp & UINT64_C(0xfffffffff0)) << 8) | (far & 0xfff);
    return ((esr >> 26) & 63) == 0x24 && (esr & (UINT64_C(1) << 25)) &&
           (esr & 0x7ff) == 0x46 && far == state_ipa &&
           ipa == state_ipa && pc == expected_pc;
}
