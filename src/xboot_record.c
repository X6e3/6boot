/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "xboot_record.h"
_Static_assert(XB_RECORD_BYTES == 160, "Wire ABI size must not silently change");

static void put_be32(uint8_t *out, uint32_t value)
{
    out[0] = value >> 24;
    out[1] = value >> 16;
    out[2] = value >> 8;
    out[3] = value;
}

int xb_encode_record(const struct xb_session *s, const struct xb_record_meta *m,
                     void *out, size_t size)
{
    if (!s || !m || !out || size != XB_RECORD_BYTES || !s->trace_count ||
        s->trace_count > XB_TRACE_LEN || s->phase > XB_FAILED || s->error > XB_E_RECORD)
        return -1;
    const uint32_t header[XB_RECORD_HEADER_WORDS] = {
        XB_RECORD_MAGIC, XB_RECORD_VERSION, XB_RECORD_BYTES,
        s->phase, s->error, s->trace_count, XB_TRACE_LEN,
        m->flags, m->chip, m->cpu_count, m->ram_count, m->reserved_count,
        m->firmware_reserved_count, m->mtp_dapf_owner, m->dart_owner, m->checks,
    };
    uint8_t *p = out;
    for (unsigned i = 0; i < XB_RECORD_HEADER_WORDS; i++)
        put_be32(p + 4 * i, header[i]);
    for (unsigned i = 0; i < XB_TRACE_LEN; i++) {
        put_be32(p + 4 * (XB_RECORD_HEADER_WORDS + 2 * i),
                 i < s->trace_count ? s->trace[i].phase : 0);
        put_be32(p + 4 * (XB_RECORD_HEADER_WORDS + 2 * i + 1),
                 i < s->trace_count ? s->trace[i].error : 0);
    }
    return 0;
}
