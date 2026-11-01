/* SPDX-License-Identifier: MIT */

#ifndef FB_H
#define FB_H

#include "types.h"
#include "fb_e0.h"

#define FB_DEPTH_FLAG_RETINA 0x10000

typedef struct {
    u32 *ptr;   /* pointer to the start of the framebuffer */
    u32 *hwptr; /* pointer to the start of the real framebuffer */
    u32 stride; /* framebuffer stride divided by four (i.e. stride in pixels) */
    u32 depth;  /* framebuffer depth (i.e. bits per pixel) */
    u32 width;  /* width of the framebuffer in pixels */
    u32 height; /* height of the framebuffer in pixels */
    u32 size;   /* size of the framebuffer in bytes */
} fb_t;

typedef struct {
    u8 r;
    u8 g;
    u8 b;
} rgb_t;

typedef enum {
    PIX_FMT_XRGB,
    PIX_FMT_XBGR,
} pix_fmt_t;

extern fb_t fb;

static inline rgb_t int2rgb(u32 c)
{
    return (rgb_t){c >> 16, c >> 8, c};
}

void fb_init(bool clear);
void fb_shutdown(bool restore_logo);
void fb_reinit(void);
void fb_update(void);
void fb_set_active(bool active);
void fb_boot_status(const char *line, size_t length, bool failed);

/* Experimental caller decides PASS only after validating its binary trace. */
enum fb_e0_state { FB_E0_RUNNING, FB_E0_PASS, FB_E0_FAIL, FB_E0_SMOKE };
enum fb_e0_reason {
    FB_E0_OK, FB_E0_ARGUMENT, FB_E0_GEOMETRY, FB_E0_RANGE,
    FB_E0_SHADOW, FB_E0_FOOTPRINT, FB_E0_REGIME, FB_E0_TRANSLATION,
    FB_E0_IDENTITY, FB_E0_ATTRIBUTE, FB_E0_PAR_RESTORE,
    FB_E0_DAIF_RESTORE, FB_E0_HCR_CHANGED, FB_E0_MODE,
    FB_E0_P3_FAILED, FB_E0_PUBLICATION
};
enum fb_e0_stage {
    FB_E0_STAGE_NONE, FB_E0_STAGE_GEOMETRY, FB_E0_STAGE_ADDRESS,
    FB_E0_STAGE_SHADOW, FB_E0_STAGE_FOOTPRINT, FB_E0_STAGE_REGIME,
    FB_E0_STAGE_TRANSLATE, FB_E0_STAGE_RESTORE, FB_E0_STAGE_DRAW,
    FB_E0_STAGE_PUBLISH, FB_E0_STAGE_P3
};
#define FB_E0_VA_VALID 1u
#define FB_E0_PAR_VALID 2u
#define FB_E0_SNAPSHOT_VALID 4u
#define FB_E0_REGIME_VALID 8u
#define FB_E0_DRAW_STARTED 16u
#define FB_E0_PAR_TOUCHED 32u
#define FB_E0_DAIF_TOUCHED 64u
#define FB_E0_UNSAFE 128u
struct fb_e0_diagnostic {
    u32 reason, stage, flags, reserved;
    u64 failed_va, raw_par, saved_par, restored_par;
    u64 daif_before, daif_after, detail;
    struct fb_e0_translation translation;
};
enum fb_e0_reason fb_e0_frame(unsigned world, enum fb_e0_state state,
                              struct fb_e0_diagnostic *diagnostic);

void fb_blit(u32 x, u32 y, u32 w, u32 h, void *data, u32 stride, pix_fmt_t format);
void fb_unblit(u32 x, u32 y, u32 w, u32 h, void *data, u32 stride);
void fb_fill(u32 x, u32 y, u32 w, u32 h, rgb_t color);
void boot_progress(int n);
void fb_clear(rgb_t color);
void fb_clear_direct(void);

void fb_display_logo(void);
void fb_restore_logo(void);
void fb_improve_logo(void);

void fb_console_scroll(u32 n);
void fb_console_reserve_lines(u32 n);
ssize_t fb_console_write(const char *bfr, size_t len);

#endif
