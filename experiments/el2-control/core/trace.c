/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "el2_trace.h"

void e2_trace_reset(struct e2_trace *trace)
{
    if (!trace) return;
    uint8_t *p = (uint8_t *)trace;
    for (size_t i = 0; i < sizeof(*trace); i++) p[i] = 0;
}

int e2_trace_push(struct e2_trace *trace, unsigned kind, uint32_t status,
                  uint64_t ticks, uint64_t argument)
{
    if (!trace || kind < E2_EVENT_ENTRY || kind > E2_EVENT_RETURN ||
        trace->count > E2_TRACE_CAPACITY) return E2_ARGUMENT;
    if (trace->count == E2_TRACE_CAPACITY) {
        if (trace->dropped != UINT32_MAX) trace->dropped++;
        return E2_CAPACITY;
    }
    struct e2_event *e = &trace->event[trace->count];
    e->sequence = trace->count + 1;
    e->kind = kind;
    e->status = status;
    e->reserved = 0;
    e->ticks = ticks;
    e->argument = argument;
    trace->count++;
    return E2_READY;
}
