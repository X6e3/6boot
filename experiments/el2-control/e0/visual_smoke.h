/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef E0_VISUAL_SMOKE_H
#define E0_VISUAL_SMOKE_H
#include <stddef.h>
#include <stdint.h>
#include "../../../src/fb_e0.h"
/* Historical E0VS v1 prefix. Its offsets and error meanings remain unchanged. */
struct e0_visual_smoke_result {
    uint32_t magic; uint16_t version, size;
    uint32_t frames, error;
    uint64_t hcr_before, hcr_after, ctr, ticks, framebuffer;
    uint32_t width, height, stride, depth;
    uint64_t build;
};
#define E0VS_COMPLETE 1u
#define E0VS_REFUSED 2u
#define E0VS_FAILED 3u
#define E0VS_HCR_OK 1u
#define E0VS_PAR_OK 2u
#define E0VS_DAIF_OK 4u
#define E0VS_HOST_OK 8u
#define E0VS_SAFE 15u
#define E0VS_PUB_PENDING 0u
#define E0VS_PUB_COMPLETE 1u
#define E0VS_PUB_FAILED 2u
#define E0VS_CONTINUE 1u
#define E0VS_BLOCK_CONTEXT 2u
#define E0VS_BLOCK_EVIDENCE 3u
#define E0VS_BLOCK_P3 4u
#define E0VS_TRANSLATION_REQUIRED 256u
/* Scalar fields are serialized little-endian by the AArch64 LE build. */
struct e0_visual_smoke_v2 {
    struct e0_visual_smoke_result v1;
    uint32_t outcome, reason, stage, safety;
    uint32_t publication; int32_t publication_error;
    uint32_t failed_frame, diagnostic_flags;
    uint64_t failed_va, raw_par, saved_par, restored_par;
    uint64_t daif_before, daif_after;
    uint64_t sctlr, tcr, ttbr0, ttbr1, mair, current_el;
    uint64_t context_changed, extent, cache_line, detail;
    uint32_t decision, publication_step;
    uint64_t reserved;
};
_Static_assert(sizeof(struct e0_visual_smoke_result) == 80, "E0VS v1 wire size");
_Static_assert(sizeof(struct e0_visual_smoke_v2) == 256, "E0VS v2 wire size");
_Static_assert(offsetof(struct e0_visual_smoke_v2, failed_va) == 112, "E0VS diagnostic offset");
_Static_assert(offsetof(struct e0_visual_smoke_v2, decision) == 240, "E0VS policy offset");
void e0_visual_smoke_run(struct e0_visual_smoke_v2 *result, struct fb_e0_translation *translation);
int e0_visual_smoke_bootargs(const char *args, int bytes, uint64_t build);
int e0_visual_smoke_reserve(void *dt);
int e0_visual_smoke_publish(void *dt, struct e0_visual_smoke_v2 *result,
                           struct fb_e0_translation *translation);
unsigned e0_visual_smoke_decision(const struct e0_visual_smoke_v2 *result, int p3_ok,
                                 const struct fb_e0_translation *translation);
#endif
