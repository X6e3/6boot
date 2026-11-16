/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host tests for src/xboot_bootstate.c. With no arguments it runs the tests. With arguments it is
 * the C side of the cross test with tools/bootstate.py:
 *   bootstate-test parse FILE                       prints "ok SEQ STATE LEFT SET SIZE SHA256HEX" or "err N"
 *   bootstate-test encode SEQ STATE LEFT SET SIZE SHA256HEX   prints the record as hex
 *   bootstate-test decide FILE TRIAL SECTOR          TRIAL 0-4 as enum xb_bs_trial_check, SECTOR 0/1;
 *                                                   prints "BOOT SHOW REASON WRITE WATCHDOG [OUTHEX]"
 *   bootstate-test locate BPBFILE FIRST BLOCKS BLOCKBYTES CLUSTER SIZE   prints the block or "err N"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/xboot_bootstate.h"

static int failures, checks;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(c)) {                                                                                \
            failures++;                                                                            \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                                \
        }                                                                                          \
    } while (0)

static struct xb_bootstate armed(uint8_t left, uint8_t set)
{
    struct xb_bootstate bs = {.seq = 41, .state = XB_BS_ARMED, .tries_left = left,
                              .tries_set = set, .trial_size = 23068672};
    for (unsigned i = 0; i < XB_BS_HASH_BYTES; i++)
        bs.trial_sha256[i] = (uint8_t)(0xa0 + i);
    return bs;
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* Re-seal a hand-edited record so that only the field rules can reject it. */
static void reseal(uint8_t *r)
{
    put32(r + 508, xb_bs_crc32(r, 508));
}

static void test_crc(void)
{
    CHECK(xb_bs_crc32("123456789", 9) == 0xcbf43926U);
    CHECK(xb_bs_crc32("", 0) == 0);
}

static void test_roundtrip(void)
{
    uint8_t r[XB_BS_BYTES];
    struct xb_bootstate in = armed(2, 3), out;
    CHECK(xb_bs_encode(&in, r) == 0);
    CHECK(memcmp(r, "XBST", 4) == 0);
    CHECK(xb_bs_parse(r, sizeof(r), &out) == XB_BS_OK);
    CHECK(memcmp(&in, &out, sizeof(in)) == 0);

    struct xb_bootstate idle = {.seq = 7};
    CHECK(xb_bs_encode(&idle, r) == 0);
    CHECK(xb_bs_parse(r, sizeof(r), &out) == XB_BS_OK && out.state == XB_BS_IDLE && out.seq == 7);
}

static void test_parse_errors(void)
{
    uint8_t r[XB_BS_BYTES], t[XB_BS_BYTES + 1];
    struct xb_bootstate bs = armed(1, 1);
    xb_bs_encode(&bs, r);

    CHECK(xb_bs_parse(NULL, XB_BS_BYTES, NULL) == XB_BS_E_SIZE);
    CHECK(xb_bs_parse(r, 511, NULL) == XB_BS_E_SIZE);
    memcpy(t, r, sizeof(r));
    t[512] = 0;
    CHECK(xb_bs_parse(t, 513, NULL) == XB_BS_E_SIZE);

    memcpy(t, r, sizeof(r));
    t[7] = 1; /* size 256 */
    reseal(t);
    CHECK(xb_bs_parse(t, XB_BS_BYTES, NULL) == XB_BS_E_SIZE);
    memcpy(t, r, sizeof(r));
    t[0] = 'Y';
    CHECK(xb_bs_parse(t, XB_BS_BYTES, NULL) == XB_BS_E_MAGIC);
    memcpy(t, r, sizeof(r));
    t[4] = 2;
    reseal(t);
    CHECK(xb_bs_parse(t, XB_BS_BYTES, NULL) == XB_BS_E_VERSION);
    memcpy(t, r, sizeof(r));
    t[100] ^= 1;
    CHECK(xb_bs_parse(t, XB_BS_BYTES, NULL) == XB_BS_E_CRC);

    /* Field rules, each with a valid CRC. */
    struct {
        int off;
        uint8_t val;
    } bad[] = {
        {12, 2},   /* state */
        {15, 1},   /* flags */
        {14, 0},   /* armed with no tries set */
        {14, 4},   /* more than three */
        {13, 2},   /* more left than set (set is 1) */
        {52, 1},   /* reserved */
        {507, 1},  /* last reserved byte */
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        memcpy(t, r, sizeof(r));
        t[bad[i].off] = bad[i].val;
        reseal(t);
        CHECK(xb_bs_parse(t, XB_BS_BYTES, NULL) == XB_BS_E_FIELD);
    }
    memcpy(t, r, sizeof(r));
    put32(t + 16, 0); /* armed without a trial size */
    reseal(t);
    CHECK(xb_bs_parse(t, XB_BS_BYTES, NULL) == XB_BS_E_FIELD);

    /* An idle record carries no trial. */
    struct xb_bootstate idle = {0};
    xb_bs_encode(&idle, r);
    for (int off = 13; off < 52; off++) {
        memcpy(t, r, sizeof(r));
        t[off] = 1;
        reseal(t);
        CHECK(xb_bs_parse(t, XB_BS_BYTES, NULL) == XB_BS_E_FIELD);
    }

    /* encode refuses what parse would refuse */
    struct xb_bootstate x = armed(3, 2);
    CHECK(xb_bs_encode(&x, r) == -1);
    x = armed(1, 1);
    x.flags = 1;
    CHECK(xb_bs_encode(&x, r) == -1);
}

/* Every single-bit change of a valid record is refused. */
static void test_bit_flips(void)
{
    uint8_t r[XB_BS_BYTES], t[XB_BS_BYTES];
    struct xb_bootstate bs = armed(2, 3);
    xb_bs_encode(&bs, r);
    int refused = 0;
    for (unsigned byte = 0; byte < XB_BS_BYTES; byte++)
        for (int bit = 0; bit < 8; bit++) {
            memcpy(t, r, sizeof(r));
            t[byte] ^= (uint8_t)(1 << bit);
            refused += xb_bs_parse(t, XB_BS_BYTES, NULL) != XB_BS_OK;
        }
    CHECK(refused == (int)XB_BS_BYTES * 8);
}

static struct xb_bs_decision decide(const uint8_t *rec, int found, enum xb_bs_trial_check trial,
                                    int sector)
{
    struct xb_bs_input in = {.record_found = found, .record = rec, .record_len = XB_BS_BYTES,
                             .sector_ok = sector, .trial = trial};
    struct xb_bs_decision d;
    xb_bs_decide(&in, &d);
    return d;
}

static int is_own(const struct xb_bs_decision *d, enum xb_bs_show show, enum xb_bs_reason reason)
{
    return d->boot == XB_BS_BOOT_OWN && d->show == show && d->reason == reason && !d->write &&
           !d->watchdog;
}

static void test_decide(void)
{
    uint8_t r[XB_BS_BYTES], bad[XB_BS_BYTES];
    struct xb_bs_decision d;

    d = decide(NULL, 0, XB_BS_TRIAL_UNCHECKED, 0);
    CHECK(is_own(&d, XB_BS_SHOW_NONE, XB_BS_R_NO_RECORD));
    xb_bs_decide(NULL, &d);
    CHECK(is_own(&d, XB_BS_SHOW_NONE, XB_BS_R_NO_RECORD));

    struct xb_bootstate idle = {.seq = 3};
    xb_bs_encode(&idle, r);
    d = decide(r, 1, XB_BS_TRIAL_OK, 1);
    CHECK(is_own(&d, XB_BS_SHOW_NONE, XB_BS_R_IDLE));

    memcpy(bad, r, sizeof(r));
    bad[200] ^= 0x10;
    d = decide(bad, 1, XB_BS_TRIAL_OK, 1);
    CHECK(is_own(&d, XB_BS_SHOW_REFUSED, XB_BS_R_BAD_RECORD) && d.parse == XB_BS_E_CRC);

    struct xb_bootstate bs = armed(0, 2);
    xb_bs_encode(&bs, r);
    d = decide(r, 1, XB_BS_TRIAL_OK, 1);
    CHECK(is_own(&d, XB_BS_SHOW_FAILED, XB_BS_R_TRIAL_FAILED));

    bs = armed(2, 2);
    xb_bs_encode(&bs, r);
    d = decide(r, 1, XB_BS_TRIAL_MISSING, 1);
    CHECK(is_own(&d, XB_BS_SHOW_REFUSED, XB_BS_R_TRIAL_MISSING));
    d = decide(r, 1, XB_BS_TRIAL_SIZE, 1);
    CHECK(is_own(&d, XB_BS_SHOW_REFUSED, XB_BS_R_TRIAL_MISMATCH));
    d = decide(r, 1, XB_BS_TRIAL_HASH, 1);
    CHECK(is_own(&d, XB_BS_SHOW_REFUSED, XB_BS_R_TRIAL_MISMATCH));
    d = decide(r, 1, XB_BS_TRIAL_UNCHECKED, 1);
    CHECK(is_own(&d, XB_BS_SHOW_REFUSED, XB_BS_R_UNCHECKED));
    d = decide(r, 1, (enum xb_bs_trial_check)99, 1);
    CHECK(is_own(&d, XB_BS_SHOW_REFUSED, XB_BS_R_UNCHECKED));
    d = decide(r, 1, XB_BS_TRIAL_OK, 0);
    CHECK(is_own(&d, XB_BS_SHOW_REFUSED, XB_BS_R_SECTOR));

    d = decide(r, 1, XB_BS_TRIAL_OK, 1);
    CHECK(d.boot == XB_BS_BOOT_TRIAL && d.show == XB_BS_SHOW_TRIAL && d.reason == XB_BS_R_TRIAL &&
          d.write && d.watchdog);
    struct xb_bootstate after;
    CHECK(xb_bs_parse(d.out, XB_BS_BYTES, &after) == XB_BS_OK);
    CHECK(after.tries_left == 1 && after.seq == bs.seq + 1);
    after.tries_left = bs.tries_left;
    after.seq = bs.seq;
    CHECK(memcmp(&after, &bs, sizeof(bs)) == 0); /* nothing else changed */

    xb_bs_write_failed(&d);
    CHECK(is_own(&d, XB_BS_SHOW_REFUSED, XB_BS_R_WRITE_FAILED));

    CHECK(xb_bs_wants_trial(r, XB_BS_BYTES));
    bs = armed(0, 1);
    xb_bs_encode(&bs, r);
    CHECK(!xb_bs_wants_trial(r, XB_BS_BYTES));
    CHECK(!xb_bs_wants_trial(bad, XB_BS_BYTES));
}

/* Boots that never report back: the trial runs exactly "tries left" times, then never again. */
static void test_trial_runs_out(void)
{
    for (uint8_t set = 1; set <= XB_BS_MAX_TRIES; set++)
        for (uint8_t left = 0; left <= set; left++) {
            uint8_t r[XB_BS_BYTES];
            struct xb_bootstate bs = armed(left, set);
            xb_bs_encode(&bs, r);
            int trials = 0, writes = 0;
            for (int boot = 0; boot < 10; boot++) {
                struct xb_bs_decision d = decide(r, 1, XB_BS_TRIAL_OK, 1);
                if (d.write) {
                    memcpy(r, d.out, sizeof(r));
                    writes++;
                }
                trials += d.boot == XB_BS_BOOT_TRIAL;
                if (boot >= left)
                    CHECK(is_own(&d, XB_BS_SHOW_FAILED, XB_BS_R_TRIAL_FAILED));
            }
            CHECK(trials == left && writes == left);
        }
}

static void test_reason_names(void)
{
    for (int i = 0; i < XB_BS_R_COUNT; i++) {
        const char *n = xb_bs_reason_name((enum xb_bs_reason)i);
        CHECK(n && *n && strcmp(n, "unknown"));
        for (int j = 0; j < i; j++)
            CHECK(strcmp(n, xb_bs_reason_name((enum xb_bs_reason)j)));
    }
    CHECK(!strcmp(xb_bs_reason_name(XB_BS_R_COUNT), "unknown"));
}

static void bpb(uint8_t *b, uint16_t bps, uint8_t spc, uint16_t reserved, uint8_t fats,
                uint16_t root_entries, uint32_t total, uint32_t fat_size, int fat32)
{
    memset(b, 0, 512);
    b[11] = (uint8_t)bps;
    b[12] = (uint8_t)(bps >> 8);
    b[13] = spc;
    b[14] = (uint8_t)reserved;
    b[15] = (uint8_t)(reserved >> 8);
    b[16] = fats;
    b[17] = (uint8_t)root_entries;
    b[18] = (uint8_t)(root_entries >> 8);
    if (fat32) {
        put32(b + 32, total);
        put32(b + 36, fat_size);
    } else {
        b[19] = (uint8_t)total;
        b[20] = (uint8_t)(total >> 8);
        b[22] = (uint8_t)fat_size;
        b[23] = (uint8_t)(fat_size >> 8);
    }
    b[510] = 0x55;
    b[511] = 0xaa;
}

static void test_locate(void)
{
    uint8_t b[512];
    uint64_t blk = 0;

    /* FAT32, 4096-byte sectors, one sector per cluster: data at 32 + 2 * 100 = 232. */
    bpb(b, 4096, 1, 32, 2, 0, 128000, 100, 1);
    CHECK(xb_bs_locate(b, 1000, 128000, 4096, 2, 512, &blk) == XB_BS_LOC_OK && blk == 1000 + 232);
    CHECK(xb_bs_locate(b, 1000, 128000, 4096, 9, 512, &blk) == XB_BS_LOC_OK && blk == 1000 + 239);
    CHECK(xb_bs_locate(b, 1000, 128000, 4096, 0, 512, &blk) == XB_BS_LOC_CLUSTER);
    CHECK(xb_bs_locate(b, 1000, 128000, 4096, 1, 512, &blk) == XB_BS_LOC_CLUSTER);
    /* the last cluster is 2 + (128000 - 232) - 1 */
    CHECK(xb_bs_locate(b, 1000, 128000, 4096, 127769, 512, &blk) == XB_BS_LOC_OK &&
          blk == 1000 + 127999);
    CHECK(xb_bs_locate(b, 1000, 128000, 4096, 127770, 512, &blk) == XB_BS_LOC_CLUSTER);
    CHECK(xb_bs_locate(b, 1000, 200, 4096, 2, 512, &blk) == XB_BS_LOC_BOUNDS);
    CHECK(xb_bs_locate(b, 1000, 128000, 4096, 2, 511, &blk) == XB_BS_LOC_FILE);
    CHECK(xb_bs_locate(b, 1000, 128000, 4096, 2, 4096, &blk) == XB_BS_LOC_FILE);
    CHECK(xb_bs_locate(b, 1000, 128000, 3000, 2, 512, &blk) == XB_BS_LOC_BPB);

    /* 512-byte sectors, 8 per cluster, data area on a 4 KiB boundary: 32 + 2 * 1000 = 2032 = 254 * 8. */
    bpb(b, 512, 8, 32, 2, 0, 1024000, 1000, 1);
    CHECK(xb_bs_locate(b, 0, 128000, 4096, 2, 512, &blk) == XB_BS_LOC_OK && blk == 254);
    CHECK(xb_bs_locate(b, 0, 128000, 4096, 3, 512, &blk) == XB_BS_LOC_OK && blk == 255);
    /* the same with 512-byte device blocks */
    CHECK(xb_bs_locate(b, 0, 1024000, 512, 3, 512, &blk) == XB_BS_LOC_OK && blk == 2040);

    /* data area not on a 4 KiB boundary: refuse rather than write half of two clusters */
    bpb(b, 512, 8, 33, 2, 0, 1024000, 1000, 1);
    CHECK(xb_bs_locate(b, 0, 128000, 4096, 2, 512, &blk) == XB_BS_LOC_ALIGN);
    /* clusters smaller than a device block */
    bpb(b, 512, 1, 32, 2, 0, 1024000, 1000, 1);
    CHECK(xb_bs_locate(b, 0, 128000, 4096, 2, 512, &blk) == XB_BS_LOC_ALIGN);

    /* FAT16 with a root directory of 512 entries (32 sectors): data at 4 + 2 * 32 + 32 = 100. */
    bpb(b, 512, 8, 4, 2, 512, 60000, 32, 0);
    CHECK(xb_bs_locate(b, 0, 60000, 512, 2, 512, &blk) == XB_BS_LOC_OK && blk == 100);

    /* not a boot sector */
    bpb(b, 4096, 1, 32, 2, 0, 128000, 100, 1);
    b[511] = 0;
    CHECK(xb_bs_locate(b, 0, 128000, 4096, 2, 512, &blk) == XB_BS_LOC_BPB);
    bpb(b, 4096, 3, 32, 2, 0, 128000, 100, 1); /* sectors per cluster not a power of two */
    CHECK(xb_bs_locate(b, 0, 128000, 4096, 2, 512, &blk) == XB_BS_LOC_BPB);
    bpb(b, 4096, 1, 32, 0, 0, 128000, 100, 1); /* no FAT */
    CHECK(xb_bs_locate(b, 0, 128000, 4096, 2, 512, &blk) == XB_BS_LOC_BPB);
    bpb(b, 4096, 1, 32, 2, 0, 200, 100, 1); /* FATs larger than the volume */
    CHECK(xb_bs_locate(b, 0, 128000, 4096, 2, 512, &blk) == XB_BS_LOC_BPB);
}

static void test_patch_block(void)
{
    uint8_t blk[4096], old_rec[XB_BS_BYTES], new_rec[XB_BS_BYTES];
    struct xb_bootstate a = armed(2, 2), b = armed(1, 2);
    xb_bs_encode(&a, old_rec);
    xb_bs_encode(&b, new_rec);
    for (unsigned i = 0; i < sizeof(blk); i++)
        blk[i] = (uint8_t)(i * 7);
    memcpy(blk, old_rec, sizeof(old_rec));
    uint8_t keep[4096];
    memcpy(keep, blk, sizeof(blk));

    CHECK(xb_bs_patch_block(blk, sizeof(blk), new_rec, new_rec) == -1); /* not what is there */
    CHECK(memcmp(blk, keep, sizeof(blk)) == 0);
    CHECK(xb_bs_patch_block(blk, sizeof(blk), old_rec, new_rec) == 0);
    CHECK(memcmp(blk, new_rec, XB_BS_BYTES) == 0);
    CHECK(memcmp(blk + XB_BS_BYTES, keep + XB_BS_BYTES, sizeof(blk) - XB_BS_BYTES) == 0);
    CHECK(xb_bs_patch_block(blk, 511, new_rec, old_rec) == -1);
}

/* ---- cross-test CLI ---- */

static int read_file(const char *path, uint8_t *buf, size_t cap, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    *len = fread(buf, 1, cap, f);
    fclose(f);
    return 0;
}

static void hex(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        printf("%02x", p[i]);
}

static int unhex(const char *s, uint8_t *out, size_t n)
{
    if (strlen(s) != 2 * n)
        return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1)
            return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

static int cli(int argc, char **argv)
{
    uint8_t buf[XB_BS_BYTES + 64];
    size_t len = 0;
    if (!strcmp(argv[1], "parse") && argc == 3) {
        struct xb_bootstate bs;
        if (read_file(argv[2], buf, sizeof(buf), &len))
            return 2;
        int rc = xb_bs_parse(buf, len, &bs);
        if (rc) {
            printf("err %d\n", rc);
            return 0;
        }
        printf("ok %u %u %u %u %u ", bs.seq, bs.state, bs.tries_left, bs.tries_set, bs.trial_size);
        hex(bs.trial_sha256, XB_BS_HASH_BYTES);
        printf("\n");
        return 0;
    }
    if (!strcmp(argv[1], "encode") && argc == 8) {
        struct xb_bootstate bs = {.seq = (uint32_t)strtoul(argv[2], NULL, 0),
                                  .state = (uint8_t)atoi(argv[3]),
                                  .tries_left = (uint8_t)atoi(argv[4]),
                                  .tries_set = (uint8_t)atoi(argv[5]),
                                  .trial_size = (uint32_t)strtoul(argv[6], NULL, 0)};
        uint8_t r[XB_BS_BYTES];
        if (unhex(argv[7], bs.trial_sha256, XB_BS_HASH_BYTES) || xb_bs_encode(&bs, r)) {
            printf("err\n");
            return 0;
        }
        hex(r, sizeof(r));
        printf("\n");
        return 0;
    }
    if (!strcmp(argv[1], "decide") && argc == 5) {
        int found = !read_file(argv[2], buf, sizeof(buf), &len);
        struct xb_bs_input in = {.record_found = found, .record = buf, .record_len = len,
                                 .sector_ok = atoi(argv[4]),
                                 .trial = (enum xb_bs_trial_check)atoi(argv[3])};
        struct xb_bs_decision d;
        xb_bs_decide(&in, &d);
        printf("%d %d %s %d %d", d.boot, d.show, xb_bs_reason_name(d.reason), d.write, d.watchdog);
        if (d.write) {
            printf(" ");
            hex(d.out, XB_BS_BYTES);
        }
        printf("\n");
        return 0;
    }
    if (!strcmp(argv[1], "locate") && argc == 8) {
        uint8_t b[512];
        uint64_t blk = 0;
        if (read_file(argv[2], b, sizeof(b), &len) || len != sizeof(b))
            return 2;
        int rc = xb_bs_locate(b, strtoull(argv[3], NULL, 0), strtoull(argv[4], NULL, 0),
                              (uint32_t)strtoul(argv[5], NULL, 0),
                              (uint32_t)strtoul(argv[6], NULL, 0),
                              (uint32_t)strtoul(argv[7], NULL, 0), &blk);
        if (rc)
            printf("err %d\n", rc);
        else
            printf("%llu\n", (unsigned long long)blk);
        return 0;
    }
    fprintf(stderr, "usage: see the comment at the top of %s\n", __FILE__);
    return 2;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        return cli(argc, argv);
    test_crc();
    test_roundtrip();
    test_parse_errors();
    test_bit_flips();
    test_decide();
    test_trial_runs_out();
    test_reason_names();
    test_locate();
    test_patch_block();
    printf("bootstate: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
