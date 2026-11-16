/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef XBOOT_ESP_H
#define XBOOT_ESP_H
#include <stddef.h>
#include <stdint.h>

/*
 * Read-only access to the EFI system partition from stage 2: the partition by its GPT unique UUID,
 * then a FAT32 file by its path. Stage 2 is built without m1n1's chainloading and so has no reader
 * of its own; this one reads through a block callback (nvme_read on the J713, a file on the host)
 * and never writes. Everything it follows on disk is bounded: a damaged table or a cluster chain
 * that loops ends in an error, not in a hang.
 */
struct xb_blk {
    int (*read)(void *ctx, uint64_t block, void *buf); /* one block; 0 when it was read */
    void *ctx;
    uint32_t bytes;   /* block size: 4096 on the J713's NVMe */
    uint8_t *cache;   /* one block of the caller's memory */
    uint64_t cached;  /* the block in the cache; set to UINT64_MAX before first use */
};

enum xb_esp_err {
    XB_ESP_OK = 0,
    XB_ESP_E_IO = -1,     /* a block read failed */
    XB_ESP_E_GPT = -2,    /* no valid GPT header */
    XB_ESP_E_NOPART = -3, /* no partition with that UUID */
    XB_ESP_E_FAT = -4,    /* not a FAT32 file system this code accepts */
    XB_ESP_E_NOENT = -5,  /* no such file or directory */
    XB_ESP_E_CHAIN = -6,  /* a cluster chain leaves the volume, loops or ends early */
    XB_ESP_E_ARG = -7,
    XB_ESP_E_SINK = -8,   /* the caller's sink asked to stop */
};

/* "f2a5b0ec-…" to the 16 bytes GPT stores (first three fields little-endian). */
int xb_uuid_parse(const char *text, uint8_t out[16]);
int xb_gpt_find(struct xb_blk *d, const uint8_t uuid[16], uint64_t *first, uint64_t *count);

struct xb_fat {
    struct xb_blk *d;
    uint64_t part_first, part_blocks; /* in device blocks */
    uint8_t bpb[512];
    uint32_t bps, spc, fat_start, fat_size, root_cluster, clusters;
    uint64_t data_start; /* in FAT sectors */
};

int xb_fat_mount(struct xb_fat *f, struct xb_blk *d, uint64_t part_first, uint64_t part_blocks);
/* path like "j713/bootstate.bin": ASCII, '/' separated, case-insensitive, long or 8.3 names */
int xb_fat_lookup(struct xb_fat *f, const char *path, uint32_t *first_cluster, uint32_t *size);
typedef int (*xb_sink)(void *ctx, const void *data, size_t len); /* nonzero stops the read */
int xb_fat_read(struct xb_fat *f, uint32_t first_cluster, uint32_t size, xb_sink sink, void *ctx);
/* One raw device block of the partition, relative to the device (not cached). */
int xb_blk_read(struct xb_blk *d, uint64_t block, void *buf);
#endif
