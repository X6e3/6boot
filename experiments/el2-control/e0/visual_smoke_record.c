/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Device-tree ownership and boot policy only; this file does not run E0. */
#include "visual_smoke.h"
#include "libfdt/libfdt.h"
#include <string.h>

#define E0VS_MAGIC UINT32_C(0x53563045)
#define E0VS_VERSION 2u
#define E0VS_PROPERTY "6boot,e0-visual-result"
#define E0XT_PROPERTY "6boot,e0-translation-result"
#define E0VS_DIAGNOSTIC_UNSAFE 128u
#define E0VS_SNAPSHOT_REGIME 12u
#define E0VS_DRAW_EVIDENCE 0x7cu
#define E0VS_HOST_PROFILE ((UINT64_C(1) << 34) | (UINT64_C(1) << 31) | (UINT64_C(1) << 27))

/* Bind this opt-in experiment to the selected image, before any rendering.
 * FDT strings must have exactly one terminator and one matching build token. */
int e0_visual_smoke_bootargs(const char *args, int bytes, uint64_t build)
{
    static const char prefix[] = "x6e3.fbsmoke=";
    char expected[sizeof(prefix) - 1 + 16];
    if (!args || bytes < 1 || args[bytes - 1] || memchr(args, 0, (size_t)bytes - 1))
        return 0;
    memcpy(expected, prefix, sizeof(prefix) - 1);
    for (unsigned i = 0; i < 16; i++)
        expected[sizeof(prefix) - 1 + i] = "0123456789abcdef"[(build >> (60 - 4 * i)) & 15];
    unsigned matches = 0;
    const char *end = args + bytes - 1;
    while (args < end) {
        while (args < end && (*args == ' ' || *args == '\t')) args++;
        const char *token = args;
        while (args < end && *args != ' ' && *args != '\t') args++;
        size_t length = (size_t)(args - token);
        if (length >= sizeof(prefix) - 1 && !memcmp(token, prefix, sizeof(prefix) - 1)) {
            if (length != sizeof(expected) || memcmp(token, expected, sizeof(expected))) return 0;
            matches++;
        }
    }
    return matches == 1;
}

static struct e0_visual_smoke_v2 pending_record(void)
{
    struct e0_visual_smoke_v2 pending = {0};
    pending.v1.magic = E0VS_MAGIC;
    pending.v1.version = E0VS_VERSION;
    pending.v1.size = (uint16_t)sizeof(pending);
    return pending;
}

static struct fb_e0_translation pending_translation(void)
{
    return (struct fb_e0_translation){.magic = FB_E0_XLAT_MAGIC, .version = 1,
                                     .size = sizeof(struct fb_e0_translation)};
}

static int valid_identity(const struct e0_visual_smoke_v2 *r)
{
    return r && r->v1.magic == E0VS_MAGIC && r->v1.version == E0VS_VERSION &&
           r->v1.size == sizeof(*r);
}

static int valid_result(const struct e0_visual_smoke_v2 *r)
{
    if (!valid_identity(r) || r->v1.frames > 3 || r->v1.error > 3 ||
        r->reason > 15 || r->stage > 10)
        return 0;
    if (r->outcome == E0VS_COMPLETE)
        return r->v1.frames == 3 && !r->v1.error && !r->reason;
    if (r->outcome == E0VS_REFUSED || r->outcome == E0VS_FAILED)
        return r->v1.error && r->reason;
    return 0;
}

static int valid_translation(const struct e0_visual_smoke_v2 *r,
                             const struct fb_e0_translation *t)
{
    if (!(r->diagnostic_flags & E0VS_TRANSLATION_REQUIRED)) return t == NULL;
    if (!t || t->magic != FB_E0_XLAT_MAGIC || t->version != 1 || t->size != sizeof(*t) ||
        t->build != r->v1.build || t->publication != E0VS_PUB_COMPLETE || t->publication_error ||
        t->status > FB_E0_WALK_PA || t->flags & ~7u || t->attribute > FB_E0_ATTR_WB_SCANOUT)
        return 0;
    if (!t->samples) {
        struct fb_e0_translation empty = pending_translation();
        empty.build = r->v1.build; empty.publication = E0VS_PUB_COMPLETE;
        return !r->v1.frames && !(r->diagnostic_flags & 32) && !memcmp(t, &empty, sizeof(empty));
    }
    const uint64_t extent = (uint64_t)r->v1.stride * r->v1.height;
    if (r->v1.height < 180 || r->v1.width < 400 || t->page_shift != 14 ||
        (r->diagnostic_flags & 96) != 96 || r->v1.stride < (uint64_t)r->v1.width * 4 ||
        extent > UINT32_MAX || r->v1.framebuffer >= (UINT64_C(1) << 48) ||
        extent > (UINT64_C(1) << 48) - r->v1.framebuffer) return 0;
    const uint64_t start = (r->v1.framebuffer + (uint64_t)((r->v1.height - 180) / 2) * r->v1.stride) &
                           ~UINT64_C(0x3fff);
    const uint64_t end = r->v1.framebuffer + (uint64_t)((r->v1.height - 180) / 2 + 180) * r->v1.stride;
    if (end <= start || t->samples > (end - start + 16383) / 16384 ||
        t->last_va != start + (uint64_t)(t->samples - 1) * 16384 ||
        (t->va != start && t->va != t->last_va)) return 0;
    if (r->reason == 7 || r->reason == 8)
        return t->va == r->failed_va && t->par == r->raw_par && !(t->flags & FB_E0_XLAT_VALID);
    if (t->status != FB_E0_WALK_OK)
        return r->reason == 6 && t->status && r->detail == t->status &&
               !(t->flags & FB_E0_XLAT_VALID) && !(t->par & 1) &&
               (t->par & UINT64_C(0x000ffffffffff000)) == t->va;
    if (!(t->flags & FB_E0_XLAT_VALID) || !(t->flags & FB_E0_XLAT_MMFR3) ||
        (t->mmfr3 & 15) > 1 || t->tcr2 ||
        (((t->mmfr3 & 15) == 1) != !!(t->flags & FB_E0_XLAT_TCR2)) ||
        ((t->flags & FB_E0_XLAT_TCR2) && ((t->pfr0 >> 12) & 15)) ||
        ((t->isar1 >> 56) & 15) > 1 || t->mair_alias != r->mair ||
        t->root != r->ttbr0 || !t->root || t->root & ~UINT64_C(0x0000ffffffffc000) ||
        (r->tcr & 63) != 16 || ((r->tcr >> 14) & 3) != 2 ||
        (r->tcr & ((UINT64_C(1) << 59) | (UINT64_C(1) << 40) | (UINT64_C(1) << 39) |
                   (UINT64_C(1) << 37) | (UINT64_C(1) << 7))) ||
        (t->level != 2 && t->level != 3))
        return 0;
    const uint64_t mask = UINT64_C(0x0000ffffffffc000);
    for (unsigned n = 0; n < t->level; n++)
        if ((t->descriptors[n] & 3) != 3 || (t->descriptors[n] & ~(mask | 3)) ||
            !(t->descriptors[n] & mask)) return 0;
    const uint64_t leaf = t->descriptors[t->level];
    const unsigned shift = t->level == 2 ? 25 : 14;
    const uint64_t offset = (UINT64_C(1) << shift) - 1;
    if ((leaf & 3) != (t->level == 2 ? 1u : 3u) || !(leaf & 1024) || (leaf & 128) ||
        ((leaf >> 8) & 3) != 2 || (leaf & mask & offset) ||
        (leaf & ~(mask | UINT64_C(0xfff) | (UINT64_C(3) << 53))) ||
        (t->level == 2 && t->descriptors[3]) ||
        t->pa != ((leaf & mask) | (t->va & offset)) || t->pa != t->va ||
        (t->par & 1) || (t->par & UINT64_C(0x000ffffffffff000)) != (t->pa & ~UINT64_C(0xfff)) ||
        t->resolved_mair != ((r->mair >> (((leaf >> 2) & 7) * 8)) & 255))
        return 0;
    unsigned expected = fb_e0_attribute(t->par, (unsigned)t->resolved_mair, t->isar1);
    if (!expected && t->resolved_mair == 0xff) expected = FB_E0_ATTR_WB_SCANOUT;
    if (t->attribute != expected) return 0;
    if (r->reason == 9)
        return expected && t->va == r->failed_va && t->par == r->raw_par;
    if (expected) return 0;
    return r->outcome != E0VS_COMPLETE ||
           (t->va == start && t->samples == (end - start + 16383) / 16384);
}

/* Wire evidence, not another framebuffer mapper. Keep this acceptance contract
 * independently checked against visual_smoke_decode.py by the host tests. */
static int valid_evidence(const struct e0_visual_smoke_v2 *r, const struct fb_e0_translation *t)
{
    const uint64_t line = UINT64_C(4) << ((r->v1.ctr >> 16) & 15);
    const uint64_t extent = (uint64_t)r->v1.stride * r->v1.height;
    const unsigned flags = r->diagnostic_flags & ~E0VS_TRANSLATION_REQUIRED, failure = flags & 3;
    if (!valid_result(r) || !valid_translation(r, t) || r->reserved || flags & ~255u ||
        r->extent != extent || r->cache_line != line ||
        (failure != 0 && failure != 3))
        return 0;
    if (r->outcome == E0VS_COMPLETE) {
        const uint64_t base = r->v1.framebuffer, tcr = r->tcr;
        const unsigned ips = (unsigned)((tcr >> 32) & 7), t0sz = (unsigned)(tcr & 63);
        static const unsigned pa_bits[] = {32, 36, 40, 42, 44, 48};
        if (r->failed_frame != 3 || r->stage != 8 || flags != E0VS_DRAW_EVIDENCE ||
            r->detail || r->failed_va || r->raw_par ||
            line < 64 || !base || base % line ||
            r->v1.width < 400 || r->v1.height < 180 || r->v1.stride % 4 ||
            r->v1.stride < (uint64_t)r->v1.width * 4 ||
            ((r->v1.depth & 255) != 30 && (r->v1.depth & 255) != 32) ||
            extent > UINT32_MAX || base >= (UINT64_C(1) << 48) ||
            extent > (UINT64_C(1) << 48) - base || ips > 5 || t0sz < 16 || t0sz > 32 ||
            (tcr & (UINT64_C(1) << 59)) || (((tcr >> 14) & 3) != 0 && ((tcr >> 14) & 3) != 2))
            return 0;
        if (base + extent > (UINT64_C(1) << pa_bits[ips]) ||
            base + extent > (UINT64_C(1) << (64 - t0sz)))
            return 0;
        const uint64_t end = base + ((uint64_t)(r->v1.height - 180) / 2 + 179) * r->v1.stride +
                             ((uint64_t)(r->v1.width - 400) / 2 + 400) * 4;
        return ((end + line - 1) & ~(line - 1)) <= base + extent;
    }
    if (r->failed_frame > r->v1.frames || (r->v1.frames && !(flags & 16)))
        return 0;
    /* Stage bits indexed by the existing fb_e0_reason wire values in fb.h. */
    static const uint16_t stages[] = {
        0, 1u << 1, 1u << 1, 1u << 2, 1u << 3, 1u << 4, 1u << 5,
        1u << 6, 1u << 6, 1u << 6, 1u << 7, 1u << 7,
        (1u << 5) | (1u << 7), 1u << 5, 1u << 10, 1u << 9
    };
    if (!(stages[r->reason] & (1u << r->stage)) ||
        (r->reason == 2 && (!r->detail || r->detail > 11)))
        return 0;
    const int at_failure = r->reason >= 7 && r->reason <= 9;
    if (!failure)
        return !at_failure && !r->failed_va && !r->raw_par;
    const uint64_t page = ((r->tcr >> 14) & 3) == 2 ? 16384 : 4096;
    const uint64_t output = r->raw_par & UINT64_C(0x000ffffffffff000);
    if (!at_failure || r->stage != 6 || (flags & 96) != 96 || r->failed_va % page)
        return 0;
    if (r->reason == 7)
        return (r->raw_par & 1) != 0;
    if (r->raw_par & 1)
        return 0;
    if (r->reason == 8)
        return output != r->failed_va;
    if (r->diagnostic_flags & E0VS_TRANSLATION_REQUIRED) return 1;
    return output == r->failed_va && (r->raw_par >> 56) != 0x44 && (r->raw_par >> 56) != 0xff;
}

unsigned e0_visual_smoke_decision(const struct e0_visual_smoke_v2 *r, int p3_ok,
                                 const struct fb_e0_translation *t)
{
    if (!p3_ok)
        return E0VS_BLOCK_P3;
    if (!r)
        return E0VS_BLOCK_EVIDENCE;
    /* These are the hardware runner's contract flags, not an independent
     * physical-safety measurement by this device-tree helper. Contradictory
     * recorded observations still deny continuation. Error 2 alone is never
     * evidence that restoring PAR, DAIF, HCR, and host context succeeded. */
    if (r->safety != E0VS_SAFE || r->context_changed || r->v1.error == 3 ||
        (r->diagnostic_flags & E0VS_SNAPSHOT_REGIME) != E0VS_SNAPSHOT_REGIME ||
        r->current_el != 8 || !(r->sctlr & 1) ||
        (r->v1.hcr_before & E0VS_HOST_PROFILE) != E0VS_HOST_PROFILE ||
        (r->daif_before & ~UINT64_C(0x3c0)) ||
        r->v1.hcr_before != r->v1.hcr_after ||
        ((r->v1.hcr_before | r->v1.hcr_after) & UINT64_C(1)) ||
        r->saved_par != r->restored_par || r->daif_before != r->daif_after ||
        (r->diagnostic_flags & E0VS_DIAGNOSTIC_UNSAFE))
        return E0VS_BLOCK_CONTEXT;
    if (!valid_evidence(r, t) || r->publication != E0VS_PUB_COMPLETE ||
        r->publication_error || r->publication_step != 2)
        return E0VS_BLOCK_EVIDENCE;
    return E0VS_CONTINUE;
}

/* A failed attempt must not leave an earlier COMPLETE receipt at /chosen.
 * Never resize or allocate here. Exact slots become canonical pending records;
 * malformed short slots lose their magic where any bytes can be written. */
static void invalidate_property(void *dt, int node, const char *property,
                                const void *pending, int expected_size)
{
    int size = 0;
    const void *slot = fdt_getprop(dt, node, property, &size);
    if (!slot || size <= 0) return;
    if (size == expected_size) {
        (void)fdt_setprop_inplace(dt, node, property, pending, expected_size);
    } else {
        const uint32_t invalid = 0;
        int bytes = size < (int)sizeof(invalid) ? size : (int)sizeof(invalid);
        (void)fdt_setprop_inplace_namelen_partial(dt, node, property, (int)strlen(property),
                                               0, &invalid, bytes);
    }
}

static void invalidate_slot(void *dt)
{
    if (!dt) return;
    int node = fdt_path_offset(dt, "/chosen");
    if (node < 0) return;
    struct e0_visual_smoke_v2 r = pending_record();
    struct fb_e0_translation t = pending_translation();
    invalidate_property(dt, node, E0VS_PROPERTY, &r, sizeof(r));
    invalidate_property(dt, node, E0XT_PROPERTY, &t, sizeof(t));
}

int e0_visual_smoke_reserve(void *dt)
{
    if (!dt)
        return -FDT_ERR_BADMAGIC;
    int node = fdt_path_offset(dt, "/chosen");
    if (node < 0)
        return node;
    struct e0_visual_smoke_v2 pending = pending_record();
    int ret = fdt_setprop(dt, node, E0VS_PROPERTY, &pending, sizeof(pending));
    if (!ret) {
        struct fb_e0_translation translation = pending_translation();
        ret = fdt_setprop(dt, node, E0XT_PROPERTY, &translation, sizeof(translation));
    }
    if (ret)
        invalidate_slot(dt);
    return ret;
}

static int publication_failure(void *dt, struct e0_visual_smoke_v2 *r,
                               struct fb_e0_translation *t, int error,
                               uint32_t step)
{
    invalidate_slot(dt);
    if (t) { t->publication = E0VS_PUB_FAILED; t->publication_error = error; }
    if (r) {
        /* The experiment outcome, reason, and legacy error are first-failure
         * evidence and must survive a separate publication failure. */
        r->publication = E0VS_PUB_FAILED;
        r->publication_error = error;
        r->publication_step = step;
        r->decision = e0_visual_smoke_decision(r, 1, t);
    }
    return error;
}

int e0_visual_smoke_publish(void *dt, struct e0_visual_smoke_v2 *r, struct fb_e0_translation *t)
{
    if (!r)
        return publication_failure(dt, r, t, -FDT_ERR_BADVALUE, 2);
    /* The caller records a failed reservation because reserve deliberately
     * owns only the FDT. A later finalization must not conceal its errno. */
    if (r->publication == E0VS_PUB_FAILED && r->publication_error < 0 &&
        r->publication_step == 1)
        return publication_failure(dt, r, t, r->publication_error, 1);
    if (!valid_result(r))
        return publication_failure(dt, r, t, -FDT_ERR_BADVALUE, 2);
    if (!dt)
        return publication_failure(dt, r, t, -FDT_ERR_BADMAGIC, 2);
    int node = fdt_path_offset(dt, "/chosen");
    if (node < 0)
        return publication_failure(dt, r, t, node, 2);
    int size = 0;
    const void *slot = fdt_getprop(dt, node, E0VS_PROPERTY, &size);
    if (!slot)
        return publication_failure(dt, r, t, size, 2);
    if (size != (int)sizeof(*r))
        return publication_failure(dt, r, t, -FDT_ERR_NOSPACE, 2);
    struct e0_visual_smoke_v2 pending = pending_record();
    if (memcmp(slot, &pending, sizeof(pending)))
        return publication_failure(dt, r, t, -FDT_ERR_BADSTATE, 2);

    /* Prepare both receipts, copy the companion first and commit E0VS last.
     * COMPLETE and the decision describe the exact bytes to be published. */
    struct fb_e0_translation translated = pending_translation();
    if (r->diagnostic_flags & E0VS_TRANSLATION_REQUIRED) {
        if (!t) return publication_failure(dt, r, t, -FDT_ERR_BADVALUE, 2);
        const void *slot_t = fdt_getprop(dt, node, E0XT_PROPERTY, &size);
        if (!slot_t) return publication_failure(dt, r, t, size, 2);
        if (size != (int)sizeof(*t)) return publication_failure(dt, r, t, -FDT_ERR_NOSPACE, 2);
        struct fb_e0_translation empty = pending_translation();
        if (memcmp(slot_t, &empty, sizeof(empty)))
            return publication_failure(dt, r, t, -FDT_ERR_BADSTATE, 2);
        translated = *t;
        translated.publication = E0VS_PUB_COMPLETE;
        translated.publication_error = 0;
    }
    struct e0_visual_smoke_v2 final = *r;
    final.publication = E0VS_PUB_COMPLETE;
    final.publication_error = 0;
    final.publication_step = 2;
    final.decision = 0;
    final.decision = e0_visual_smoke_decision(&final, 1, t ? &translated : NULL);
    if (r->diagnostic_flags & E0VS_TRANSLATION_REQUIRED) {
        int ret = fdt_setprop_inplace(dt, node, E0XT_PROPERTY, &translated, sizeof(translated));
        if (ret) return publication_failure(dt, r, t, ret, 2);
    }
    int ret = fdt_setprop_inplace(dt, node, E0VS_PROPERTY, &final, sizeof(final));
    if (ret)
        return publication_failure(dt, r, t, ret, 2);
    *r = final;
    if (r->diagnostic_flags & E0VS_TRANSLATION_REQUIRED) *t = translated;
    return 0;
}
