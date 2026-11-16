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

enum xb_commit_step { XB_C_NONE = 0, XB_C_PATCH, XB_C_WRITE, XB_C_FLUSH, XB_C_READ, XB_C_VERIFY };

struct xb_trial_report {
    enum xb_trial_step failed_at; /* the first step that did not work, or XB_T_NONE */
    int err;                      /* its error */
    uint64_t part_first, part_blocks;
    uint32_t rec_cluster, rec_size, trial_cluster, trial_size;
    uint64_t rec_block;
    uint8_t record[XB_BS_BYTES], trial_sha256[32];
    struct xb_bs_decision d;
    uint8_t *trial;               /* xb_trial_check_keep: the trial's bytes, the same that were hashed */
    int wrote;                    /* xb_trial_commit: 0 not tried, 1 written and read back, -1 failed */
    enum xb_commit_step wstep;    /* where it failed */
};

/* block: the caller's buffer of one device block (d->bytes), for the raw read of the record. */
void xb_trial_check(struct xb_blk *d, const char *esp_uuid, uint8_t *block, struct xb_trial_report *r);

/*
 * The same check, and for an armed record also keeps the trial: alloc(size) gives memory for it, and
 * the bytes hashed are the bytes kept, so what is booted is what was checked. No memory: the trial is
 * left unchecked and 6boot boots its own image. r->trial is set only when the hash matched.
 */
void xb_trial_check_keep(struct xb_blk *d, const char *esp_uuid, uint8_t *block, void *(*alloc)(size_t),
                         struct xb_trial_report *r);

/*
 * The only write 6boot makes, for a decision to boot the trial: the new record over the first
 * XB_BS_BYTES of the raw block xb_trial_check read (which must still hold the old record), the block
 * written, flushed and read back raw into verify. Anything short of the same block back turns the
 * decision into write-failed. Returns 0 when the trial may boot; the trial never boots on an unwritten
 * try. write and flush are the device's; they return 0 on success.
 */
int xb_trial_commit(struct xb_blk *d, struct xb_trial_report *r, uint8_t *block, uint8_t *verify,
                    int (*write)(void *ctx, uint64_t block, const void *buf), int (*flush)(void *ctx));
const char *xb_trial_step_name(enum xb_trial_step s);
const char *xb_commit_step_name(enum xb_commit_step s);
#endif
