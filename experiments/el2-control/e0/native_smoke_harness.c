/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Diskless single-PE QEMU fixture. Calls the actual native smoke runner once. */
#include "fb.h"
#include "iodev.h"
#include "xnuboot.h"
#include "visual_smoke.h"
#include "e0_visual_build.h"
#include "libfdt/libfdt.h"
#include "string.h"

#define WIDTH 640u
#define HEIGHT 480u
#define STRIDE 656u
#define GUARD 256u
#define PIXEL UINT32_C(0x101820)
#define READ(reg) ({ uint64_t value; __asm__ volatile("mrs %0, " #reg : "=r"(value)); value; })

/* Only fixture parameters are changed through the local debugger. Production
 * code receives ordinary boot-video metadata and ordinary FDT buffers. */
volatile uint32_t native_smoke_case;
volatile uint32_t native_smoke_publication;
volatile uint32_t native_smoke_fault;
unsigned char native_smoke_pixels[GUARD + STRIDE * HEIGHT * 4 + GUARD]
    __attribute__((aligned(0x4000)));
const uint64_t native_smoke_pixel_bytes = sizeof(native_smoke_pixels);
struct boot_args cur_boot_args;
struct e0_visual_smoke_v2 native_smoke_result;
struct fb_e0_translation native_smoke_translation;
unsigned char native_smoke_dt[4096] __attribute__((aligned(8)));
static unsigned checks, errors, unexpected_locks;
extern void test_exit(unsigned status) __attribute__((noreturn));

static void print(const char *text)
{
    register uint64_t x0 __asm__("x0") = 4;
    register const char *x1 __asm__("x1") = text;
    __asm__ volatile("hlt #0xf000" : "+r"(x0), "+r"(x1) :: "memory");
}

static void hex(uint64_t value)
{
    char text[19] = "0x0000000000000000";
    const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; i++) text[17-i] = digits[(value >> (4*i)) & 15];
    print(text);
}

static void wire(const char *key, const void *record, unsigned bytes)
{
    const unsigned char *data = record;
    const char digits[] = "0123456789abcdef";
    char text[129];
    print(key);
    for (unsigned off = 0; off < bytes;) {
        unsigned n = 0;
        while (n < 128 && off < bytes) {
            text[n++] = digits[data[off] >> 4];
            text[n++] = digits[data[off++] & 15];
        }
        text[n] = 0;
        print(text);
    }
    print("\n");
}

static void check(int valid, const char *name)
{
    checks++;
    if (!valid) {
        errors++;
        print("CHECK_FAIL "); print(name); print("\n");
    }
}

/* The console remains uninitialized. These satisfy unused runtime branches;
 * their invocation would fail this single-owner, single-PE fixture. */
void iodev_lock(iodev_id_t id) { (void)id; unexpected_locks++; }
void iodev_unlock(iodev_id_t id) { (void)id; unexpected_locks++; }

/* start.S contains an unused native caller in the same kept start section. */
void el2_native_invoke(void)
{
    print("UNEXPECTED_NATIVE_GATE\n");
    test_exit(96);
}

struct state { uint64_t hcr, par, daif, sctlr, tcr, ttbr0, ttbr1, mair; };
static struct state snapshot(void)
{
    return (struct state){READ(HCR_EL2), READ(PAR_EL1), READ(DAIF), READ(SCTLR_EL2),
        READ(TCR_EL2), READ(TTBR0_EL2), READ(TTBR1_EL2), READ(MAIR_EL2)};
}

void test_main(void)
{
    print("NATIVE_SMOKE_QEMU_START\n");
    check(READ(CurrentEL) == 8, "EL2 entry");
    check(sizeof(native_smoke_pixels) > 1024 * 1024, "owned pixel RAM exceeds 1 MiB");
    memset(native_smoke_pixels, 0xa5, sizeof(native_smoke_pixels));
    uint32_t *pixels = (uint32_t *)(native_smoke_pixels + GUARD);
    for (unsigned i = 0; i < STRIDE * HEIGHT; i++) pixels[i] = PIXEL;
    cur_boot_args.phys_base = 0x40000000; cur_boot_args.mem_size = 128 * 1024 * 1024;
    cur_boot_args.top_of_kernel_data = 0x40000000;
    cur_boot_args.video = (struct boot_video){.base = (uint64_t)pixels,
        .width = WIDTH, .height = HEIGHT, .stride = STRIDE * 4, .depth = 32};
    if (native_smoke_case == 1) cur_boot_args.video.width = 399;
    if (native_smoke_case == 2) cur_boot_args.video.base = UINT64_C(0x90000000);
    if (native_smoke_case == 5) cur_boot_args.top_of_kernel_data = UINT64_C(0x47ffc000);

    check(fdt_create_empty_tree(native_smoke_dt, sizeof(native_smoke_dt)) == 0, "create real FDT");
    int node = fdt_add_subnode(native_smoke_dt, 0, "chosen");
    check(node >= 0, "create chosen");
    int reserved = 0;
    if (native_smoke_publication == 2) check(fdt_pack(native_smoke_dt) == 0, "pack before reservation");
    if (native_smoke_publication != 1) reserved = e0_visual_smoke_reserve(native_smoke_dt);
    check(reserved == (native_smoke_publication == 2 ? -FDT_ERR_NOSPACE : 0), "actual reserve errno");

    /* An unmasked IRQ bit makes omission of the real DAIF restore observable.
     * QEMU runs one PE with no devices, interrupt sources, disks, or network. */
    __asm__ volatile("msr daifclr, #2; isb" ::: "memory");
    struct state before = snapshot();
    e0_visual_smoke_run(&native_smoke_result, &native_smoke_translation);
    struct state after = snapshot();
    struct e0_visual_smoke_v2 *r = &native_smoke_result;
    check(r->v1.magic == UINT32_C(0x53563045) && r->v1.version == 2 &&
          r->v1.size == sizeof(*r), "v2 identity");
    check(r->v1.build == E0_VISUAL_BUILD_ID, "actual runner build identity");
    check(r->saved_par == before.par && r->restored_par == after.par, "actual PAR snapshots");
    check(r->daif_before == before.daif && r->daif_after == after.daif, "actual DAIF snapshots");
    check(before.sctlr == after.sctlr && before.tcr == after.tcr &&
          before.ttbr0 == after.ttbr0 && before.ttbr1 == after.ttbr1 &&
          before.mair == after.mair, "unchanged host translation regime");
    if (!native_smoke_fault) {
        check(before.hcr == after.hcr && before.par == after.par && before.daif == after.daif,
              "unchanged HCR PAR DAIF");
        check(r->safety == E0VS_SAFE && !r->context_changed, "verified context flags");
        if (native_smoke_case == 0)
            check(r->outcome == E0VS_COMPLETE && r->v1.frames == 3 && !r->v1.error && !r->reason,
                  "actual runner completes three frames");
        else {
            check(r->outcome == E0VS_REFUSED && !r->v1.frames && r->v1.error == 2,
                  "actual runner safely refuses first frame");
            if (native_smoke_case == 1) {
                check(r->reason == FB_E0_GEOMETRY && r->stage == FB_E0_STAGE_GEOMETRY,
                      "geometry first reason");
                check(!(r->diagnostic_flags & FB_E0_PAR_TOUCHED), "geometry refused before AT");
            } else if (native_smoke_case == 2) {
                check(r->reason == FB_E0_TRANSLATION && r->stage == FB_E0_STAGE_TRANSLATE,
                      "actual AT translation refusal");
                check((r->diagnostic_flags & (FB_E0_PAR_TOUCHED | FB_E0_PAR_VALID | FB_E0_VA_VALID)) ==
                      (FB_E0_PAR_TOUCHED | FB_E0_PAR_VALID | FB_E0_VA_VALID) &&
                      (r->raw_par & 1) && r->failed_va >= UINT64_C(0x90000000),
                      "raw faulting AT diagnostic");
            } else if (native_smoke_case == 3) {
                check(r->reason == FB_E0_ATTRIBUTE && r->stage == FB_E0_STAGE_TRANSLATE,
                      "actual memory attribute refusal");
                check(native_smoke_translation.status == FB_E0_WALK_OK &&
                      native_smoke_translation.attribute != FB_E0_ATTR_OK,
                      "descriptor and MAIR evidence precede attribute refusal");
            } else {
                check(r->reason == FB_E0_REGIME && r->stage == FB_E0_STAGE_REGIME,
                      "owned-table inspection refuses before draw");
                check(native_smoke_translation.status ==
                      (native_smoke_case == 4 ? FB_E0_WALK_ROOT : FB_E0_WALK_BOUNDS),
                      "first failed ownership or RAM/heap bound predicate");
            }
        }
    } else {
        check(r->outcome == E0VS_FAILED && r->context_changed, "injected architectural state detected");
        check(r->safety != E0VS_SAFE, "unsafe context cannot claim full safety");
        if (native_smoke_fault == 1)
            check(before.par != after.par && (r->context_changed & 2) &&
                  r->reason == FB_E0_PAR_RESTORE && !r->v1.frames,
                  "skipped actual PAR restore blocks before draw");
        if (native_smoke_fault == 2)
            check(before.daif != after.daif && (r->context_changed & 4) &&
                  r->reason == FB_E0_DAIF_RESTORE && !r->v1.frames,
                  "skipped actual DAIF restore blocks after first draw");
        if (native_smoke_fault == 3)
            check(before.hcr != after.hcr && (r->context_changed & 1) &&
                  r->reason == FB_E0_HCR_CHANGED && r->v1.frames == 1,
                  "actual HCR change after first draw blocks next draw");
    }

    const uint32_t first_error = r->v1.error, first_reason = r->reason, outcome = r->outcome;
    if (reserved) {
        r->publication = E0VS_PUB_FAILED;
        r->publication_error = reserved;
        r->publication_step = 1;
    }
    int pub = e0_visual_smoke_publish(native_smoke_dt, r, &native_smoke_translation);
    check(r->v1.error == first_error && r->reason == first_reason && r->outcome == outcome,
          "publication preserves first experiment result");
    unsigned expected = native_smoke_fault ? E0VS_BLOCK_CONTEXT :
                        native_smoke_publication ? E0VS_BLOCK_EVIDENCE : E0VS_CONTINUE;
    check(r->decision == expected && e0_visual_smoke_decision(r, 1, &native_smoke_translation) == expected,
          "actual publication and boot policy agree");
    check(e0_visual_smoke_decision(r, 0, &native_smoke_translation) == E0VS_BLOCK_P3, "failed P3 always blocks");
    check(pub == (native_smoke_publication == 1 ? -FDT_ERR_NOTFOUND :
                  native_smoke_publication == 2 ? -FDT_ERR_NOSPACE : 0), "actual final FDT errno");
    int size = 0;
    const void *receipt = fdt_getprop(native_smoke_dt, fdt_path_offset(native_smoke_dt, "/chosen"),
                                      "6boot,e0-visual-result", &size);
    if (!native_smoke_publication) {
        check(r->publication == E0VS_PUB_COMPLETE && !r->publication_error && r->publication_step == 2,
              "publication complete only after exact final copy");
        check(receipt && size == (int)sizeof(*r) && !memcmp(receipt, r, sizeof(*r)),
              "actual FDT receipt equals in-memory result");
        int translation_bytes = 0;
        const void *translation_receipt = fdt_getprop(native_smoke_dt,
            fdt_path_offset(native_smoke_dt, "/chosen"), "6boot,e0-translation-result", &translation_bytes);
        check(translation_receipt && translation_bytes == (int)sizeof(native_smoke_translation) &&
              !memcmp(translation_receipt, &native_smoke_translation, sizeof(native_smoke_translation)),
              "actual companion FDT receipt equals in-memory translation");
        if (translation_receipt && translation_bytes == (int)sizeof(native_smoke_translation))
            wire("WIRE_FDT_XLAT=", translation_receipt, sizeof(native_smoke_translation));
    } else {
        check(!receipt && size == -FDT_ERR_NOTFOUND, "failed publication leaves no complete receipt");
        check(r->publication == E0VS_PUB_FAILED && r->publication_error == pub &&
              r->publication_step == (native_smoke_publication == 2 ? 1u : 2u),
              "publication failure stage and errno retained");
    }
    for (unsigned i = 0; i < GUARD; i++)
        if (native_smoke_pixels[i] != 0xa5 || native_smoke_pixels[GUARD + STRIDE * HEIGHT * 4 + i] != 0xa5)
            errors++;
    if (native_smoke_case || native_smoke_fault == 1)
        for (unsigned i = 0; i < STRIDE * HEIGHT; i++) if (pixels[i] != PIXEL) errors++;
    check(!unexpected_locks, "console stays uninitialized");
    wire("WIRE_XLAT=", &native_smoke_translation, sizeof(native_smoke_translation));
    wire("WIRE_RESULT=", r, sizeof(*r));
    if (receipt && size == (int)sizeof(*r)) wire("WIRE_FDT=", receipt, sizeof(*r));
    print("CHECKS="); hex(checks); print(" ERRORS="); hex(errors); print("\n");
    print(errors ? "NATIVE_SMOKE_QEMU_FAIL\n" : "NATIVE_SMOKE_QEMU_PASS\n");
    test_exit(errors ? 1 : 0);
}
