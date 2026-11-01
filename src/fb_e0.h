/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FB_E0_H
#define FB_E0_H
#include <stdint.h>

/* Bounded, read-only evidence for the first or rejected framebuffer page.
 * Published separately from the unchanged E0VS v2 record. Scalars are LE. */
struct fb_e0_translation {
    uint32_t magic; uint16_t version, size;
    uint64_t build, va, par, root, descriptors[4], pa;
    uint64_t isar1, mmfr3, tcr2, mair_alias, last_va, resolved_mair, pfr0;
    uint32_t samples, page_shift, level, status, attribute, flags;
    uint32_t publication; int32_t publication_error;
};
_Static_assert(sizeof(struct fb_e0_translation) == 168, "E0XT v1 wire size");
#define FB_E0_XLAT_MAGIC UINT32_C(0x54583045)
#define FB_E0_XLAT_VALID 1u
#define FB_E0_XLAT_MMFR3 2u
#define FB_E0_XLAT_TCR2 4u
enum fb_e0_walk_status {
    FB_E0_WALK_NONE, FB_E0_WALK_OK, FB_E0_WALK_REGIME, FB_E0_WALK_ROOT,
    FB_E0_WALK_BOUNDS, FB_E0_WALK_DESCRIPTOR, FB_E0_WALK_READ, FB_E0_WALK_PA
};
enum fb_e0_attribute_status {
    FB_E0_ATTR_OK, FB_E0_ATTR_UNSUPPORTED, FB_E0_ATTR_XS_UNAVAILABLE,
    FB_E0_ATTR_MISMATCH, FB_E0_ATTR_WB_SCANOUT
};

/* ARM DDI0601 MAIR_EL2; Arm ARM known issue D20380 (EncodePARAttrs).
 * 0x40 is NC/NC with XS=0, not a wildcard for an unknown memory type.
 * Compare memory classes, not MAIR/PAR bytes: PAR may report effective XS=0.
 * WB is recognized for emulator-owned pixel RAM; native scanout rejects it. */
static inline unsigned fb_e0_attribute(uint64_t par, unsigned selected, uint64_t isar1)
{
    unsigned actual = (unsigned)(par >> 56);
    unsigned xs = (unsigned)((isar1 >> 56) & 15);
    if ((actual == 0x40 || selected == 0x40) && xs != 1)
        return FB_E0_ATTR_XS_UNAVAILABLE;
    if ((actual != 0x44 && actual != 0x40 && actual != 0xff) ||
        (selected != 0x44 && selected != 0x40 && selected != 0xff))
        return FB_E0_ATTR_UNSUPPORTED;
    if ((actual == 0xff) != (selected == 0xff))
        return FB_E0_ATTR_MISMATCH;
    if (actual != 0xff && ((par >> 7) & 3) != 2)
        return FB_E0_ATTR_MISMATCH;
    return FB_E0_ATTR_OK;
}
#endif
