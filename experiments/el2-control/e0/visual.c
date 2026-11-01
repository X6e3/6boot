/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "worlds.h"
#include "worlds_build.h"
#include "visual_check.h"
#include "visual_smoke.h"
#include "fb.h"
#include "iodev.h"
#include "xnuboot.h"

/* Emulator-owned scanout RAM, outside both Stage2 domains. Native Stage2
 * display needs its own ownership/loader review; this does not provide one. */
#define WIDTH 640
#define HEIGHT 360
#define STRIDE 656
uint8_t ew_fb_storage[128 + STRIDE*HEIGHT*4 + 128] __attribute__((aligned(0x4000)));
struct boot_args cur_boot_args;
void iodev_lock(iodev_id_t id) { (void)id; }
void iodev_unlock(iodev_id_t id) { (void)id; }
uint64_t ew_visual_meta[5], ew_visual_step, ew_visual_status, ew_visual_ticks;
uint64_t ew_visual_refusals;
struct e0_visual_smoke_result ew_smoke_trace;
static unsigned active, failed;
extern uint8_t ew_after_yield[], ew_permission_pc[], ew_foreign_pc[], ew_after_done[];
#define READ(reg) ({ uint64_t v; __asm__ volatile("mrs %0, " #reg : "=r"(v)); v; })

/* An observer breakpoint is outside the drawing function and sees completed
 * cache publication. The checkpoint does not authorize a PASS. */
__attribute__((noinline)) void ew_visual_checkpoint(void)
{
    __asm__ volatile("nop" ::: "memory");
}

void ew_visual_init(void)
{
    for (unsigned i = 0; i < sizeof(ew_fb_storage); i++) ew_fb_storage[i] = 0xa5;
    uint32_t *pixels = (uint32_t *)(ew_fb_storage + 128);
    for (unsigned i = 0; i < STRIDE*HEIGHT; i++) pixels[i] = 0x101820;
    cur_boot_args.video.base = (uint64_t)pixels;
    cur_boot_args.video.width = WIDTH; cur_boot_args.video.height = HEIGHT;
    cur_boot_args.video.stride = STRIDE*4; cur_boot_args.video.depth = EW_PIXEL_DEPTH;
    ew_visual_meta[0] = (uint64_t)pixels; ew_visual_meta[1] = WIDTH;
    ew_visual_meta[2] = HEIGHT; ew_visual_meta[3] = STRIDE*4; ew_visual_meta[4] = EW_PIXEL_DEPTH;
    ew_visual_step = ew_visual_ticks = 0; failed = active = 0;
    ew_visual_refusals = 0;
    struct boot_video video = cur_boot_args.video;
    uint64_t par = READ(PAR_EL1), daif = READ(DAIF);
    for (unsigned n = 0; n < 10; n++) {
        unsigned world = n == 0 ? 2 : 0;
        enum fb_e0_state state = n == 1 ? (enum fb_e0_state)-1 : FB_E0_RUNNING;
        if (n == 2) cur_boot_args.video.width = 399;
        if (n == 3) cur_boot_args.video.stride = WIDTH*4-4;
        if (n == 4) cur_boot_args.video.depth = 16;
        if (n == 5) cur_boot_args.video.base = 0;
        if (n == 6) cur_boot_args.video.base += 4;
        if (n == 7) cur_boot_args.video.base = 0x90000000; /* unmapped AT, no device access */
        if (n == 8) {
            cur_boot_args.video.base = UINT64_MAX - 0x47fff;
            cur_boot_args.video.width = 400; cur_boot_args.video.height = 180;
            cur_boot_args.video.stride = 1600;
        }
        if (n == 9) cur_boot_args.video.height = UINT32_MAX; /* exceeds fb_t.size */
        if (fb_e0_frame(world, state, NULL) != (n < 2 ? FB_E0_ARGUMENT : n == 7 ? FB_E0_TRANSLATION : n == 8 ? FB_E0_RANGE : FB_E0_GEOMETRY) || READ(PAR_EL1) != par || READ(DAIF) != daif) failed = 1;
        else ew_visual_refusals++;
        cur_boot_args.video = video;
    }
    for (unsigned i = 0; i < STRIDE*HEIGHT; i++) if (pixels[i] != 0x101820) failed = 1;
}

void ew_visual_event(unsigned world, unsigned kind)
{
    int next = (ew_visual_step == 0 && world == 0 && kind == EW_EVENT_YIELD) ||
               (ew_visual_step == 1 && world == 1 && kind == EW_EVENT_YIELD) ||
               (ew_visual_step == 2 && world == 0 && kind == EW_EVENT_PERMISSION);
    if (!next) return;
    uint64_t start = READ(CNTPCT_EL0);
    if (fb_e0_frame(world, FB_E0_RUNNING, NULL)) failed = 1;
    active = world; ew_visual_status = FB_E0_RUNNING;
    /* Observation only. No switch latency is measured by this demo; keep
     * drawing/pause time in a separate counter rather than reporting it as one. */
    uint64_t until = READ(CNTPCT_EL0) + READ(CNTFRQ_EL0)/4;
    while (READ(CNTPCT_EL0) < until) __asm__ volatile("yield");
    ew_visual_ticks += READ(CNTPCT_EL0) - start;
    ew_visual_step++;
    ew_visual_checkpoint();
}

int ew_visual_finish(void)
{
    const uint8_t *labels[] = {ew_after_yield, ew_permission_pc, ew_foreign_pc, ew_after_done};
    uint64_t pcs[4];
    for (unsigned i = 0; i < 4; i++) pcs[i] = E0_CODE_IPA + (uint64_t)((uintptr_t)labels[i] - (uintptr_t)e0_guest_vectors);
    int good = !failed && ew_visual_step == 3 &&
        ew_visual_trace_ok(&ew_trace, sizeof(ew_trace), EW_BUILD_ID, (uint64_t)e0_arena_start, pcs);
    ew_visual_status = good ? FB_E0_PASS : FB_E0_FAIL;
    if (fb_e0_frame(active, (enum fb_e0_state)ew_visual_status, NULL)) {
        ew_visual_status = FB_E0_FAIL; failed = 1; good = 0;
    }
    ew_visual_step = 4;
    ew_visual_checkpoint();
    return good;
}

/* QEMU fixture for the same framebuffer API used by the native smoke hook.
 * No EL1 entry, Stage2 programming or dual-world claim in this path. */
int ew_visual_smoke_test(void)
{
    if (failed) return 0;
    struct e0_visual_smoke_result *r = &ew_smoke_trace;
    *r = (struct e0_visual_smoke_result){.magic = 0x53563045, .version = 1,
        .size = sizeof(*r), .hcr_before = READ(HCR_EL2), .ctr = READ(CTR_EL0),
        .framebuffer = ew_visual_meta[0], .width = WIDTH, .height = HEIGHT,
        .stride = STRIDE*4, .depth = EW_PIXEL_DEPTH, .build = EW_BUILD_ID};
    if (r->hcr_before & 1) return 0;
    const unsigned worlds[] = {0,1,0};
    uint64_t start = READ(CNTPCT_EL0);
    for (unsigned i = 0; i < 3; i++) {
        if (fb_e0_frame(worlds[i], FB_E0_SMOKE, NULL) || READ(HCR_EL2) != r->hcr_before) return 0;
        r->frames++; r->hcr_after = READ(HCR_EL2); r->ticks = READ(CNTPCT_EL0) - start;
        ew_visual_step = i+1; ew_visual_status = FB_E0_SMOKE;
        ew_visual_checkpoint();
    }
    return 1;
}
