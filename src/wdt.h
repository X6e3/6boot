/* SPDX-License-Identifier: MIT */

#ifndef __WDT_H__
#define __WDT_H__

#include "types.h"

void wdt_disable(void);
void wdt_reboot(void);
u64 wdt_arm(u32 seconds); /* SIXBOOT_LEVEL2_WATCHDOG builds only */

#endif
