/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "visual_check.h"

static uint64_t word(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8*i);
    return v;
}
#define H(n) word(p + 8*(n))
#define D(w,n) word(p + 128 + 96*(w) + 8*(n))
#define E(i,n) word(p + 320 + 128*(i) + 8*(n))
int ew_visual_trace_ok(const void *bytes, size_t size, uint64_t build,
                       uint64_t arena, const uint64_t pcs[4])
{
    const uint8_t *p = bytes;
    if (!p || !pcs || size != 1856 || H(0) != 0x31573045 || H(1) != 1 || H(2) != size ||
        H(3) != build || H(4) != 7 || H(5) || H(6) != 1 || H(7) != 7 || H(8) != 8 || H(9) || H(15) != 1)
        return 0;
    uint64_t hcr = H(14), mm0 = H(10), mm1 = H(11), mm2 = H(12), pf0 = H(13);
    uint64_t host = UINT64_C(1) << 34 | UINT64_C(1) << 31 | UINT64_C(1) << 27;
    uint64_t prohibited = UINT64_C(1) << 46 | UINT64_C(1) << 45 | UINT64_C(1) << 43 |
                          UINT64_C(1) << 42 | UINT64_C(1) << 12 | 1;
    unsigned ps = (unsigned)(mm0 & 15), g2 = (unsigned)((mm0 >> 32) & 15), g1 = (unsigned)((mm0 >> 20) & 15);
    unsigned vmid = (unsigned)((mm1 >> 4) & 15), vh = (unsigned)((mm1 >> 8) & 15);
    unsigned el1 = (unsigned)((pf0 >> 4) & 15), el2 = (unsigned)((pf0 >> 8) & 15);
    if ((hcr & host) != host || hcr & prohibited || ps > 6 ||
        (g2 != 2 && g2 != 3 && !(g2 == 0 && (g1 == 1 || g1 == 2))) ||
        (vmid != 0 && vmid != 2) || vh != 1 || ((mm2 >> 24) & 15) > 2 ||
        (el1 != 1 && el1 != 2) || (el2 != 1 && el2 != 2)) return 0;
    if (ps > 5) ps = 5;
    static const unsigned bits[] = {32,36,40,42,44,48};
    if (!arena || arena % 65536 || arena > (UINT64_C(1) << bits[ps]) - 0x40000) return 0;
    uint64_t vtcr = UINT64_C(0x8000b560) | (uint64_t)ps << 16;
    const unsigned offsets[2][5] = {{0x10000,0x4000,0x18000,0x1c000,0x20000},
                                   {0x24000,0x38000,0x2c000,0x30000,0x34000}};
    for (unsigned w = 0; w < 2; w++) {
        if (D(w,0) != 0x7e + w || D(w,2) != vtcr || D(w,7) != 0xa00 + 0x100*w ||
            D(w,8) != 1 || D(w,9) != 1 || D(w,10) != D(w,7) || D(w,11) != 0x1357) return 0;
        for (unsigned j = 0; j < 5; j++)
            if (D(w,j ? j+2 : 1) != arena + offsets[w][j]) return 0;
    }
    const unsigned worlds[] = {0,1,1,1,1,0,0,0}, kinds[] = {1,1,2,3,4,2,3,4};
    const unsigned generations[] = {1,3,5,5,5,7,7,7};
    for (unsigned i = 0; i < 8; i++) {
        unsigned w = worlds[i], kind = kinds[i];
        if (E(i,0) != w || E(i,1) != kind || E(i,2) != D(w,0) ||
            E(i,3) != (D(w,1) | D(w,0) << 48) || E(i,4) != ((hcr & ~UINT64_C(0x28000039)) | 1) ||
            E(i,5) != vtcr || E(i,7) != pcs[kind-1] || (E(i,8) & 0x1003df) != 0x3c5 ||
            E(i,13) != 1 || E(i,15) != generations[i]) return 0;
        uint64_t esr = E(i,6);
        if (kind == 1 || kind == 4) {
            if (esr != (UINT64_C(0x5a000000) | (kind == 1 ? 0x6e38 : 0x6e39)) ||
                E(i,14) != 1 || E(i,11) || E(i,12)) return 0;
        } else {
            uint64_t address = kind == 2 ? 0x1000c000 : D(1-w,5);
            if ((esr >> 26) != 0x24 || !(esr & (1u << 25)) ||
                (esr & 0x7ff) != (kind == 2 ? 0x4fu : 0x46u) || E(i,9) != address ||
                E(i,11) != address || E(i,12) != (kind == 2 ? 5u : 7u)) return 0;
            if (kind == 3 && (((E(i,10) & UINT64_C(0xfffffffff0)) << 8) | (E(i,9) & 0xfff)) != address)
                return 0;
        }
    }
    for (unsigned i = 320+128*8; i < size; i++) if (p[i]) return 0;
    return 1;
}
