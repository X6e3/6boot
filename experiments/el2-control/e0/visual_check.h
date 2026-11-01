/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef E0_VISUAL_CHECK_H
#define E0_VISUAL_CHECK_H
#include <stddef.h>
#include <stdint.h>
/* Independent wire reader, used before the target displays PASS. */
int ew_visual_trace_ok(const void *bytes, size_t size, uint64_t build,
                       uint64_t arena, const uint64_t pcs[4]);
#endif
