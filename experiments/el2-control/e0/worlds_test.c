/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "worlds.h"
#include <assert.h>
#include <stdio.h>
static struct e0_tables t[2];
int main(void)
{
    struct ew_domain d[2];
    const uint64_t base = UINT64_C(0x800000000), id = UINT64_C(2) << 32 | 5;
    assert(ew_capable(0x100, 0, 0x110));
    assert(ew_capable(0x120, UINT64_C(2) << 24, 0x220));
    assert(!ew_capable(0x110, 0, 0x110));
    assert(!ew_capable(0x100, UINT64_C(3) << 24, 0x110));
    assert(!ew_capable(0, 0, 0x110));
    assert(!ew_capable(0x100, 0, 0));
    for (unsigned w = 0; w < 2; w++) {
        assert(!ew_build(&t[w], id, base, w, 0x7e + w, &d[w]));
        assert(!ew_validate(&t[w], base, w, &d[w]));
        assert((d[w].vtcr & 63) == 32);
        assert(!t[w].l3[4] && !t[w].l3[6]); /* real stack guards */
        const unsigned mapped[] = {0,2,3,5};
        for (unsigned n = 0; n < 4; n++) {
            unsigned i = mapped[n];
            uint64_t old = t[w].l3[i];
            t[w].l3[i] ^= UINT64_C(1) << 6;
            assert(ew_validate(&t[w], base, w, &d[w]));
            t[w].l3[i] = old;
        }
        t[w].l3[4] = t[w].l3[0];
        assert(ew_validate(&t[w], base, w, &d[w])); t[w].l3[4] = 0;
        assert(e0_s2_page(&t[w], E0_ALLOWED_IPA, d[w].data, E0_READ, 48)); /* overlap */
        assert(e0_s2_page(&t[w], UINT64_C(1) << 32, d[w].data, E0_READ, 48));
        assert(e0_s2_page(&t[w], E0_CODE_IPA + 1, d[w].data, E0_READ, 48));
        assert(e0_s2_page(&t[w], E0_CODE_IPA + 0x10000, d[w].data + 1, E0_READ, 48));
        assert(e0_s2_page(&t[w], E0_CODE_IPA + 0x10000, d[w].data, E0_WRITE, 48));
        assert(e0_s2_page(&t[w], E0_CODE_IPA + 0x10000, d[w].data, 7, 48));
        assert(e0_s2_page(&t[w], E0_CODE_IPA + 0x10000, UINT64_C(1) << 48, E0_READ, 48));
    }
    assert(!ew_pair(d));
    uint64_t old = d[1].vmid; d[1].vmid = d[0].vmid; assert(ew_pair(d)); d[1].vmid = old;
    old = d[1].data; d[1].data = d[0].data; assert(ew_pair(d)); d[1].data = old;
    old = d[0].vtcr; d[0].vtcr |= 1u << 19; assert(ew_validate(&t[0], base, 0, &d[0])); d[0].vtcr = old;
    assert(ew_build(&t[0], id, UINT64_MAX & ~UINT64_C(65535), 0, 0x7e, &d[0]));
    assert(ew_build(&t[0], id, base, 2, 0x7e, &d[0]));
    assert(ew_build(&t[0], id, base, 0, 256, &d[0]));
    assert(ew_build(&t[0], id, base, 0, 0, &d[0]));
    assert(ew_build(&t[0], UINT64_C(2) << 32, base, 0, 0x7e, &d[0]));
    const uint64_t esr = UINT64_C(0x24) << 26 | 1u << 25;
    uint64_t ipa;
    assert(ew_fault(esr | 0x4f, E0_DENIED_IPA, UINT64_MAX, 1, 1, E0_DENIED_IPA, EW_EVENT_PERMISSION, &ipa));
    assert(ipa == E0_DENIED_IPA); /* HPFAR never consumed for this permission fault */
    assert(!ew_fault(esr | 0x44f, E0_DENIED_IPA, 0, 1, 1, E0_DENIED_IPA, EW_EVENT_PERMISSION, &ipa) && !ipa);
    assert(!ew_fault(esr | 0x47, E0_DENIED_IPA, 0, 1, 1, E0_DENIED_IPA, EW_EVENT_PERMISSION, &ipa));
    const unsigned offsets[] = {0, 0xfff, 0x1000, 0x2000, 0x3fff};
    for (unsigned i = 0; i < 5; i++) {
        uint64_t address = EW_FOREIGN_IPA + offsets[i], hp = (address >> 12) << 4;
        assert(ew_fault(esr | 0x46, address, hp, 1, 1, address, EW_EVENT_FOREIGN, &ipa) && ipa == address);
        assert(!ew_fault(esr | 0x46, address, hp ^ 0x10, 1, 1, address, EW_EVENT_FOREIGN, &ipa) && !ipa);
        assert(!ew_fault(esr | 0x446, address, hp, 1, 1, address, EW_EVENT_FOREIGN, &ipa));
        assert(!ew_fault(esr | 0x4e, address, hp, 1, 1, address, EW_EVENT_FOREIGN, &ipa));
        assert(!ew_fault(esr | 0x46, address, hp, 1, 2, address, EW_EVENT_FOREIGN, &ipa));
    }
    puts("EW_HOST_PASS: fixed pool, high PA/low IPA, mapping permissions/aliases, capabilities, VMIDs, fault validity");
}
