/* SPDX-License-Identifier: MIT */

#include "../build/build_cfg.h"
#include "../build/build_tag.h"

#include "../config.h"

#include "adt.h"
#include "aic.h"
#include "cpufreq.h"
#include "display.h"
#include "exception.h"
#include "fb.h"
#include "firmware.h"
#include "gxf.h"
#include "heapblock.h"
#include "mcc.h"
#include "memory.h"
#include "nvme.h"
#include "payload.h"
#include "pcie.h"
#include "pmgr.h"
#include "sep.h"
#include "smp.h"
#include "m1n1_adapter.h"
#include "string.h"
#include "tps6598x.h"
#include "uart.h"
#include "uartproxy.h"
#include "usb.h"
#include "utils.h"
#include "boottrace_adapter.h"
#include "wdt.h"
#include "xnuboot.h"

struct vector_args next_stage;

const char version_tag[] = "##m1n1_ver##" BUILD_TAG;
const char *const m1n1_version = version_tag + 12;

u32 board_id = ~0, chip_id = ~0;

void get_device_info(void)
{
    const char *model = (const char *)adt_getprop(adt, 0, "model", NULL);
    const char *target = (const char *)adt_getprop(adt, 0, "target-type", NULL);

    printf("Device info:\n");

    if (model)
        printf("  Model: %s\n", model);

    if (target)
        printf("  Target: %s\n", target);

    is_mac = !!strstr(model, "Mac");

    int chosen = adt_path_offset(adt, "/chosen");
    if (chosen > 0) {
        if (ADT_GETPROP(adt, chosen, "board-id", &board_id) < 0)
            printf("Failed to find board-id\n");
        if (ADT_GETPROP(adt, chosen, "chip-id", &chip_id) < 0)
            printf("Failed to find chip-id\n");

        printf("  Board-ID: 0x%x\n", board_id);
        printf("  Chip-ID: 0x%x\n", chip_id);
    } else {
        printf("No chosen node!\n");
    }

    printf("\n");
}

void run_actions(void)
{
    bool usb_up = false;

#ifndef BRINGUP
#ifdef EARLY_PROXY_TIMEOUT
    int node = adt_path_offset(adt, "/chosen/asmb");
    u64 lp_sip0 = 0;

    if (node >= 0) {
        ADT_GETPROP(adt, node, "lp-sip0", &lp_sip0);
        printf("Boot policy: sip0 = %ld\n", lp_sip0);
    }

    if (!cur_boot_args.video.display && lp_sip0 == 127) {
        printf("Bringing up USB for early debug...\n");

        usb_init();
        usb_iodev_init();

        usb_up = true;

        printf("Waiting for proxy connection... ");
        for (int i = 0; i < EARLY_PROXY_TIMEOUT * 100; i++) {
            for (int j = 0; j < USB_IODEV_COUNT; j++) {
                iodev_id_t iodev = IODEV_USB0 + j;

                if (!(iodev_get_usage(iodev) & USAGE_UARTPROXY))
                    continue;

                usb_iodev_vuart_setup(iodev);
                iodev_handle_events(iodev);
                if (iodev_can_write(iodev) || iodev_can_write(IODEV_USB_VUART)) {
                    printf(" Connected!\n");
                    uartproxy_run(NULL);
                    return;
                }
            }

            mdelay(10);
            if (i % 100 == 99)
                printf(".");
        }
        printf(" Timed out\n");
    }
#endif
#endif

    printf("Checking for payloads...\n");

    xboot_trace(XBT_PAYLOAD,XBT_BEGIN,XBT_NO_RESULT);
    int payload_result=payload_run();
    xboot_trace(XBT_PAYLOAD,XBT_END,payload_result);
    if (payload_result == 0) {
        printf("Valid payload found\n");
        return;
    }
    fb_set_active(true);

    printf("No valid payload found\n");

#ifndef BRINGUP
    if (!usb_up) {
        usb_init();
        usb_iodev_init();
    }
#endif

    printf("Running proxy...\n");

    uartproxy_run(NULL);
}

void m1n1_main(void)
{
    printf("\n\nm1n1 %s\n", m1n1_version);
    printf("Copyright The Asahi Linux Contributors\n");
    printf("Licensed under the MIT license\n\n");

    printf("Running in EL%lu\n\n", mrs(CurrentEL) >> 2);

    xboot_trace(XBT_FIRMWARE,XBT_BEGIN,XBT_NO_RESULT);
    firmware_init();
    xboot_trace(XBT_FIRMWARE,XBT_END,XBT_NO_RESULT);

    xboot_trace(XBT_HEAP,XBT_BEGIN,XBT_NO_RESULT);
    heapblock_init();
    xboot_trace(XBT_HEAP,XBT_END,XBT_NO_RESULT);

#ifndef BRINGUP
    if (supports_gxf()) {
        xboot_trace(XBT_GXF,XBT_BEGIN,XBT_NO_RESULT);
        gxf_init();
        xboot_trace(XBT_GXF,XBT_END,XBT_NO_RESULT);
    }
    xboot_trace(XBT_MCC,XBT_BEGIN,XBT_NO_RESULT);
    mcc_init();
    xboot_trace(XBT_MCC,XBT_END,XBT_NO_RESULT);
    xboot_trace(XBT_MMU,XBT_BEGIN,XBT_NO_RESULT);
    mmu_init();
    xboot_trace(XBT_MMU,XBT_END,XBT_NO_RESULT);
    xboot_trace(XBT_AIC,XBT_BEGIN,XBT_NO_RESULT);
    aic_init();
    xboot_trace(XBT_AIC,XBT_END,XBT_NO_RESULT);
#endif
    wdt_disable();
#ifndef BRINGUP
    xboot_trace(XBT_PMGR,XBT_BEGIN,XBT_NO_RESULT);
    pmgr_init();
    xboot_trace(XBT_PMGR,XBT_END,XBT_NO_RESULT);
#ifdef USE_DEBUG_USB
    tps6598x_enable_debugusb();
#endif
#ifdef USE_FB
    xboot_trace(XBT_DISPLAY,XBT_BEGIN,XBT_NO_RESULT);
    int display_result=display_init();
    xboot_trace(XBT_DISPLAY,XBT_END,display_result);
    // Kick DCP to sleep, so dodgy monitors which cause reconnect cycles don't cause us to lose the
    // framebuffer.
    display_shutdown(DCP_SLEEP_IF_EXTERNAL);
    // On idevice we need to always clear, because otherwise it looks scuffed on white devices
    xboot_trace(XBT_FB,XBT_BEGIN,XBT_NO_RESULT);
    fb_init(!is_mac);
    xboot_trace_fb_ready();
    xboot_trace(XBT_FB,XBT_END,XBT_NO_RESULT);
    fb_display_logo();
#ifdef FB_SILENT_MODE
    fb_set_active(!cur_boot_args.video.display);
#else
    fb_set_active(true);
#endif
#endif

    xboot_trace(XBT_CPUFREQ_FIXUP,XBT_BEGIN,XBT_NO_RESULT);
    cpufreq_fixup();
    xboot_trace(XBT_CPUFREQ_FIXUP,XBT_END,XBT_NO_RESULT);
    xboot_trace(XBT_SEP_TRANSPORT,XBT_BEGIN,XBT_NO_RESULT);
    int sep_result=sep_init();
    xboot_trace(XBT_SEP_TRANSPORT,XBT_END,sep_result);
#endif

    printf("Initialization complete.\n");

    run_actions();

    if (!next_stage.entry) {
        panic("Nothing to do!\n");
    }

    xboot_trace(XBT_HANDOFF,XBT_BEGIN,XBT_NO_RESULT);
    int vector_allowed=xboot_vector_allowed(next_stage.entry, next_stage.args[0],
                              next_stage.args[1], next_stage.args[2],
                              next_stage.args[3], next_stage.args[4]);
    xboot_trace(XBT_HANDOFF,XBT_END,vector_allowed ? 0 : -1);
    if (!vector_allowed)
        panic("6boot: refusing unvalidated kernel handoff\n");

    printf("Preparing to run next stage at %p...\n", next_stage.entry);

    xboot_trace(XBT_NVME_STOP,XBT_BEGIN,XBT_NO_RESULT);
    nvme_shutdown();
    xboot_trace(XBT_NVME_STOP,XBT_END,XBT_NO_RESULT);
    boot_progress(8);
    xboot_trace(XBT_EXCEPTIONS_STOP,XBT_BEGIN,XBT_NO_RESULT);
    exception_shutdown();
    xboot_trace(XBT_EXCEPTIONS_STOP,XBT_END,XBT_NO_RESULT);
    boot_progress(9);
#ifndef BRINGUP
    xboot_trace(XBT_USB_STOP,XBT_BEGIN,XBT_NO_RESULT);
    usb_iodev_shutdown();
    xboot_trace(XBT_USB_STOP,XBT_END,XBT_NO_RESULT);
    boot_progress(10);
    xboot_trace(XBT_DISPLAY_STOP,XBT_BEGIN,XBT_NO_RESULT);
    display_shutdown(DCP_SLEEP_IF_EXTERNAL);
    xboot_trace(XBT_DISPLAY_STOP,XBT_END,XBT_NO_RESULT);
    boot_progress(11);
#ifdef USE_FB
    xboot_trace(XBT_OUTPUTS_RELEASE,XBT_POINT,XBT_NO_RESULT);
    xboot_trace_outputs_off();
    xboot_trace(XBT_FB_STOP,XBT_BEGIN,XBT_NO_RESULT);
    fb_shutdown(next_stage.restore_logo);
    xboot_trace(XBT_FB_STOP,XBT_END,XBT_NO_RESULT);
    boot_progress(12);
#endif
    xboot_trace(XBT_MMU_STOP,XBT_BEGIN,XBT_NO_RESULT);
    xboot_trace_freeze_dt();
    mmu_shutdown();
#endif

    printf("Vectoring to next stage...\n");
    boot_progress(13);

    next_stage.entry(next_stage.args[0], next_stage.args[1], next_stage.args[2], next_stage.args[3],
                     next_stage.args[4]);

    panic("Next stage returned!\n");
}
