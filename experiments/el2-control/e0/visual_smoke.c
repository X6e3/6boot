/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "build_cfg.h"
#include "fb.h"
#include "utils.h"
#include "xnuboot.h"
#include "visual_smoke.h"
#include "e0_visual_build.h"
#if !defined(SIXBOOT_E0_FB_SMOKE) || !defined(SIXBOOT_E0_VISUAL)
#error "Framebuffer smoke is an explicit, separate development build"
#endif

/* Only the state this path observes or may affect. This is not a vCPU bank. */
struct smoke_context {
    u64 hcr, par, daif, sctlr, tcr, ttbr0, ttbr1, mair, el, tcr2;
};
static void snapshot(struct smoke_context *s)
{
    s->el = mrs(CurrentEL);
    if (s->el != 8) return;
    s->hcr = mrs(HCR_EL2); s->par = mrs(PAR_EL1); s->daif = mrs(DAIF);
    s->sctlr = mrs(SCTLR_EL2); s->tcr = mrs(TCR_EL2);
    s->ttbr0 = mrs(TTBR0_EL2); s->ttbr1 = mrs(TTBR1_EL2); s->mair = mrs(MAIR_EL2);
    if ((mrs(S3_0_C0_C7_3) & 15) == 1 && !((mrs(ID_AA64PFR0_EL1) >> 12) & 15))
        s->tcr2 = mrs(S3_4_C2_C0_3);
}
static u64 changed(const struct smoke_context *a, const struct smoke_context *b)
{
    return (u64)(a->hcr != b->hcr) | (u64)(a->par != b->par) << 1 |
        (u64)(a->daif != b->daif) << 2 | (u64)(a->sctlr != b->sctlr) << 3 |
        (u64)(a->tcr != b->tcr) << 4 | (u64)(a->ttbr0 != b->ttbr0) << 5 |
        (u64)(a->ttbr1 != b->ttbr1) << 6 | (u64)(a->mair != b->mair) << 7 |
        (u64)(a->el != b->el) << 8 | (u64)(a->tcr2 != b->tcr2) << 9;
}
void e0_visual_smoke_run(struct e0_visual_smoke_v2 *r, struct fb_e0_translation *translation)
{
    *translation = (struct fb_e0_translation){.magic = FB_E0_XLAT_MAGIC,
        .version = 1, .size = sizeof(*translation), .build = E0_VISUAL_BUILD_ID};
    *r = (struct e0_visual_smoke_v2){.v1 = {.magic = 0x53563045, .version = 2,
        .size = sizeof(*r), .build = E0_VISUAL_BUILD_ID}, .outcome = E0VS_REFUSED};
    struct smoke_context before = {0}, after = {0};
    snapshot(&before);
    r->current_el = before.el;
    if (before.el != 8) {
        r->v1.error = 1; r->reason = FB_E0_MODE; r->stage = FB_E0_STAGE_REGIME;
        return; /* Unknown return context: caller must not enter its normal fallback. */
    }
    r->v1.hcr_before = before.hcr; r->v1.ctr = mrs(CTR_EL0);
    r->v1.framebuffer = cur_boot_args.video.base;
    r->v1.width = cur_boot_args.video.width; r->v1.height = cur_boot_args.video.height;
    r->v1.stride = cur_boot_args.video.stride; r->v1.depth = cur_boot_args.video.depth;
    r->saved_par = before.par; r->daif_before = before.daif;
    r->sctlr = before.sctlr; r->tcr = before.tcr;
    r->ttbr0 = before.ttbr0; r->ttbr1 = before.ttbr1; r->mair = before.mair;
    r->cache_line = 4ULL << ((r->v1.ctr >> 16) & 15);
    r->diagnostic_flags = FB_E0_SNAPSHOT_VALID | E0VS_TRANSLATION_REQUIRED;
    if (cur_boot_args.video.height && cur_boot_args.video.stride <= UINT64_MAX / cur_boot_args.video.height)
        r->extent = cur_boot_args.video.stride * cur_boot_args.video.height;
    const u64 profile = BIT(34) | BIT(31) | BIT(27);
    int entry_ok = (before.hcr & profile) == profile && !(before.hcr & 1) && (before.sctlr & 1);
    if (!entry_ok) {
        r->v1.error = 1; r->reason = FB_E0_MODE; r->stage = FB_E0_STAGE_REGIME;
    } else {
        r->diagnostic_flags |= FB_E0_REGIME_VALID;
        const unsigned worlds[] = {0, 1, 0};
        u64 start = mrs(CNTPCT_EL0);
        for (unsigned i = 0; i < 3; i++) {
            struct fb_e0_diagnostic d;
            enum fb_e0_reason reason = fb_e0_frame(worlds[i], FB_E0_SMOKE, &d);
            uint64_t build = translation->build;
            *translation = d.translation;
            translation->magic = FB_E0_XLAT_MAGIC; translation->version = 1;
            translation->size = sizeof(*translation); translation->build = build;
            r->diagnostic_flags |= d.flags;
            r->failed_frame = i;
            if (reason) {
                r->v1.error = 2; r->reason = reason; r->stage = d.stage;
                r->failed_va = d.failed_va; r->raw_par = d.raw_par; r->detail = d.detail;
            } else {
                r->v1.frames++;
                r->stage = FB_E0_STAGE_DRAW;
            }
            snapshot(&after);
            r->context_changed |= changed(&before, &after);
            if (reason || r->context_changed || (d.flags & FB_E0_UNSAFE)) break;
            /* Observation only; never included in a context-switch benchmark. */
            udelay(750000);
            snapshot(&after);
            r->context_changed |= changed(&before, &after);
            if (r->context_changed) break;
        }
        r->v1.ticks = mrs(CNTPCT_EL0) - start;
    }
    /* Covers the last draw AND its observation delay. No failure drawing or
     * other experiment operation follows this final context observation. */
    snapshot(&after);
    r->context_changed |= changed(&before, &after);
    r->v1.hcr_after = after.hcr; r->restored_par = after.par; r->daif_after = after.daif;
    if (entry_ok) {
        if (!(r->context_changed & 1)) r->safety |= E0VS_HCR_OK;
        if (!(r->context_changed & 2)) r->safety |= E0VS_PAR_OK;
        if (!(r->context_changed & 4)) r->safety |= E0VS_DAIF_OK;
        if (!(r->context_changed & ~7ULL)) r->safety |= E0VS_HOST_OK;
    }
    if ((r->diagnostic_flags & FB_E0_UNSAFE) || r->context_changed) {
        r->outcome = E0VS_FAILED;
        if (!r->v1.error) {
            r->v1.error = 3; r->stage = FB_E0_STAGE_RESTORE;
            r->reason = r->context_changed & 2 ? FB_E0_PAR_RESTORE :
                        r->context_changed & 4 ? FB_E0_DAIF_RESTORE : FB_E0_HCR_CHANGED;
        }
    } else if (!r->v1.error && r->v1.frames == 3) {
        r->outcome = E0VS_COMPLETE; r->failed_frame = 3;
    }
}
