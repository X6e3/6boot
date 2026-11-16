/* SPDX-License-Identifier: MIT */

#ifndef __PAYLOAD_H__
#define __PAYLOAD_H__

#include "types.h"

bool payload_logo(void **custom_128, void **custom_256);

int payload_run(void);

/* 6boot: the trial check of installer level 2, log only (xboot_level2.c). It takes the place of a
 * blank line in payload.c, so that file's line numbers, and with them its assert messages, stay as
 * they are in builds without it. */
#ifdef SIXBOOT_LEVEL2
#include "xboot_level2.h"
#define XB_LEVEL2_CHECK(vars, n) xboot_level2_check(vars, n)
#else
#define XB_LEVEL2_CHECK(vars, n) ((void)0)
#endif

#endif
