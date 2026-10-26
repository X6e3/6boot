/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef XBOOT_BOOTTRACE_H
#define XBOOT_BOOTTRACE_H
#include <stddef.h>
#include <stdint.h>
#define XBT_CAPACITY 96u
#define XBT_HEADER_BYTES 48u
#define XBT_EVENT_BYTES 32u
#define XBT_BYTES (XBT_HEADER_BYTES + XBT_CAPACITY * XBT_EVENT_BYTES)
#define XBT_PROPERTY "xboot,boot-trace"
#define XBT_NO_RESULT INT32_MIN
enum xbt_kind { XBT_BEGIN=1, XBT_END=2, XBT_POINT=3 };
enum xbt_stage {
    XBT_ENTRY=1, XBT_UART, XBT_CPU, XBT_EXCEPTIONS, XBT_FIRMWARE,
    XBT_HEAP, XBT_GXF, XBT_MCC, XBT_MMU, XBT_AIC, XBT_PMGR,
    XBT_DISPLAY, XBT_FB, XBT_CPUFREQ_FIXUP, XBT_SEP_TRANSPORT,
    XBT_PAYLOAD, XBT_CPUFREQ, XBT_SMP, XBT_MITIGATIONS, XBT_DT,
    XBT_CACHE, XBT_TUNABLES, XBT_CLOCKS, XBT_USB, XBT_PCIE,
    XBT_HANDOFF, XBT_NVME_STOP, XBT_EXCEPTIONS_STOP, XBT_USB_STOP,
    XBT_DISPLAY_STOP, XBT_FB_STOP, XBT_MMU_STOP, XBT_KERNEL_BRANCH,
    XBT_OUTPUTS_RELEASE,
    XBT_STAGE_LAST=XBT_OUTPUTS_RELEASE
};
struct xbt_event {
    uint32_t sequence, stage, kind, flags;
    uint64_t ticks;
    int32_t result;
};
struct xbt_state {
    uint32_t count, dropped, uart_bytes_dropped, flags;
    uint64_t frequency;
    struct xbt_event events[XBT_CAPACITY];
};
void xbt_reset(struct xbt_state *s);
int xbt_append(struct xbt_state *s, unsigned stage, unsigned kind,
               int32_t result, uint64_t ticks, unsigned time_valid);
int xbt_encode(const struct xbt_state *s, void *out, size_t capacity);
const char *xbt_stage_name(unsigned stage);
const char *xbt_kind_name(unsigned kind);
#endif
