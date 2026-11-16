/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef XBOOT_TRIAL_H
#define XBOOT_TRIAL_H
#include "xboot_bootstate.h"
#include "xboot_esp.h"

/*
 * Everything stage 2 checks before it decides about a trial, read only: the ESP by its UUID, the
 * record, the trial's size and SHA-256, and the record's raw block. The decision comes back with a
 * report of each step; nothing here writes. The caller logs it, and in a log-only build stops there.
 */
enum xb_trial_step {
    XB_T_NONE = 0, XB_T_UUID, XB_T_GPT, XB_T_MOUNT, XB_T_RECORD, XB_T_READ, XB_T_TRIAL, XB_T_HASH,
    XB_T_LOCATE, XB_T_BLOCK,
};

struct xb_trial_report {
    enum xb_trial_step failed_at; /* the first step that did not work, or XB_T_NONE */
    int err;                      /* its error */
    uint64_t part_first, part_blocks;
    uint32_t rec_cluster, rec_size, trial_cluster, trial_size;
    uint64_t rec_block;
    uint8_t record[XB_BS_BYTES], trial_sha256[32];
    struct xb_bs_decision d;
};

/* block: the caller's buffer of one device block (d->bytes), for the raw read of the record. */
void xb_trial_check(struct xb_blk *d, const char *esp_uuid, uint8_t *block, struct xb_trial_report *r);
const char *xb_trial_step_name(enum xb_trial_step s);
#endif
