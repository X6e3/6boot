/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include "xboot_sha256.h"
#include "xboot_trial.h"

struct rec_sink {
    uint8_t *buf;
    size_t len, cap;
};

static int to_buf(void *ctx, const void *data, size_t len)
{
    struct rec_sink *s = ctx;
    if (len > s->cap - s->len)
        return 1;
    memcpy(s->buf + s->len, data, len);
    s->len += len;
    return 0;
}

static int to_sha(void *ctx, const void *data, size_t len)
{
    xb_sha256_update(ctx, data, len);
    return 0;
}

static void fail(struct xb_trial_report *r, enum xb_trial_step at, int err)
{
    if (r->failed_at == XB_T_NONE) {
        r->failed_at = at;
        r->err = err;
    }
}

void xb_trial_check(struct xb_blk *d, const char *esp_uuid, uint8_t *block, struct xb_trial_report *r)
{
    struct xb_bs_input in = {0};
    struct xb_fat fat;
    struct xb_bootstate bs;
    uint8_t uuid[16];
    int e;

    memset(r, 0, sizeof(*r));
    if ((e = xb_uuid_parse(esp_uuid, uuid))) {
        fail(r, XB_T_UUID, e);
        goto decide;
    }
    if ((e = xb_gpt_find(d, uuid, &r->part_first, &r->part_blocks))) {
        fail(r, XB_T_GPT, e);
        goto decide;
    }
    if ((e = xb_fat_mount(&fat, d, r->part_first, r->part_blocks))) {
        fail(r, XB_T_MOUNT, e);
        goto decide;
    }
    if ((e = xb_fat_lookup(&fat, XB_BS_PATH, &r->rec_cluster, &r->rec_size))) {
        if (e != XB_ESP_E_NOENT)
            fail(r, XB_T_RECORD, e);
        goto decide; /* no record: a normal boot */
    }
    in.record_found = 1;
    in.record = r->record;
    struct rec_sink rs = {r->record, 0, sizeof(r->record)};
    if (r->rec_size > sizeof(r->record) || (e = xb_fat_read(&fat, r->rec_cluster, r->rec_size, to_buf, &rs))) {
        fail(r, XB_T_READ, r->rec_size > sizeof(r->record) ? XB_ESP_E_FAT : e);
        in.record_len = 0; /* parse refuses it */
        goto decide;
    }
    in.record_len = rs.len;
    if (!xb_bs_wants_trial(r->record, in.record_len) || xb_bs_parse(r->record, in.record_len, &bs))
        goto decide;

    /* an armed record: the trial must be there, be as long as recorded and hash the same */
    if ((e = xb_fat_lookup(&fat, XB_BS_TRIAL_PATH, &r->trial_cluster, &r->trial_size))) {
        in.trial = XB_BS_TRIAL_MISSING;
        if (e != XB_ESP_E_NOENT)
            fail(r, XB_T_TRIAL, e);
        goto decide;
    }
    if (r->trial_size != bs.trial_size) {
        in.trial = XB_BS_TRIAL_SIZE;
        goto decide;
    }
    struct xb_sha256 sha;
    xb_sha256_init(&sha);
    if ((e = xb_fat_read(&fat, r->trial_cluster, r->trial_size, to_sha, &sha))) {
        fail(r, XB_T_HASH, e);
        in.trial = XB_BS_TRIAL_MISSING;
        goto decide;
    }
    xb_sha256_final(&sha, r->trial_sha256);
    in.trial = memcmp(r->trial_sha256, bs.trial_sha256, 32) ? XB_BS_TRIAL_HASH : XB_BS_TRIAL_OK;
    if (in.trial != XB_BS_TRIAL_OK)
        goto decide;

    /* the one block 6boot would write: located from the FAT, and holding what the FAT gave us */
    if ((e = xb_bs_locate(fat.bpb, r->part_first, r->part_blocks, d->bytes, r->rec_cluster,
                          r->rec_size, &r->rec_block))) {
        fail(r, XB_T_LOCATE, e);
        goto decide;
    }
    if ((e = xb_blk_read(d, r->rec_block, block))) {
        fail(r, XB_T_BLOCK, e);
        goto decide;
    }
    in.sector_ok = !memcmp(block, r->record, XB_BS_BYTES);

decide:
    xb_bs_decide(&in, &r->d);
}

const char *xb_trial_step_name(enum xb_trial_step s)
{
    static const char *const n[] = {"none", "uuid", "gpt", "mount", "record", "read", "trial",
                                    "hash", "locate", "block"};
    return (unsigned)s < sizeof(n) / sizeof(n[0]) ? n[s] : "unknown";
}
