/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef XBOOT_BOOTTRACE_ADAPTER_H
#define XBOOT_BOOTTRACE_ADAPTER_H
#include "xboot_boottrace.h"
void xboot_trace_init(void);
void xboot_trace(unsigned stage,unsigned kind,int result);
void xboot_trace_uart_ready(int ready);
void xboot_trace_timer_ready(void);
void xboot_trace_fb_ready(void);
void xboot_trace_outputs_off(void);
void xboot_trace_dt_clear(void);
void xboot_trace_dt_prepare(void *dt,size_t capacity);
void xboot_trace_freeze_dt(void);
#endif
