/* SPDX-License-Identifier: MIT */

#include "fb.h"
#include "assert.h"
#include "iodev.h"
#include "malloc.h"
#include "memory.h"
#include "payload.h"
#include "string.h"
#include "types.h"
#include "utils.h"
#include "xnuboot.h"

#define FB_DEPTH_MASK 0xff

fb_t fb;

struct image {
    u32 *ptr;
    u32 width;
    u32 height;
};

static struct {
    struct {
        u8 *ptr;
        u32 width;
        u32 height;
    } font;

    struct {
        u32 row;
        u32 col;

        u32 max_row;
        u32 max_col;
    } cursor;

    struct {
        u32 rows;
        u32 cols;
    } margin;

    bool initialized;
    bool active;
} console;

extern u8 _binary_build_bootlogo_48_bin_start[];
extern u8 _binary_build_bootlogo_128_bin_start[];
extern u8 _binary_build_bootlogo_256_bin_start[];

extern u8 _binary_build_font_bin_start[];
extern u8 _binary_build_font_retina_bin_start[];

const struct image logo_48 = {
    .ptr = (void *)_binary_build_bootlogo_48_bin_start,
    .width = 48,
    .height = 48,
};

const struct image logo_128 = {
    .ptr = (void *)_binary_build_bootlogo_128_bin_start,
    .width = 128,
    .height = 128,
};

const struct image logo_256 = {
    .ptr = (void *)_binary_build_bootlogo_256_bin_start,
    .width = 256,
    .height = 256,
};

struct image custom_logo = {};

const struct image *logo;
struct image orig_logo;

void fb_update(void)
{
    memcpy128(fb.hwptr, fb.ptr, fb.size);
}

static void fb_clear_font_row(u32 row)
{
    const u32 row_size = (console.margin.cols + console.cursor.max_col) * console.font.width * 4;
    const u32 ystart = (console.margin.rows + row) * console.font.height * fb.stride;

    for (u32 y = 0; y < console.font.height; ++y)
        memset32(fb.ptr + ystart + y * fb.stride, 0, row_size);
}

static void fb_move_font_row(u32 dst, u32 src)
{
    const u32 row_size = (console.margin.cols + console.cursor.max_col) * console.font.width * 4;
    u32 ysrc = (console.margin.rows + src) * console.font.height;
    u32 ydst = (console.margin.rows + dst) * console.font.height;

    ysrc *= fb.stride;
    ydst *= fb.stride;

    for (u32 y = 0; y < console.font.height; ++y)
        memcpy32(fb.ptr + ydst + y * fb.stride, fb.ptr + ysrc + y * fb.stride, row_size);

    fb_clear_font_row(src);
}

static inline u32 rgb2pixel_30(rgb_t c)
{
    return (c.b << 2) | (c.g << 12) | (c.r << 22);
}

static inline rgb_t pixel2rgb_30(u32 c)
{
    return (rgb_t){(c >> 22) & 0xff, (c >> 12) & 0xff, c >> 2};
}

static inline u32 rgb2pixel_24(rgb_t c)
{
    return c.b | (c.g << 8) | (c.r << 16);
}

static inline rgb_t pixel2rgb_24(u32 c)
{
    return (rgb_t){(c >> 16) & 0xff, (c >> 8) & 0xff, c};
}

static inline u32 rgb2pixel(rgb_t c)
{
    if ((cur_boot_args.video.depth & 0xff) == 32) {
        return rgb2pixel_24(c);
    } else {
        return rgb2pixel_30(c);
    }
}

static inline rgb_t pixel2rgb(u32 c)
{
    if ((cur_boot_args.video.depth & 0xff) == 32) {
        return pixel2rgb_24(c);
    } else {
        return pixel2rgb_30(c);
    }
}

static inline void fb_set_pixel(u32 x, u32 y, rgb_t c)
{
    fb.ptr[x + y * fb.stride] = rgb2pixel(c);
}

static inline rgb_t fb_get_pixel(u32 x, u32 y)
{
    return pixel2rgb(fb.ptr[x + y * fb.stride]);
}

void fb_blit(u32 x, u32 y, u32 w, u32 h, void *data, u32 stride, pix_fmt_t pix_fmt)
{
    u8 *p = data;

    for (u32 i = 0; i < h; i++) {
        for (u32 j = 0; j < w; j++) {
            rgb_t color;
            switch (pix_fmt) {
                default:
                case PIX_FMT_XRGB:
                    color.r = p[(j + i * stride) * 4];
                    color.g = p[(j + i * stride) * 4 + 1];
                    color.b = p[(j + i * stride) * 4 + 2];
                    break;
                case PIX_FMT_XBGR:
                    color.r = p[(j + i * stride) * 4 + 2];
                    color.g = p[(j + i * stride) * 4 + 1];
                    color.b = p[(j + i * stride) * 4];
                    break;
            }
            fb_set_pixel(x + j, y + i, color);
        }
    }
    fb_update();
}

void fb_unblit(u32 x, u32 y, u32 w, u32 h, void *data, u32 stride)
{
    u8 *p = data;

    if (!console.initialized)
        return;

    for (u32 i = 0; i < h; i++) {
        for (u32 j = 0; j < w; j++) {
            rgb_t color = fb_get_pixel(x + j, y + i);
            p[(j + i * stride) * 4] = color.r;
            p[(j + i * stride) * 4 + 1] = color.g;
            p[(j + i * stride) * 4 + 2] = color.b;
            p[(j + i * stride) * 4 + 3] = 0xff;
        }
    }
}

void fb_fill(u32 x, u32 y, u32 w, u32 h, rgb_t color)
{
    if (!console.initialized)
        return;

    u32 c = rgb2pixel(color);
    for (u32 i = 0; i < h; i++)
        memset32(&fb.ptr[x + (y + i) * fb.stride], c, w * 4);
    fb_update();
}

void fb_clear(rgb_t color)
{
    if (!console.initialized)
        return;

    u32 c = rgb2pixel(color);
    memset32(fb.ptr, c, fb.stride * fb.height * 4);
    fb_update();
}

void fb_blit_image(u32 x, u32 y, const struct image *img)
{
    if (!console.initialized)
        return;

    fb_blit(x, y, img->width, img->height, img->ptr, img->width, PIX_FMT_XRGB);
}

void fb_unblit_image(u32 x, u32 y, struct image *img)
{
    if (!console.initialized)
        return;

    fb_unblit(x, y, img->width, img->height, img->ptr, img->width);
}

void fb_blit_logo(const struct image *logo)
{
    if (!console.initialized)
        return;

    fb_blit_image((fb.width - logo->width) / 2, (fb.height - logo->height) / 2, logo);
}

void fb_display_logo(void)
{
    if (!console.initialized)
        return;

    printf("fb: display logo\n");
    fb_blit_logo(logo);
}

void fb_restore_logo(void)
{
    if (!console.initialized)
        return;

    if (!orig_logo.ptr)
        return;
    fb_blit_logo(&orig_logo);
}

void fb_improve_logo(void)
{
    const u8 magic[] = "BY;iX2gK0b89P9P*Qa";
    u8 *p = (void *)orig_logo.ptr;

    if (!p || p[orig_logo.width * (orig_logo.height + 1) * 2] <= 250)
        return;

    for (u32 i = 0; i < orig_logo.height; i++) {
        const u8 *c = &magic[min((max(i * 128 / orig_logo.height, 41) - 41) / 11, 5) * 3];
        for (u32 j = 0; j < (orig_logo.width * 4); j++, p++)
            *p = (*p * (c[(j - (j >> 2)) % 3] - 42)) / 63;
    }
}

static inline rgb_t font_get_pixel(u8 c, u32 x, u32 y)
{
    c -= 0x20;
    u8 v =
        console.font.ptr[c * console.font.width * console.font.height + y * console.font.width + x];

    rgb_t col = {.r = v, .g = v, .b = v};
    return col;
}

/* P2T: bounded footer in existing framebuffer and font. No full-screen copy. */
void fb_boot_status(const char *line, size_t length, bool failed)
{
    if (!console.initialized || !fb.ptr || !fb.hwptr || !line ||
        !console.font.ptr || !console.font.width || !console.font.height ||
        fb.width<4*console.font.width || fb.height<console.font.height ||
        fb.stride<fb.width || (u64)fb.stride*4*fb.height>fb.size)
        return;
    u32 y0=fb.height-console.font.height;
    for (u32 y=y0;y<fb.height;y++)
        for (u32 x=0;x<fb.width;x++) {
            size_t off=(size_t)y*fb.stride+x;
            fb.ptr[off]=fb.hwptr[off]=0;
        }
    u32 column=1,max_columns=fb.width/console.font.width-1;
    for (size_t n=0;n<length && column<max_columns;n++) {
        u8 c=(u8)line[n];
        if (c=='\r' || c=='\n')break;
        if (c<0x20 || c>=0x7f)c='?';
        for (u32 y=0;y<console.font.height;y++)
            for (u32 x=0;x<console.font.width;x++) {
                rgb_t rgb=font_get_pixel(c,x,y);
                if (failed) { rgb.g/=2;rgb.b/=2; }
                size_t off=(size_t)(y0+y)*fb.stride+column*console.font.width+x;
                u32 pixel=rgb2pixel(rgb);
                fb.ptr[off]=fb.hwptr[off]=pixel;
            }
        column++;
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static void fb_putbyte(u8 c)
{
    u32 x = (console.margin.cols + console.cursor.col) * console.font.width;
    u32 y = (console.margin.rows + console.cursor.row) * console.font.height;

    for (u32 i = 0; i < console.font.height; i++)
        for (u32 j = 0; j < console.font.width; j++)
            fb_set_pixel(x + j, y + i, font_get_pixel(c, j, i));
}

static void fb_putchar(u8 c)
{
    if (c == '\r') {
        console.cursor.col = 0;
    } else if (c == '\n') {
        console.cursor.row++;
        console.cursor.col = 0;
    } else if (c >= 0x20 && c < 0x7f) {
        fb_putbyte(c);
        console.cursor.col++;
    } else {
        fb_putbyte('?');
        console.cursor.col++;
    }

    if (console.cursor.col == console.cursor.max_col) {
        console.cursor.row++;
        console.cursor.col = 0;
    }

    if (console.cursor.row == console.cursor.max_row)
        fb_console_scroll(1);
}

void fb_console_scroll(u32 n)
{
    if (!console.initialized)
        return;

    u32 row = 0;
    n = min(n, console.cursor.row);
    for (; row < console.cursor.max_row - n; ++row)
        fb_move_font_row(row, row + n);
    for (; row < console.cursor.max_row; ++row)
        fb_clear_font_row(row);
    console.cursor.row -= n;
}

void fb_console_reserve_lines(u32 n)
{
    if (!console.initialized)
        return;

    if ((console.cursor.max_row - console.cursor.row) <= n)
        fb_console_scroll(1 + n - (console.cursor.max_row - console.cursor.row));
    fb_update();
}

ssize_t fb_console_write(const char *bfr, size_t len)
{
    ssize_t wrote = 0;

    if (!console.initialized || !console.active)
        return 0;

    while (len--) {
        fb_putchar(*bfr++);
        wrote++;
    }

    fb_update();

    return wrote;
}

static bool fb_console_iodev_can_write(void *opaque)
{
    UNUSED(opaque);
    return console.initialized && console.active;
}

static ssize_t fb_console_iodev_write(void *opaque, const void *buf, size_t len)
{
    UNUSED(opaque);
    return fb_console_write(buf, len);
}

const struct iodev_ops iodev_fb_ops = {
    .can_write = fb_console_iodev_can_write,
    .write = fb_console_iodev_write,
};

struct iodev iodev_fb = {
    .ops = &iodev_fb_ops,
    .usage = USAGE_CONSOLE,
    .lock = SPINLOCK_INIT,
};

static void fb_clear_console(void)
{
    for (u32 row = 0; row < console.cursor.max_row; ++row)
        fb_clear_font_row(row);

    console.cursor.col = 0;
    console.cursor.row = 0;
    fb_update();
}

void fb_clear_direct(void)
{
    size_t fb_size = cur_boot_args.video.stride * cur_boot_args.video.height;

    mmu_add_mapping(cur_boot_args.video.base, cur_boot_args.video.base, ALIGN_UP(fb_size, 0x4000),
                    MAIR_IDX_NORMAL_NC, PERM_RW);

    memset64((void *)cur_boot_args.video.base, 0, fb_size);
}

void fb_init(bool clear)
{
    void *custom_128, *custom_256;
    fb.hwptr = (void *)cur_boot_args.video.base;
    fb.stride = cur_boot_args.video.stride / 4;
    fb.width = cur_boot_args.video.width;
    fb.height = cur_boot_args.video.height;
    fb.depth = cur_boot_args.video.depth & FB_DEPTH_MASK;
    fb.size = cur_boot_args.video.stride * cur_boot_args.video.height;
    printf("fb init: %dx%d (%d) [s=%d] @%p\n", fb.width, fb.height, fb.depth, fb.stride, fb.hwptr);

    mmu_add_mapping(cur_boot_args.video.base, cur_boot_args.video.base, ALIGN_UP(fb.size, 0x4000),
                    MAIR_IDX_NORMAL_NC, PERM_RW);

    fb.ptr = malloc(fb.size);
    memcpy(fb.ptr, fb.hwptr, fb.size);

    // This is the touchbar, make everything tiny
    if (chip_id == T8012) {
        logo = &logo_48;
        console.font.ptr = _binary_build_font_bin_start;
        console.font.width = 8;
        console.font.height = 16;
    } else if (cur_boot_args.video.depth & FB_DEPTH_FLAG_RETINA) {
        logo = &logo_256;
        console.font.ptr = _binary_build_font_retina_bin_start;
        console.font.width = 16;
        console.font.height = 32;
    } else {
        logo = &logo_128;
        console.font.ptr = _binary_build_font_bin_start;
        console.font.width = 8;
        console.font.height = 16;
    }

    console.margin.rows = 2;
    console.margin.cols = 4;
    console.cursor.col = 0;
    console.cursor.row = 0;

    if (fb.height < fb.width) {
        console.cursor.max_row = (fb.height / console.font.height) - 2 * console.margin.rows;
        console.cursor.max_col =
            ((fb.width - logo->width) / 2) / console.font.width - 2 * console.margin.cols;
    } else { // there aren't much screen real estate for us to waste here
        console.cursor.max_row = (fb.height / console.font.height);
        console.cursor.max_col = (fb.width / console.font.width);
        console.margin.rows = 0;
        console.margin.cols = 0;
    }

    console.initialized = true;
    console.active = false;

    if (!orig_logo.ptr) {
        orig_logo = *logo;
        orig_logo.ptr = malloc(orig_logo.width * orig_logo.height * 4);
        fb_unblit_image((fb.width - orig_logo.width) / 2, (fb.height - orig_logo.height) / 2,
                        &orig_logo);
    }

    if (payload_logo(&custom_128, &custom_256)) {
        custom_logo = *logo;
        if (custom_logo.width == 256) {
            custom_logo.ptr = custom_256;
            logo = &custom_logo;
        } else if (custom_logo.width == 128) {
            custom_logo.ptr = custom_128;
            logo = &custom_logo;
        } else {
            printf("fb: unexpected logo dimensions %ux%u\n", custom_logo.width, custom_logo.height);
        }
    }

    if (clear) {
        memset32(fb.ptr, 0, fb.size);
    } else {
        // Workaround for m1n1 stage 1 framebuffer UAF bug
        memset32(fb.ptr, 0, min(256, fb.size));
    }

    fb_clear_console();

    printf("fb console: max rows %d, max cols %d\n", console.cursor.max_row,
           console.cursor.max_col);
}

void fb_set_active(bool active)
{
    console.active = active;
    if (active)
        iodev_console_kick();
}

void fb_shutdown(bool restore_logo)
{
    if (!console.initialized)
        return;

    console.active = false;
    fb_clear_console();
    if (restore_logo) {
        fb_restore_logo();
        free(orig_logo.ptr);
        orig_logo.ptr = NULL;
    }
    free(fb.ptr);
    console.initialized = false;
}

void fb_reinit(void)
{
    if (!console.initialized)
        return;

    fb_shutdown(false);
    fb_init(true);
    fb_display_logo();
}
/*
 * 6boot boot trace: the boot drawn as one logic-analyzer channel, in place of the green squares.
 *
 * Every boot step is an edge. The level between two edges is the step that ran, and
 * its width is the time that step took, read from CNTPCT: one pixel per millisecond on top of a
 * minimum width, so a step of a few microseconds still shows. A step too long for its share of
 * the screen is cut with a break (//); its time is written under every step that has room. The
 * trace's own drawing is not counted: a step runs from the end of one call to the start of the
 * next.
 *
 * The colours are the old breadcrumbs': green while 6boot loads (0-7), cyan while it tears down
 * (8-12), white for the jump to the kernel (13). A hung boot leaves an open level at the right
 * end, with the step's name in the header.
 *
 * It sits right of the console area m1n1 clears (x < 1088 on J713), under the logo. Drawing goes
 * to the hardware framebuffer and, while it exists, to the shadow buffer, so a later fb_update()
 * copy cannot erase it. Nothing here changes the boot flow.
 *
 * Visible easter egg: the header reads like an analyzer's, sample memory included. 256K was the
 * local store of a Cell SPU.
 */
#define BT_STEPS    14
#define BT_LINE     3  /* trace thickness */
#define BT_SWING    40 /* high level to low level */
#define BT_LEAD     24 /* idle low before the first edge */
#define BT_STUB     12 /* open level of the step now running */
#define BT_TEXT_MAX 32

static const char *const bt_step_name[BT_STEPS] = {
    "payload",      "secondary CPUs",  "mitigations", "devicetree",     "handoff",
    "USB",          "PCIe",            "final checks", "exceptions off", "USB off",
    "display off",  "framebuffer off", "MMU off",     "kernel",
};

/* Embedded in the binary for `strings`. It is never printed; one volatile read keeps it linked. */
__attribute__((used)) static const char bt_signature[] =
    "6boot: P3 entry called on J713 and returned, 2026-10-19 02:57";

static bool bt_started, bt_high;
static int bt_prev = -1;
static rgb_t bt_bg;
static u32 bt_x0, bt_xmax, bt_x, bt_min_w, bt_max_w, bt_name_x;
static u32 bt_hdr_y, bt_idx_y, bt_hi_y, bt_lo_y, bt_dur_y;
static u64 bt_t, bt_total_us;

static void bt_layout(void)
{
    u32 width = cur_boot_args.video.width, height = cur_boot_args.video.height;
    u32 logo_h = logo ? logo->height : 256, fw = console.font.width ? console.font.width : 16;
    u32 fh = console.font.height ? console.font.height : 32;
    u32 room;

    bt_x0 = (console.margin.cols + console.cursor.max_col) * fw + fw;
    bt_xmax = width > 2 * fw ? width - 2 * fw : width;
    bt_hdr_y = (height + logo_h) / 2 + fh;
    bt_idx_y = bt_hdr_y + 2 * fh + fh / 2;
    bt_hi_y = bt_idx_y + fh + fh / 4;
    bt_lo_y = bt_hi_y + BT_SWING;
    bt_dur_y = bt_lo_y + BT_LINE + fh / 4;

    room = bt_xmax > bt_x0 + BT_LEAD + BT_STUB ? bt_xmax - bt_x0 - BT_LEAD - BT_STUB : 0;
    bt_max_w = room / BT_STEPS;
    bt_min_w = 2 * fw + 8 < bt_max_w ? 2 * fw + 8 : bt_max_w;
}

static void bt_fill(u32 x0, u32 y0, u32 w, u32 h, rgb_t c)
{
    u32 *hw = (u32 *)cur_boot_args.video.base;
    u32 stride = cur_boot_args.video.stride / 4;
    u32 width = cur_boot_args.video.width, height = cur_boot_args.video.height;
    bool shadow = console.initialized && fb.ptr;
    u32 px = rgb2pixel(c);

    if (!hw || !stride || x0 + w > width || y0 + h > height)
        return;
    for (u32 y = y0; y < y0 + h; y++)
        for (u32 x = x0; x < x0 + w; x++) {
            hw[y * stride + x] = px;
            if (shadow)
                fb.ptr[y * fb.stride + x] = px;
        }
}

static u8 bt_mix(u8 bg, u8 fg, u32 v)
{
    return bg + ((int)fg - (int)bg) * (int)v / 255;
}

static u32 bt_text_w(const char *s, u32 scale)
{
    return strlen(s) * console.font.width * scale;
}

static void bt_text(u32 x, u32 y, const char *s, u32 scale, rgb_t fg)
{
    if (!console.font.ptr || !console.font.width || !console.font.height)
        return;
    for (u32 n = 0; s[n] && n < BT_TEXT_MAX; n++, x += console.font.width * scale) {
        u8 ch = (u8)s[n];

        if (ch < 0x20 || ch >= 0x7f)
            ch = '?';
        for (u32 fy = 0; fy < console.font.height; fy++)
            for (u32 fx = 0; fx < console.font.width; fx++) {
                u32 v = font_get_pixel(ch, fx, fy).r;
                rgb_t c = {bt_mix(bt_bg.r, fg.r, v), bt_mix(bt_bg.g, fg.g, v),
                           bt_mix(bt_bg.b, fg.b, v)};

                bt_fill(x + fx * scale, y + fy * scale, scale, scale, c);
            }
    }
}

/* Decimal digits of v, then the unit. */
static char *bt_num(char *p, u64 v, const char *unit)
{
    char tmp[20];
    int n = 0;

    do
        tmp[n++] = '0' + v % 10;
    while ((v /= 10) && n < 20);
    while (n)
        *p++ = tmp[--n];
    while (*unit)
        *p++ = *unit++;
    *p = 0;
    return p;
}

static void bt_time(char *p, u64 us)
{
    if (us < 1000)
        bt_num(p, us, "us");
    else if (us < 10000000)
        bt_num(p, us / 1000, "ms");
    else
        bt_num(p, us / 1000000, "s");
}

static rgb_t bt_colour(int step)
{
    if (step >= 13)
        return (rgb_t){0xff, 0xff, 0xff};
    if (step >= 8)
        return (rgb_t){0x00, 0xff, 0xff};
    return (rgb_t){0x00, 0xff, 0x00};
}

static rgb_t bt_dim(rgb_t c)
{
    return (rgb_t){c.r / 2, c.g / 2, c.b / 2};
}

static void bt_level(u32 x, u32 w, rgb_t c)
{
    bt_fill(x, bt_high ? bt_hi_y : bt_lo_y, w, BT_LINE, c);
}

/* The step whose level is open took `us`: draw its full width, its number and its time. */
static void bt_close(int step, u64 us)
{
    rgb_t c = bt_colour(step), d = bt_dim(c);
    u64 lin = us / 1000;
    bool cut = bt_min_w + lin > bt_max_w;
    u32 w = cut ? bt_max_w : bt_min_w + (u32)lin;
    char s[24];

    if (bt_x + w + BT_STUB > bt_xmax)
        w = bt_xmax > bt_x + BT_STUB ? bt_xmax - bt_x - BT_STUB : 0;
    if (!w)
        return;

    bt_level(bt_x, w, c);
    if (cut) {
        u32 bw = bt_text_w("//", 1), by = (bt_high ? bt_hi_y : bt_lo_y) - console.font.height / 2;

        bt_fill(bt_x + (w - bw) / 2, by, bw, console.font.height, bt_bg);
        bt_text(bt_x + (w - bw) / 2, by, "//", 1, c);
    }

    bt_num(s, step, "");
    bt_text(bt_x + (w - bt_text_w(s, 1)) / 2, bt_idx_y, s, 1, d);
    bt_time(s, us);
    if (bt_text_w(s, 1) <= w)
        bt_text(bt_x + (w - bt_text_w(s, 1)) / 2, bt_dur_y, s, 1, d);

    bt_x += w;
}

/* An edge at bt_x into the level of `step`, left open until the next call. */
static void bt_open(int step)
{
    rgb_t c = bt_colour(step);

    bt_high = !bt_high;
    bt_fill(bt_x, bt_hi_y, BT_LINE, bt_lo_y - bt_hi_y + BT_LINE, c);
    bt_level(bt_x, BT_STUB, c);
}

static void bt_header(int step)
{
    char s[BT_TEXT_MAX + 1], *p = s;
    u32 fh = console.font.height;

    bt_fill(bt_name_x, bt_hdr_y + fh, BT_TEXT_MAX * console.font.width, fh, bt_bg);
    *p++ = '>';
    *p++ = ' ';
    for (const char *n = bt_step_name[step]; *n;)
        *p++ = *n++;
    *p = 0;
    if (step == BT_STEPS - 1) {
        *p++ = ' ';
        bt_time(p, bt_total_us);
    }
    bt_text(bt_name_x, bt_hdr_y + fh, s, 1, bt_colour(step));
}

/* Developer mode only; boot_progress() in boot-die.c.in calls it. */
static void bt_step(int n)
{
    const rgb_t green = {0x00, 0xff, 0x00}, idle = {0x00, 0x60, 0x00};
    u64 now = get_ticks();
    u32 *hw = (u32 *)cur_boot_args.video.base;
    u32 stride = cur_boot_args.video.stride / 4;

    if (!hw || !stride || n < 0 || n >= BT_STEPS || n <= bt_prev)
        return;

    if (!bt_started) {
        char s[BT_TEXT_MAX + 1];
        u32 fw = console.font.width, fh = console.font.height, x;

        bt_started = true;
        (void)*(volatile const char *)bt_signature;
        bt_layout();
        bt_bg = pixel2rgb(hw[bt_hi_y * stride + bt_x0 - fw / 2]);

        bt_text(bt_x0, bt_hdr_y, "6boot", 2, green);
        x = bt_x0 + bt_text_w("6boot", 2) + fw;
        bt_text(x, bt_hdr_y + fh, "CH0", 1, bt_dim(green));
        x += bt_text_w("CH0 ", 1);
        bt_num(s, mrs(CNTFRQ_EL0) / 1000000, "MHz");
        bt_text(x, bt_hdr_y + fh, s, 1, bt_dim(green));
        x += bt_text_w(s, 1) + fw;
        bt_text(x, bt_hdr_y + fh, "depth 256K", 1, bt_dim(green));
        bt_name_x = x + bt_text_w("depth 256K", 1) + 2 * fw;

        bt_high = false;
        bt_level(bt_x0, BT_LEAD, idle);
        bt_x = bt_x0 + BT_LEAD;
    } else {
        u64 us = ticks_to_usecs(now - bt_t);

        bt_total_us += us;
        bt_close(bt_prev, us);
    }

    bt_open(n);
    bt_prev = n;
    bt_header(n);
    if (n == BT_STEPS - 1)
        bt_text(bt_x0, bt_hdr_y, "6boot", 2, bt_colour(n));

    __asm__ volatile("dsb sy" ::: "memory");
    bt_t = get_ticks();
}

/*
 * 6boot die: the boot screen. Appended to fb.c after boot-trace.c.in, whose drawing helpers it uses.
 *
 * The picture is a floorplan of 6boot's own stage2: one block per part, each block's area its
 * source line count (die_blocks.h, generated by gen_die.py). Hue is ownership: 6boot's parts are
 * green, the code still taken from m1n1 and its libraries is bronze. Brightness is power: a block
 * lights when its boot step runs, carries an amber edge while that step is running, and goes dim
 * again when 6boot tears it down. The hypervisor is built in but never runs during boot; its block is
 * the TM block, lit throughout (on -1, off 127 in die_blocks.h).
 *
 * die_256.bin holds the die with every block powered; a block that is off is drawn from the same
 * pixels at 22% brightness. m1n1 draws the same die (blocks on from the start lit) as its boot
 * logo before step 0.
 *
 * User mode (SIXBOOT_USER, built with FB_SILENT_MODE): the die alone, twice the size, no console
 * text. Developer mode: the die at logo size with the console on the left and the boot trace
 * under it. Nothing here changes the boot flow.
 */
#include "../build/build_cfg.h" /* SIXBOOT_USER comes from CFG; fb.c does not include it */
#include "../build/die_blocks.h"

extern u8 _binary_build_die_256_bin_start[];

#ifdef SIXBOOT_USER
#define DIE_SCALE 2
#else
#define DIE_SCALE 1
#endif
#define DIE_SIZE 256

static int die_shown = -2; /* step the die shows; -1 is the state before step 0 */
static bool die_after_fb;
static u32 die_x0, die_y0;

/* 0: off, 1: on, 2: its step is running. */
static int die_state(const struct die_block *b, int step)
{
    if (b->on >= 0 && step == b->on)
        return 2;
    return (b->on < step && step < b->off) || (b->on == -1 && step < b->off);
}

static rgb_t die_px(u32 x, u32 y, bool on)
{
    const u8 *p = &_binary_build_die_256_bin_start[(y * DIE_SIZE + x) * 4];

    if (on)
        return (rgb_t){p[0], p[1], p[2]};
    return (rgb_t){p[0] * 22 / 100, p[1] * 22 / 100, p[2] * 22 / 100};
}

static void die_dot(u32 x, u32 y, rgb_t c)
{
    bt_fill(die_x0 + x * DIE_SCALE, die_y0 + y * DIE_SCALE, DIE_SCALE, DIE_SCALE, c);
}

static void die_block_draw(const struct die_block *b, int state)
{
    const rgb_t amber = {0xff, 0xa0, 0x1e};
    u32 w = b->w > 1 ? b->w - 1u : 1u, h = b->h > 1 ? b->h - 1u : 1u; /* channel stays substrate */

    for (u32 j = 0; j < h; j++)
        for (u32 i = 0; i < w; i++) {
            bool edge = i == 0 || j == 0 || i == w - 1 || j == h - 1;

            die_dot(b->x + i, b->y + j, state == 2 && edge ? amber : die_px(b->x + i, b->y + j, state));
        }
}

/* The whole die: seal ring, pads and routing channels as stored, then every block in its state. */
static void die_draw_all(int step)
{
    for (u32 y = 0; y < DIE_SIZE; y++)
        for (u32 x = 0; x < DIE_SIZE; x++)
            die_dot(x, y, die_px(x, y, true));
    for (int i = 0; i < DIE_BLOCKS; i++)
        die_block_draw(&die_block[i], die_state(&die_block[i], step));
}

static void die_step(int n)
{
    if (die_shown == -2) {
        die_x0 = (cur_boot_args.video.width - DIE_SIZE * DIE_SCALE) / 2;
        die_y0 = (cur_boot_args.video.height - DIE_SIZE * DIE_SCALE) / 2;
        die_draw_all(n);
    } else if (!console.initialized && !die_after_fb) {
        /* fb_shutdown() cleared the console columns from its shadow and copied the whole frame
         * back, which cuts into the larger user-mode die. Draw it once more, hardware only. */
        die_draw_all(n);
    } else {
        for (int i = 0; i < DIE_BLOCKS; i++) {
            int was = die_state(&die_block[i], die_shown), now = die_state(&die_block[i], n);

            if (was != now)
                die_block_draw(&die_block[i], now);
        }
    }
    die_after_fb = !console.initialized;
    die_shown = n;
}

void boot_progress(int n)
{
    u32 *hw = (u32 *)cur_boot_args.video.base;

    if (!hw || !cur_boot_args.video.stride || n < 0 || n >= BT_STEPS || n <= die_shown)
        return;
    die_step(n);
#ifndef SIXBOOT_USER
    bt_step(n);
#else
    (void)bt_step; /* the trace is developer-mode only; keeps -Wall quiet in user builds */
#endif
    __asm__ volatile("dsb sy" ::: "memory");
}
