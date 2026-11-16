/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host tests for src/xboot_sha256.c, src/xboot_esp.c and src/xboot_trial.c. With no arguments: the
 * SHA-256 test vectors and the UUID parser. With arguments it reads a disk image as a block device,
 * for esp_fixture_test.py:
 *   esp-test find   IMG BLOCKBYTES UUID          prints "FIRST COUNT" or "err N"
 *   esp-test lookup IMG BLOCKBYTES UUID PATH     prints "CLUSTER SIZE SHA256HEX" or "err N"
 *   esp-test check  IMG BLOCKBYTES UUID          prints the decision and the report, one key=value each
 */
#define _POSIX_C_SOURCE 200809L /* fseeko and off_t under -std=c11 on glibc */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/xboot_esp.h"
#include "../../src/xboot_sha256.h"
#include "../../src/xboot_trial.h"

static int failures, checks;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(c)) {                                                                                \
            failures++;                                                                            \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                                \
        }                                                                                          \
    } while (0)

static void hex(const uint8_t *p, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++)
        snprintf(out + 2 * i, 3, "%02x", p[i]);
}

static int sha_is(const void *data, size_t len, const char *want)
{
    struct xb_sha256 s;
    uint8_t d[32];
    char h[65];
    xb_sha256_init(&s);
    xb_sha256_update(&s, data, len);
    xb_sha256_final(&s, d);
    hex(d, 32, h);
    return !strcmp(h, want);
}

static void test_sha256(void)
{
    CHECK(sha_is("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK(sha_is("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(sha_is(m, strlen(m), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    /* a million 'a', fed in uneven pieces */
    static char a[1000000];
    memset(a, 'a', sizeof(a));
    struct xb_sha256 s;
    uint8_t d[32];
    char h[65];
    xb_sha256_init(&s);
    for (size_t o = 0, n = 1; o < sizeof(a); o += n, n = n * 3 % 997 + 1)
        xb_sha256_update(&s, a + o, o + n > sizeof(a) ? sizeof(a) - o : n);
    xb_sha256_final(&s, d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    /* every length around the block edges */
    for (size_t n = 50; n < 140; n++) {
        struct xb_sha256 x, y;
        uint8_t dx[32], dy[32];
        xb_sha256_init(&x);
        xb_sha256_update(&x, a, n);
        xb_sha256_final(&x, dx);
        xb_sha256_init(&y);
        for (size_t i = 0; i < n; i++)
            xb_sha256_update(&y, a, 1);
        xb_sha256_final(&y, dy);
        CHECK(!memcmp(dx, dy, 32));
    }
}

static void test_uuid(void)
{
    uint8_t u[16];
    static const uint8_t want[16] = {0xec, 0xb0, 0xa5, 0xf2, 0x34, 0x12, 0x78, 0x56,
                                     0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56, 0x78};
    CHECK(xb_uuid_parse("F2A5B0EC-1234-5678-9ABC-DEF012345678", u) == 0 && !memcmp(u, want, 16));
    CHECK(xb_uuid_parse("f2a5b0ec-1234-5678-9abc-def012345678", u) == 0 && !memcmp(u, want, 16));
    CHECK(xb_uuid_parse("F2A5B0EC-1234-5678-9ABC-DEF01234567", u) != 0);
    CHECK(xb_uuid_parse("F2A5B0EC-1234-5678-9ABC-DEF0123456789", u) != 0);
    CHECK(xb_uuid_parse("F2A5B0EC_1234-5678-9ABC-DEF012345678", u) != 0);
    CHECK(xb_uuid_parse("G2A5B0EC-1234-5678-9ABC-DEF012345678", u) != 0);
    CHECK(xb_uuid_parse(NULL, u) != 0);
}

/* ---- a FAT32 volume made up on the fly: directories that end exactly at a cluster's end ---- */

#define V_CLUSTERS 70000U
#define V_FATSZ ((V_CLUSTERS + 2) * 4 / 4096 + 1)
#define V_DATA (1 + V_FATSZ) /* reserved 1, one FAT */

static int virt_read(void *ctx, uint64_t block, void *buf)
{
    const uint32_t *chain_next = ctx; /* FAT entries for clusters 2..5 */
    uint8_t *b = buf;
    memset(b, 0, 4096);
    if (block == 0) {
        b[11] = 0x00, b[12] = 0x10;           /* 4096 bytes per sector */
        b[13] = 1;                            /* one sector per cluster */
        b[14] = 1;                            /* one reserved sector */
        b[16] = 1;                            /* one FAT */
        uint32_t total = V_DATA + V_CLUSTERS, fsz = V_FATSZ;
        memcpy(b + 32, &total, 4);
        memcpy(b + 36, &fsz, 4);
        b[44] = 2;                            /* root directory at cluster 2 */
        b[510] = 0x55, b[511] = 0xaa;
    } else if (block == 1) {                  /* the FAT: clusters 2..5 */
        memcpy(b + 8, chain_next, 16);
    } else if (block >= V_DATA && block < V_DATA + 4) {
        uint32_t cl = (uint32_t)(block - V_DATA); /* cluster 2 + cl: 128 short names, no end marker */
        for (int k = 0; k < 128; k++) {
            char n[12];
            snprintf(n, sizeof(n), "F%u%03d   BIN", cl, k); /* 8.3: "F0127   BIN" */
            memcpy(b + 32 * k, n, 11);
            b[32 * k + 26] = (uint8_t)(10 + k); /* a first cluster, unused here */
            b[32 * k + 28] = 1;                 /* one byte long */
        }
    }
    return 0;
}

static void test_full_directories(void)
{
    static uint8_t cache[4096];
    struct xb_fat f;
    uint32_t c, size;
    /* the root directory: one full cluster, then two full clusters, both ending with the FAT's end mark */
    uint32_t one[4] = {0x0fffffff, 0, 0, 0}, two[4] = {3, 0x0fffffff, 0, 0}, loop[4] = {3, 2, 0, 0};
    struct xb_blk d = {virt_read, one, 4096, cache, UINT64_MAX};

    CHECK(xb_fat_mount(&f, &d, 0, V_DATA + V_CLUSTERS) == XB_ESP_OK);
    CHECK(xb_fat_lookup(&f, "F0127.BIN", &c, &size) == XB_ESP_OK && size == 1);
    CHECK(xb_fat_lookup(&f, "NOTHERE.BIN", &c, &size) == XB_ESP_E_NOENT);
    d.ctx = two, d.cached = UINT64_MAX;
    CHECK(xb_fat_mount(&f, &d, 0, V_DATA + V_CLUSTERS) == XB_ESP_OK);
    CHECK(xb_fat_lookup(&f, "f1127.bin", &c, &size) == XB_ESP_OK);
    CHECK(xb_fat_lookup(&f, "NOTHERE.BIN", &c, &size) == XB_ESP_E_NOENT);
    d.ctx = loop, d.cached = UINT64_MAX;      /* a directory chain that comes back on itself */
    CHECK(xb_fat_mount(&f, &d, 0, V_DATA + V_CLUSTERS) == XB_ESP_OK);
    CHECK(xb_fat_lookup(&f, "NOTHERE.BIN", &c, &size) == XB_ESP_E_CHAIN);
}

/* ---- a disk image as a block device ---- */

struct file_dev {
    FILE *f;
    uint32_t bytes;
    long reads;
};

static int file_read(void *ctx, uint64_t block, void *buf)
{
    struct file_dev *fd = ctx;
    fd->reads++;
    if (fseeko(fd->f, (off_t)(block * fd->bytes), SEEK_SET))
        return -1;
    return fread(buf, 1, fd->bytes, fd->f) == fd->bytes ? 0 : -1;
}

struct sha_ctx {
    struct xb_sha256 s;
};

static int sha_sink(void *ctx, const void *data, size_t len)
{
    xb_sha256_update(&((struct sha_ctx *)ctx)->s, data, len);
    return 0;
}

static int cli(int argc, char **argv)
{
    if (argc < 5)
        return 2;
    struct file_dev fd = {fopen(argv[2], "rb"), (uint32_t)strtoul(argv[3], NULL, 0), 0};
    if (!fd.f || !fd.bytes)
        return 2;
    uint8_t *cache = malloc(fd.bytes), *block = malloc(fd.bytes);
    struct xb_blk d = {file_read, &fd, fd.bytes, cache, UINT64_MAX};
    uint8_t uuid[16];
    uint64_t first = 0, count = 0;
    char h[65];

    if (!strcmp(argv[1], "find")) {
        int e = xb_uuid_parse(argv[4], uuid);
        if (!e)
            e = xb_gpt_find(&d, uuid, &first, &count);
        if (e)
            printf("err %d\n", e);
        else
            printf("%llu %llu\n", (unsigned long long)first, (unsigned long long)count);
        return 0;
    }
    if (!strcmp(argv[1], "lookup") && argc == 6) {
        struct xb_fat fat;
        uint32_t c = 0, size = 0;
        int e = xb_uuid_parse(argv[4], uuid);
        if (!e)
            e = xb_gpt_find(&d, uuid, &first, &count);
        if (!e)
            e = xb_fat_mount(&fat, &d, first, count);
        if (!e)
            e = xb_fat_lookup(&fat, argv[5], &c, &size);
        struct sha_ctx sc;
        xb_sha256_init(&sc.s);
        if (!e)
            e = xb_fat_read(&fat, c, size, sha_sink, &sc);
        if (e) {
            printf("err %d\n", e);
            return 0;
        }
        uint8_t dg[32];
        xb_sha256_final(&sc.s, dg);
        hex(dg, 32, h);
        printf("%u %u %s\n", c, size, h);
        return 0;
    }
    if (!strcmp(argv[1], "check")) {
        struct xb_trial_report r;
        xb_trial_check(&d, argv[4], block, &r);
        hex(r.trial_sha256, 32, h);
        printf("boot=%s show=%d reason=%s write=%d watchdog=%d parse=%d\n",
               r.d.boot == XB_BS_BOOT_TRIAL ? "trial" : "own", r.d.show, xb_bs_reason_name(r.d.reason),
               r.d.write, r.d.watchdog, r.d.parse);
        printf("failed_at=%s err=%d part=%llu+%llu record=%u/%u block=%llu trial=%u/%u sha=%s reads=%ld\n",
               xb_trial_step_name(r.failed_at), r.err, (unsigned long long)r.part_first,
               (unsigned long long)r.part_blocks, r.rec_cluster, r.rec_size,
               (unsigned long long)r.rec_block, r.trial_cluster, r.trial_size, h, fd.reads);
        if (r.d.write) {
            char out[2 * XB_BS_BYTES + 1];
            hex(r.d.out, XB_BS_BYTES, out);
            printf("out=%s\n", out);
        }
        return 0;
    }
    return 2;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        return cli(argc, argv);
    test_sha256();
    test_uuid();
    test_full_directories();
    printf("esp: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
