/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include "xboot_bootstate.h"

_Static_assert(XB_BS_BYTES == 512, "the record is one 512-byte file");

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

uint32_t xb_bs_crc32(const void *buf, size_t len)
{
    const uint8_t *p = buf;
    uint32_t crc = 0xffffffffU;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

static int all_zero(const uint8_t *p, size_t n)
{
    while (n--)
        if (*p++)
            return 0;
    return 1;
}

/* Field rules shared by parse and encode. */
static int fields_ok(const struct xb_bootstate *bs)
{
    if (bs->flags)
        return 0;
    if (bs->state == XB_BS_IDLE)
        return !bs->tries_left && !bs->tries_set && !bs->trial_size &&
               all_zero(bs->trial_sha256, XB_BS_HASH_BYTES);
    if (bs->state == XB_BS_ARMED)
        return bs->tries_set >= 1 && bs->tries_set <= XB_BS_MAX_TRIES &&
               bs->tries_left <= bs->tries_set && bs->trial_size;
    return 0;
}

int xb_bs_parse(const void *buf, size_t len, struct xb_bootstate *out)
{
    const uint8_t *p = buf;
    struct xb_bootstate bs;

    if (!p || len != XB_BS_BYTES || get_le16(p + 6) != XB_BS_BYTES)
        return XB_BS_E_SIZE;
    if (get_le32(p) != XB_BS_MAGIC)
        return XB_BS_E_MAGIC;
    if (get_le16(p + 4) != XB_BS_VERSION)
        return XB_BS_E_VERSION;
    if (get_le32(p + 508) != xb_bs_crc32(p, 508))
        return XB_BS_E_CRC;
    bs.seq = get_le32(p + 8);
    bs.state = p[12];
    bs.tries_left = p[13];
    bs.tries_set = p[14];
    bs.flags = p[15];
    bs.trial_size = get_le32(p + 16);
    memcpy(bs.trial_sha256, p + 20, XB_BS_HASH_BYTES);
    if (!fields_ok(&bs) || !all_zero(p + 52, 508 - 52))
        return XB_BS_E_FIELD;
    if (out)
        *out = bs;
    return XB_BS_OK;
}

int xb_bs_encode(const struct xb_bootstate *bs, uint8_t out[XB_BS_BYTES])
{
    if (!bs || !out || !fields_ok(bs))
        return -1;
    memset(out, 0, XB_BS_BYTES);
    put_le32(out, XB_BS_MAGIC);
    put_le16(out + 4, XB_BS_VERSION);
    put_le16(out + 6, XB_BS_BYTES);
    put_le32(out + 8, bs->seq);
    out[12] = bs->state;
    out[13] = bs->tries_left;
    out[14] = bs->tries_set;
    out[15] = bs->flags;
    put_le32(out + 16, bs->trial_size);
    memcpy(out + 20, bs->trial_sha256, XB_BS_HASH_BYTES);
    put_le32(out + 508, xb_bs_crc32(out, 508));
    return 0;
}

int xb_bs_wants_trial(const void *record, size_t len)
{
    struct xb_bootstate bs;
    return xb_bs_parse(record, len, &bs) == XB_BS_OK && bs.state == XB_BS_ARMED && bs.tries_left;
}

static void own(struct xb_bs_decision *d, enum xb_bs_show show, enum xb_bs_reason reason)
{
    d->boot = XB_BS_BOOT_OWN;
    d->show = show;
    d->reason = reason;
    d->write = 0;
    d->watchdog = 0;
}

void xb_bs_decide(const struct xb_bs_input *in, struct xb_bs_decision *d)
{
    struct xb_bootstate bs;

    memset(d, 0, sizeof(*d));
    d->parse = -1;
    if (!in || !in->record_found) {
        own(d, XB_BS_SHOW_NONE, XB_BS_R_NO_RECORD);
        return;
    }
    d->parse = xb_bs_parse(in->record, in->record_len, &bs);
    if (d->parse != XB_BS_OK) {
        own(d, XB_BS_SHOW_REFUSED, XB_BS_R_BAD_RECORD);
        return;
    }
    if (bs.state == XB_BS_IDLE) {
        own(d, XB_BS_SHOW_NONE, XB_BS_R_IDLE);
        return;
    }
    if (!bs.tries_left) {
        own(d, XB_BS_SHOW_FAILED, XB_BS_R_TRIAL_FAILED);
        return;
    }
    switch (in->trial) {
        case XB_BS_TRIAL_OK:
            break;
        case XB_BS_TRIAL_MISSING:
            own(d, XB_BS_SHOW_REFUSED, XB_BS_R_TRIAL_MISSING);
            return;
        case XB_BS_TRIAL_SIZE:
        case XB_BS_TRIAL_HASH:
            own(d, XB_BS_SHOW_REFUSED, XB_BS_R_TRIAL_MISMATCH);
            return;
        default:
            own(d, XB_BS_SHOW_REFUSED, XB_BS_R_UNCHECKED);
            return;
    }
    if (!in->sector_ok) {
        own(d, XB_BS_SHOW_REFUSED, XB_BS_R_SECTOR);
        return;
    }
    bs.tries_left--;
    bs.seq++;
    if (xb_bs_encode(&bs, d->out)) { /* cannot happen for a record that parsed */
        own(d, XB_BS_SHOW_REFUSED, XB_BS_R_BAD_RECORD);
        return;
    }
    d->boot = XB_BS_BOOT_TRIAL;
    d->show = XB_BS_SHOW_TRIAL;
    d->reason = XB_BS_R_TRIAL;
    d->write = 1;
    d->watchdog = 1;
}

void xb_bs_write_failed(struct xb_bs_decision *d)
{
    own(d, XB_BS_SHOW_REFUSED, XB_BS_R_WRITE_FAILED);
}

const char *xb_bs_reason_name(enum xb_bs_reason r)
{
    static const char *const names[XB_BS_R_COUNT] = {
        [XB_BS_R_NO_RECORD] = "no-record",
        [XB_BS_R_BAD_RECORD] = "bad-record",
        [XB_BS_R_IDLE] = "idle",
        [XB_BS_R_TRIAL] = "trial",
        [XB_BS_R_TRIAL_FAILED] = "trial-failed",
        [XB_BS_R_TRIAL_MISSING] = "trial-missing",
        [XB_BS_R_TRIAL_MISMATCH] = "trial-mismatch",
        [XB_BS_R_SECTOR] = "sector-mismatch",
        [XB_BS_R_WRITE_FAILED] = "write-failed",
        [XB_BS_R_UNCHECKED] = "unchecked",
    };
    return (unsigned)r < XB_BS_R_COUNT ? names[r] : "unknown";
}

int xb_bs_locate(const uint8_t bpb[512], uint64_t part_first, uint64_t part_blocks,
                 uint32_t block_bytes, uint32_t first_cluster, uint32_t file_size,
                 uint64_t *block)
{
    if (!bpb || !block || !block_bytes || (block_bytes & (block_bytes - 1)))
        return XB_BS_LOC_BPB;
    if (file_size != XB_BS_BYTES || block_bytes < XB_BS_BYTES)
        return XB_BS_LOC_FILE;
    if (bpb[510] != 0x55 || bpb[511] != 0xaa)
        return XB_BS_LOC_BPB;

    uint32_t bps = get_le16(bpb + 11), spc = bpb[13], reserved = get_le16(bpb + 14);
    uint32_t fats = bpb[16], root_entries = get_le16(bpb + 17);
    uint32_t total = get_le16(bpb + 19), fat_size = get_le16(bpb + 22);
    if (!total)
        total = get_le32(bpb + 32);
    if (!fat_size)
        fat_size = get_le32(bpb + 36);
    if (bps < 512 || bps > 4096 || (bps & (bps - 1)) || !spc || (spc & (spc - 1)) || !reserved ||
        !fats || fats > 2 || !fat_size || !total)
        return XB_BS_LOC_BPB;

    uint64_t root_sectors = ((uint64_t)root_entries * 32 + bps - 1) / bps;
    uint64_t first_data = reserved + (uint64_t)fats * fat_size + root_sectors;
    if (first_data >= total)
        return XB_BS_LOC_BPB;
    uint64_t clusters = (total - first_data) / spc;
    if (first_cluster < 2 || first_cluster - 2ULL >= clusters)
        return XB_BS_LOC_CLUSTER;

    uint64_t cluster_bytes = (uint64_t)spc * bps;
    uint64_t offset = (first_data + (uint64_t)(first_cluster - 2) * spc) * bps;
    if (cluster_bytes < block_bytes || offset % block_bytes)
        return XB_BS_LOC_ALIGN;
    uint64_t rel = offset / block_bytes;
    if ((offset + block_bytes) > (uint64_t)total * bps || rel >= part_blocks)
        return XB_BS_LOC_BOUNDS;
    *block = part_first + rel;
    return XB_BS_LOC_OK;
}

int xb_bs_patch_block(uint8_t *block, size_t block_bytes, const uint8_t old_rec[XB_BS_BYTES],
                      const uint8_t new_rec[XB_BS_BYTES])
{
    if (!block || block_bytes < XB_BS_BYTES || !old_rec || !new_rec ||
        memcmp(block, old_rec, XB_BS_BYTES))
        return -1;
    memcpy(block, new_rec, XB_BS_BYTES);
    return 0;
}
