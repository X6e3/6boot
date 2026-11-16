/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Trial boots in stage 2. Brings up the NVMe the way the first stage does to read M1N1/BOOT.BIN and
 * runs the check of xboot_trial.c. It leaves what it decided in /chosen.
 *
 * Log only (SIXBOOT_LEVEL2): nothing is written and the image 6boot came in boots anyway.
 *
 * With SIXBOOT_LEVEL2_TRIAL the decision is carried out. For an armed record with a try left and a
 * trial whose SHA-256 matches, the trial is kept in memory as it is hashed; then the one try is
 * spent on disk (xb_trial_commit: one block, flushed and read back), and only then is the trial
 * loaded over 6boot's own payload. A try that could not be written never boots the trial. Every
 * other outcome, including a trial whose try is already spent, boots 6boot's own image and writes
 * nothing. main.c shuts the NVMe down before the handoff as on every boot.
 */
#include "../build/build_cfg.h"
#include "xboot_level2.h"
#include "kboot.h"
#include "malloc.h"
#include "nvme.h"
#include "string.h"
#include "utils.h"
#include "xboot_trial.h"

#define ESP_VAR "chosen.gravity,efi-system-partition="

static int nvme_block(void *ctx, uint64_t block, void *buf)
{
    (void)ctx;
    return nvme_read(1, block, buf) ? 0 : -1;
}

static const char *esp_uuid(char **vars, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (vars[i] && !strncmp(vars[i], ESP_VAR, strlen(ESP_VAR)))
            return vars[i] + strlen(ESP_VAR);
    return NULL;
}

#ifdef SIXBOOT_LEVEL2_TRIAL
static int nvme_wblock(void *ctx, uint64_t block, const void *buf)
{
    (void)ctx;
    return nvme_write(1, block, buf) ? 0 : -1;
}

static int nvme_sync(void *ctx)
{
    (void)ctx;
    return nvme_flush(1) ? 0 : -1;
}

/* The trial stays where it was read: its device tree is used in place. Four zero bytes after it end
 * payload.c's loop ("No more payloads"). */
static void *trial_alloc(size_t n)
{
    u8 *p = malloc(n + SZ_4K);
    if (p)
        memset(p + n, 0, SZ_4K);
    return p;
}
#endif

void xboot_level2_check(char **vars, size_t n, void *(*load)(void *start, size_t size))
{
    static struct xb_trial_report r;
    char report[160];
    const char *uuid = esp_uuid(vars, n);
    const char *path = "known";
    u64 t0 = get_ticks();
    uint8_t *cache = NULL, *block = NULL, *verify = NULL;
    int boot_trial = 0;

    memset(&r, 0, sizeof(r));
    if (!uuid) {
        r.failed_at = XB_T_UUID;
        xb_bs_decide(&(struct xb_bs_input){0}, &r.d);
    } else if (!nvme_init()) {
        r.failed_at = XB_T_GPT;
        r.err = XB_ESP_E_IO;
        xb_bs_decide(&(struct xb_bs_input){0}, &r.d);
    } else {
        cache = memalign(SZ_4K, SZ_4K);
        block = memalign(SZ_4K, SZ_4K);
        verify = memalign(SZ_4K, SZ_4K);
        if (cache && block && verify) {
            struct xb_blk d = {nvme_block, NULL, SZ_4K, cache, UINT64_MAX};
#ifdef SIXBOOT_LEVEL2_TRIAL
            xb_trial_check_keep(&d, uuid, block, trial_alloc, &r);
            if (r.d.boot == XB_BS_BOOT_TRIAL)
                boot_trial = !xb_trial_commit(&d, &r, block, verify, nvme_wblock, nvme_sync);
#else
            xb_trial_check(&d, uuid, block, &r);
#endif
        } else {
            r.failed_at = XB_T_GPT;
            r.err = XB_ESP_E_ARG;
            xb_bs_decide(&(struct xb_bs_input){0}, &r.d);
        }
        free(cache);
        free(block);
        free(verify);
    }
    u64 us = ticks_to_usecs(get_ticks() - t0);

#ifdef SIXBOOT_LEVEL2_TRIAL
    const char *mode = "trial boots";
    if (boot_trial && load) {
        void *p = r.trial;
        while (p)
            p = load(p, 0); /* later boot arguments replace 6boot's own in kboot_set_chosen */
        path = "trial";
    }
#else
    const char *mode = "log only";
    (void)load;
    (void)boot_trial;
#endif
    const char *would = r.d.boot == XB_BS_BOOT_TRIAL ? "trial" : "known";
    snprintf(report, sizeof(report), "would=%s write=%d wrote=%d wstep=%s failed_at=%s err=%d block=%lu us=%lu",
             would, r.d.write, r.wrote, xb_commit_step_name(r.wstep), xb_trial_step_name(r.failed_at), r.err,
             (unsigned long)r.rec_block, (unsigned long)us);
    printf("6boot: level 2, %s: %s, boots %s (%s)\n", mode, xb_bs_reason_name(r.d.reason), path, report);
    kboot_set_chosen("xboot,path", path);
    kboot_set_chosen("xboot,trial", xb_bs_reason_name(r.d.reason));
    kboot_set_chosen("xboot,trial-report", report);
}
