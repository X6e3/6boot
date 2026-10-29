/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 6boot P3 on the device: load the separately built 6boot entry probe, call it
 * once on the boot CPU and record that it returned.
 *
 * The probe is its own position-independent image (sources/6boot/p3-entry):
 * code at offset 0 and its 16 KiB stack at offset 0x4000, reached through
 * adrp. No m1n1 object is linked into it. This adapter only places the image,
 * builds the v1 context from J713's own ADT and boot arguments, masks
 * interrupts for the call, and writes the 64-byte result into the device tree
 * so Linux can read it from /proc/device-tree/chosen.
 *
 * Identity-mapped RAM is RWX here: the M4 feature set does not enable SPRR.
 */
#include "entry_probe.h"
#include "adt.h"
#include "libfdt/libfdt.h"
#include "malloc.h"
#include "memory.h"
#include "soc.h"
#include "string.h"
#include "utils.h"
#include "xnuboot.h"
#include "../build/build_cfg.h"
#ifdef SIXBOOT_EL2_NATIVE
#include "el2_native.h"
#endif

#include "../build/entry_probe_image.h"

#define P3_IMAGE_ALIGN  0x10000
#define P3_IMAGE_SIZE   0x8000
#define P3_STACK_OFFSET 0x4000
#define P3_CONTEXT_SIZE (64 + 16)
#define P3_RESULT_SIZE  64

extern char _base[], _end[];

typedef int (*p3_entry_t)(const void *context, size_t context_size, void *result,
                          size_t result_capacity);

static void put32(u8 *p, u32 v)
{
    for (int i = 0; i < 4; i++)
        p[i] = v >> (8 * i);
}

static void put64(u8 *p, u64 v)
{
    put32(p, v);
    put32(p + 4, v >> 32);
}

static void entry_probe_record(void *dt, const u8 *context, const u8 *result, u32 ret, u64 ticks,
                      u64 load)
{
    int node = fdt_path_offset(dt, "/chosen");

    if (node < 0) {
        printf("6boot P3: no /chosen in the DT, result not recorded\n");
        return;
    }
    if (fdt_setprop(dt, node, "6boot,p3-context", context, P3_CONTEXT_SIZE) ||
        fdt_setprop(dt, node, "6boot,p3-result", result, P3_RESULT_SIZE) ||
        fdt_setprop_u32(dt, node, "6boot,p3-return", ret) ||
        fdt_setprop_u64(dt, node, "6boot,p3-ticks", ticks) ||
        fdt_setprop_u64(dt, node, "6boot,p3-load-address", load) ||
        fdt_setprop_string(dt, node, "6boot,p3-image-sha256", entry_probe_image_sha256))
        printf("6boot P3: failed to record the result in the DT\n");
}

int entry_probe_run(void *dt)
{
    u8 context[P3_CONTEXT_SIZE];
    u8 result[P3_RESULT_SIZE];
    const char *target = adt_getprop(adt, 0, "target-type", NULL);
    u64 daif, t0, t1;
    u8 *image;
    int ret;

    if (sizeof(entry_probe_image) > P3_STACK_OFFSET) {
        printf("6boot P3: image text 0x%lx overlaps its stack\n", sizeof(entry_probe_image));
        return -1;
    }

    image = memalign(P3_IMAGE_ALIGN, P3_IMAGE_SIZE);
    if (!image) {
        printf("6boot P3: out of memory\n");
        return -1;
    }
    memset(image, 0, P3_IMAGE_SIZE);
    memcpy(image, entry_probe_image, sizeof(entry_probe_image));
    dc_cvau_range(image, P3_IMAGE_SIZE);
    sysop("dsb ish");
    ic_ivau_range(image, sizeof(entry_probe_image));
    sysop("dsb ish");
    sysop("isb");

    /* Context ABI v1. Chip and board come from the device, not from constants. */
    memset(context, 0, sizeof(context));
    put32(context + 0, 0x58504354);
    put32(context + 4, (64u << 16) | 1);
    put32(context + 8, sizeof(context));
    put32(context + 12, chip_id);
    if (target)
        memcpy(context + 16, target, min(strlen(target), (size_t)4));
    put64(context + 24, (u64)_base);
    put64(context + 32, (u64)_end - (u64)_base);
    put64(context + 40, (u64)adt);
    put64(context + 48, cur_boot_args.devtree_size);
    put32(context + 56, 1);
    put64(context + 64, ram_base);
    put64(context + 72, mem_size_actual);

    /* A pattern the probe must overwrite, so a return without a result shows. */
    memset(result, 0xa5, sizeof(result));

    printf("6boot P3: calling entry at %p (image sha256 %s)\n", image, entry_probe_image_sha256);
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t0)::"memory");
    ret = ((p3_entry_t)image)(context, sizeof(context), result, sizeof(result));
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t1)::"memory");
    __asm__ volatile("msr daif, %0" ::"r"(daif) : "memory");

    printf("6boot P3: entry returned %d; result magic 0x%x status %u EL 0x%x MPIDR 0x%x, "
           "%lu ticks\n",
           ret, *(u32 *)result, *(u32 *)(result + 12), *(u32 *)(result + 16),
           *(u32 *)(result + 20), t1 - t0);

    entry_probe_record(dt, context, result, (u32)ret, t1 - t0, (u64)image);
    free(image);
#ifdef SIXBOOT_EL2_NATIVE
    if(ret || *(u32 *)result!=0x58505242 || *(u32 *)(result+12)!=0)
        return -1;
    return el2_native_run(dt);
#endif
    return ret;
}
