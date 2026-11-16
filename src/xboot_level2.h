/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef XBOOT_LEVEL2_H
#define XBOOT_LEVEL2_H
#include <stddef.h>

/*
 * The trial check from stage 2's payload variables (see xboot_level2.c). load is payload.c's own
 * payload loader: with SIXBOOT_LEVEL2_TRIAL, a trial that is chosen and whose try was written goes
 * through it after 6boot's own payload, so its kernel, device tree, initramfs and boot arguments are
 * the ones booted. Without SIXBOOT_LEVEL2_TRIAL the check only logs.
 */
void xboot_level2_check(char **vars, size_t n, void *(*load)(void *start, size_t size));
#endif
