/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Trial boots in stage 2, log only. Brings up the NVMe the way the first stage does to read
 * M1N1/BOOT.BIN, runs the read-only check of xboot_trial.c, prints what it would do and leaves it in
 * /chosen, and boots the image it came in anyway. Nothing is written to the disk; main.c shuts the
 * NVMe down before the handoff as on every boot.
 */
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

void xboot_level2_check(char **vars, size_t n)
{
    static struct xb_trial_report r;
    char report[160];
    const char *uuid = esp_uuid(vars, n);
    u64 t0 = get_ticks();
    uint8_t *cache = NULL, *block = NULL;

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
        if (cache && block) {
            struct xb_blk d = {nvme_block, NULL, SZ_4K, cache, UINT64_MAX};
            xb_trial_check(&d, uuid, block, &r);
        } else {
            r.failed_at = XB_T_GPT;
            r.err = XB_ESP_E_ARG;
            xb_bs_decide(&(struct xb_bs_input){0}, &r.d);
        }
        free(cache);
        free(block);
    }
    u64 us = ticks_to_usecs(get_ticks() - t0);

    const char *would = r.d.boot == XB_BS_BOOT_TRIAL ? "trial" : "known";
    snprintf(report, sizeof(report), "would=%s write=%d failed_at=%s err=%d block=%lu us=%lu", would,
             r.d.write, xb_trial_step_name(r.failed_at), r.err, (unsigned long)r.rec_block,
             (unsigned long)us);
    printf("6boot: level 2, log only: %s (%s)\n", xb_bs_reason_name(r.d.reason), report);
    kboot_set_chosen("xboot,path", "known"); /* log only: always the image 6boot came in */
    kboot_set_chosen("xboot,trial", xb_bs_reason_name(r.d.reason));
    kboot_set_chosen("xboot,trial-report", report);
}
