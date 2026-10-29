/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SIXBOOT_NATIVE_ADAPTER_H
#define SIXBOOT_NATIVE_ADAPTER_H
#include "types.h"
int el2_native_run(void *dt);
void el2_native_set_kernel(const void *kernel);
void el2_native_set_ctrr(uint64_t start,uint64_t size);
int el2_native_handoff_allowed(void);
#endif
