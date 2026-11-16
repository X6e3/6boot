/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef XBOOT_LEVEL2_H
#define XBOOT_LEVEL2_H
#include <stddef.h>

/* Log-only trial check from stage 2's payload variables (see xboot_level2.c). */
void xboot_level2_check(char **vars, size_t n);
#endif
