/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XBOOT_M1N1_ADAPTER_H
#define XBOOT_M1N1_ADAPTER_H
#include "types.h"

int xboot_input_ok(const void *fdt);
int xboot_begin_dt(const void *fdt);
void xboot_capture_ctrr(u64 start, u64 size);
void xboot_capture_release(int cpu, u64 mpidr, u64 release);
int xboot_dt_prepared(void);
int xboot_prepare_record(void *dt, size_t capacity);
void xboot_dt_failed(int result);
int xboot_check_handoff(void *kernel, void *dt, size_t dt_capacity,
                        void *initrd, size_t initrd_size);
int xboot_arm_handoff(void *kernel, void *dt);
void xboot_proxy_override(void);
int xboot_vector_allowed(void *kernel, u64 dt, u64 arg1, u64 arg2, u64 arg3, u64 arg4);
#endif
