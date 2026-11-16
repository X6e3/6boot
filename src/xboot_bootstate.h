/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef XBOOT_BOOTSTATE_H
#define XBOOT_BOOTSTATE_H
#include <stddef.h>
#include <stdint.h>

/*
 * Trial boots (installer level 2). Linux arms a trial in one 512-byte file on the ESP; 6boot boots
 * the trial's kernel, device tree, initramfs and boot arguments a limited number of times, and the
 * image it was loaded with (M1N1/BOOT.BIN) otherwise. 6boot itself is never the trial.
 *
 * The file, little-endian:
 *     0  magic "XBST"           4  version (u16) = 1      6  size (u16) = 512
 *     8  seq (u32): every write adds one
 *    12  state (u8): 0 idle, 1 armed
 *    13  tries left (u8)       14  tries set (u8), 1 to 3      15  flags (u8) = 0
 *    16  trial size (u32): bytes of M1N1/BOOT-TRIAL.BIN
 *    20  trial SHA-256 (32 bytes)
 *    52  zero up to 507
 *   508  CRC-32 (IEEE, as zlib.crc32) of bytes 0 to 507
 * An idle record has every trial field zero. Linux arms, disarms and promotes; 6boot's only write
 * lowers "tries left" by one, just before it boots the trial. Anything it cannot check, it does
 * not write, and it boots its own image.
 */
#define XB_BS_PATH       "j713/bootstate.bin"
#define XB_BS_TRIAL_PATH "M1N1/BOOT-TRIAL.BIN"
#define XB_BS_BYTES      512U
#define XB_BS_MAGIC      0x54534258U /* "XBST" */
#define XB_BS_VERSION    1U
#define XB_BS_MAX_TRIES  3U
#define XB_BS_HASH_BYTES 32U

enum xb_bs_state { XB_BS_IDLE = 0, XB_BS_ARMED = 1 };

struct xb_bootstate {
    uint32_t seq;
    uint8_t state, tries_left, tries_set, flags;
    uint32_t trial_size;
    uint8_t trial_sha256[XB_BS_HASH_BYTES];
};

enum xb_bs_parse {
    XB_BS_OK = 0,
    XB_BS_E_SIZE,    /* not 512 bytes, or the size field is not 512 */
    XB_BS_E_MAGIC,
    XB_BS_E_VERSION,
    XB_BS_E_CRC,
    XB_BS_E_FIELD, /* a field out of range, an idle record with a trial, or a reserved byte set */
};

uint32_t xb_bs_crc32(const void *buf, size_t len);
int xb_bs_parse(const void *buf, size_t len, struct xb_bootstate *out);
int xb_bs_encode(const struct xb_bootstate *bs, uint8_t out[XB_BS_BYTES]);

/* What 6boot checked before it decides; the caller fills it. */
enum xb_bs_trial_check {
    XB_BS_TRIAL_UNCHECKED = 0,
    XB_BS_TRIAL_MISSING,
    XB_BS_TRIAL_SIZE,
    XB_BS_TRIAL_HASH,
    XB_BS_TRIAL_OK,
};

struct xb_bs_input {
    int record_found;           /* XB_BS_PATH exists */
    const void *record;         /* its bytes, read through the FAT */
    size_t record_len;
    int sector_ok;              /* the block xb_bs_locate() gives, read raw, starts with record */
    enum xb_bs_trial_check trial;
};

enum xb_bs_boot { XB_BS_BOOT_OWN = 0, XB_BS_BOOT_TRIAL };
enum xb_bs_show { XB_BS_SHOW_NONE = 0, XB_BS_SHOW_TRIAL, XB_BS_SHOW_FAILED, XB_BS_SHOW_REFUSED };
enum xb_bs_reason {
    XB_BS_R_NO_RECORD = 0,
    XB_BS_R_BAD_RECORD,
    XB_BS_R_IDLE,
    XB_BS_R_TRIAL,
    XB_BS_R_TRIAL_FAILED, /* armed with no tries left: the last trial never reported back */
    XB_BS_R_TRIAL_MISSING,
    XB_BS_R_TRIAL_MISMATCH,
    XB_BS_R_SECTOR,
    XB_BS_R_WRITE_FAILED,
    XB_BS_R_UNCHECKED,
    XB_BS_R_COUNT,
};

struct xb_bs_decision {
    enum xb_bs_boot boot;
    enum xb_bs_show show;
    enum xb_bs_reason reason;
    int parse;    /* xb_bs_parse() of the record, when one was found */
    int write;    /* write out[] over the record, and read it back, before the trial */
    int watchdog; /* arm the watchdog for this boot: trials only */
    uint8_t out[XB_BS_BYTES];
};

/* True when the record asks for a trial: only then is the trial file worth hashing. */
int xb_bs_wants_trial(const void *record, size_t len);
void xb_bs_decide(const struct xb_bs_input *in, struct xb_bs_decision *d);
/* The write or its read-back failed: boot 6boot's own image instead. */
void xb_bs_write_failed(struct xb_bs_decision *d);
const char *xb_bs_reason_name(enum xb_bs_reason r);

/*
 * Where the record lives on the disk. boot_sector is the first 512 bytes of the FAT partition;
 * the partition starts at device block part_first and is part_blocks long, in blocks of
 * block_bytes (4096 on the J713's NVMe). The file must start a cluster that is at least one
 * device block long and starts on a device block, so the block holds the record and otherwise
 * only the rest of that cluster. Returns 0 and the device block, or a negative xb_bs_loc.
 */
enum xb_bs_loc {
    XB_BS_LOC_OK = 0,
    XB_BS_LOC_BPB = -1,     /* not a FAT boot sector this code accepts */
    XB_BS_LOC_CLUSTER = -2, /* the cluster number is outside the data area */
    XB_BS_LOC_ALIGN = -3,   /* the cluster is smaller than, or not aligned to, a device block */
    XB_BS_LOC_BOUNDS = -4,  /* the block is outside the partition */
    XB_BS_LOC_FILE = -5,    /* the file is not exactly one record long */
};
int xb_bs_locate(const uint8_t boot_sector[512], uint64_t part_first, uint64_t part_blocks,
                 uint32_t block_bytes, uint32_t first_cluster, uint32_t file_size,
                 uint64_t *block);

/* Put the new record into a raw block that must start with the old one. -1 if it does not. */
int xb_bs_patch_block(uint8_t *block, size_t block_bytes, const uint8_t old_rec[XB_BS_BYTES],
                      const uint8_t new_rec[XB_BS_BYTES]);
#endif
