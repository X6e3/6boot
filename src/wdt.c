/* SPDX-License-Identifier: MIT */

#include "../build/build_cfg.h" /* SIXBOOT_LEVEL2_WATCHDOG comes from CFG */
#include "wdt.h"
#include "adt.h"
#include "types.h"
#include "utils.h"

#define WDT_COUNT 0x10
#define WDT_ALARM 0x14
#define WDT_CTL   0x1c

static u64 wdt_base = 0;

void wdt_disable(void)
{
    int path[8];
    int node = adt_path_offset_trace(adt, "/arm-io/wdt", path);

    if (node < 0) {
        printf("WDT node not found!\n");
        return;
    }

    if (adt_get_reg(adt, path, "reg", 0, &wdt_base, NULL)) {
        printf("Failed to get WDT reg property!\n");
        return;
    }

    printf("Primary WDT register @ 0x%lx\n", wdt_base);
    write32(wdt_base + WDT_CTL, 0);
    printf("Primary WDT disabled\n");

    // disable secondary watchdog if wdt-version is 2 or 3
    u32 wdt_version;
    if (ADT_GETPROP(adt, node, "wdt-version", &wdt_version) < 0)
        return;

    if (wdt_version == 2 || wdt_version == 3) {
        u64 wdt_2nd = 0;
        if (adt_get_reg(adt, path, "reg", 2, &wdt_2nd, NULL)) {
            printf("Failed to get WDT reg[2] property!\n");
            return;
        }

        printf("Secondary WDT register @ 0x%lx\n", wdt_2nd);
        write32(wdt_2nd, 0);
        printf("Secondary WDT disabled\n");
    }
}

void wdt_reboot(void)
{
    if (!wdt_base)
        return;

    write32(wdt_base + WDT_ALARM, 0x100000);
    write32(wdt_base + WDT_COUNT, 0);
    write32(wdt_base + WDT_CTL, 4);
}

#ifdef SIXBOOT_LEVEL2_WATCHDOG
#ifndef SIXBOOT_LEVEL2_TRIAL
#error "SIXBOOT_LEVEL2_WATCHDOG arms the watchdog for trials: it needs SIXBOOT_LEVEL2_TRIAL"
#endif
/* The J713's (t8132) watchdog runs on the 24 MHz reference clock, as Linux's device tree says (clkref). */
#define WDT_HZ 24000000U

/*
 * 6boot trial boots: arm the primary watchdog, the one wdt_disable() turned off at start (Linux's
 * apple_wdt calls it WD1), to reset the machine `seconds` from now unless something feeds it; Linux's
 * apple_wdt does from the moment it probes, and a reset ends in the first stage, whose m1n1 turns it
 * off again. The same three registers wdt_reboot() writes, with a long alarm. Returns the counter
 * (CNTPCT_EL0, 24 MHz) at the moment it was enabled, or 0 when there is no watchdog or seconds is out
 * of range; nothing is armed then.
 */
u64 wdt_arm(u32 seconds)
{
    if (!wdt_base || !seconds || seconds > 0xffffffffU / WDT_HZ)
        return 0;

    write32(wdt_base + WDT_CTL, 0);
    write32(wdt_base + WDT_ALARM, seconds * WDT_HZ);
    write32(wdt_base + WDT_COUNT, 0);
    u64 t = get_ticks();
    write32(wdt_base + WDT_CTL, 4);
    return t;
}
#endif
