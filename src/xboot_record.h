/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XBOOT_RECORD_H
#define XBOOT_RECORD_H
#include "xboot.h"

/* Wire ABI: sixteen big-endian u32 header cells, then twelve phase/error pairs.
 * No addresses, native padding, pointers, serials or hardware assertions. */
#define XB_RECORD_MAGIC 0x58425452U /* XBTR */
#define XB_RECORD_VERSION 1U
#define XB_RECORD_HEADER_WORDS 16U
#define XB_RECORD_WORDS (XB_RECORD_HEADER_WORDS + 2U * XB_TRACE_LEN)
#define XB_RECORD_BYTES (4U * XB_RECORD_WORDS)
#define XB_RECORD_PROPERTY "xboot,handoff-record"
#define XB_RECORD_METADATA_ONLY 1U
#define XB_RECORD_PROXY_BYPASS 2U

struct xb_record_meta {
    uint32_t flags, chip, cpu_count, ram_count, reserved_count;
    uint32_t firmware_reserved_count, mtp_dapf_owner, dart_owner, checks;
};

int xb_encode_record(const struct xb_session *session,
                     const struct xb_record_meta *meta, void *out, size_t size);
#endif
