/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SIXBOOT_EL2_TRACE_H
#define SIXBOOT_EL2_TRACE_H
#include "el2_control.h"
#define E2_TRACE_CAPACITY 64u
enum e2_event_kind { E2_EVENT_ENTRY = 1, E2_EVENT_CHECK = 2, E2_EVENT_RETURN = 3 };
struct e2_event {
    uint32_t sequence, kind, status, reserved;
    uint64_t ticks, argument;
};
struct e2_trace {
    uint32_t count, dropped;
    struct e2_event event[E2_TRACE_CAPACITY];
};
_Static_assert(sizeof(struct e2_event) == 32, "trace event ABI");
_Static_assert(sizeof(struct e2_trace) <= E2_PAGE_SIZE, "trace page budget");
/* Single-writer in-memory API. Ticks supplied by caller; no hardware access.
 * This phase does not export native trace structs as a wire ABI. */
void e2_trace_reset(struct e2_trace *trace);
int e2_trace_push(struct e2_trace *trace, unsigned kind, uint32_t status,
                  uint64_t ticks, uint64_t argument);
#endif
