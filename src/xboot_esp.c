/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include "xboot_esp.h"

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static uint32_t crc32(uint32_t crc, const uint8_t *p, size_t len)
{
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

int xb_blk_read(struct xb_blk *d, uint64_t block, void *buf)
{
    if (!d || !d->read || !buf)
        return XB_ESP_E_ARG;
    return d->read(d->ctx, block, buf) ? XB_ESP_E_IO : XB_ESP_OK;
}

/* Bytes [off, off + len) of the device through the one-block cache. */
static int dev_read(struct xb_blk *d, uint64_t off, void *buf, size_t len)
{
    uint8_t *out = buf;

    while (len) {
        uint64_t blk = off / d->bytes;
        size_t in = (size_t)(off % d->bytes), n = d->bytes - in < len ? d->bytes - in : len;
        if (d->cached != blk) {
            if (d->read(d->ctx, blk, d->cache)) {
                d->cached = UINT64_MAX;
                return XB_ESP_E_IO;
            }
            d->cached = blk;
        }
        memcpy(out, d->cache + in, n);
        out += n, off += n, len -= n;
    }
    return XB_ESP_OK;
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

int xb_uuid_parse(const char *t, uint8_t out[16])
{
    static const int dash[4] = {8, 13, 18, 23};
    uint8_t raw[16];
    int n = 0;

    if (!t || !out)
        return XB_ESP_E_ARG;
    for (int i = 0; i < 36; i++) {
        if (i == dash[0] || i == dash[1] || i == dash[2] || i == dash[3]) {
            if (t[i] != '-')
                return XB_ESP_E_ARG;
            continue;
        }
        int hi = hexval(t[i]), lo = hexval(t[i + 1]);
        if (hi < 0 || lo < 0)
            return XB_ESP_E_ARG;
        raw[n++] = (uint8_t)(hi << 4 | lo);
        i++;
    }
    if (t[36] || n != 16)
        return XB_ESP_E_ARG;
    static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    for (int i = 0; i < 16; i++)
        out[i] = raw[order[i]];
    return XB_ESP_OK;
}

int xb_gpt_find(struct xb_blk *d, const uint8_t uuid[16], uint64_t *first, uint64_t *count)
{
    uint8_t h[92], e[128];

    if (!d || !d->read || !d->cache || !uuid || !first || !count || d->bytes < 512)
        return XB_ESP_E_ARG;
    if (dev_read(d, d->bytes, h, sizeof(h)))
        return XB_ESP_E_IO;
    uint32_t hsize = le32(h + 12), n = le32(h + 80), esize = le32(h + 84);
    if (memcmp(h, "EFI PART", 8) || hsize < 92 || hsize > d->bytes || le64(h + 24) != 1 ||
        esize < 128 || esize > 4096 || (esize & 7) || !n || n > 1024)
        return XB_ESP_E_GPT;
    /* the header's CRC (with its own field as zero), then the entry array's */
    uint8_t hc[512];
    if (hsize > sizeof(hc) || dev_read(d, d->bytes, hc, hsize))
        return hsize > sizeof(hc) ? XB_ESP_E_GPT : XB_ESP_E_IO;
    memset(hc + 16, 0, 4);
    if (crc32(0, hc, hsize) != le32(h + 16))
        return XB_ESP_E_GPT;
    uint64_t at = le64(h + 72) * d->bytes;
    uint32_t ecrc = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t chunk[128];
        for (uint32_t o = 0; o < esize; o += sizeof(chunk)) {
            if (dev_read(d, at + (uint64_t)i * esize + o, chunk, sizeof(chunk)))
                return XB_ESP_E_IO;
            ecrc = crc32(ecrc, chunk, sizeof(chunk));
        }
    }
    if (ecrc != le32(h + 88))
        return XB_ESP_E_GPT;
    for (uint32_t i = 0; i < n; i++) {
        if (dev_read(d, at + (uint64_t)i * esize, e, sizeof(e)))
            return XB_ESP_E_IO;
        static const uint8_t none[16];
        if (!memcmp(e, none, 16) || memcmp(e + 16, uuid, 16))
            continue;
        uint64_t lo = le64(e + 32), hi = le64(e + 40);
        if (hi < lo || !lo)
            return XB_ESP_E_GPT;
        *first = lo;
        *count = hi - lo + 1;
        return XB_ESP_OK;
    }
    return XB_ESP_E_NOPART;
}

static int fat_read_bytes(struct xb_fat *f, uint64_t off, void *buf, size_t len)
{
    uint64_t part_bytes = f->part_blocks * f->d->bytes;
    if (off > part_bytes || len > part_bytes - off)
        return XB_ESP_E_FAT;
    return dev_read(f->d, f->part_first * f->d->bytes + off, buf, len);
}

int xb_fat_mount(struct xb_fat *f, struct xb_blk *d, uint64_t part_first, uint64_t part_blocks)
{
    if (!f || !d || !d->read || !d->cache || !d->bytes || !part_blocks)
        return XB_ESP_E_ARG;
    memset(f, 0, sizeof(*f));
    f->d = d;
    f->part_first = part_first;
    f->part_blocks = part_blocks;
    if (fat_read_bytes(f, 0, f->bpb, 512))
        return XB_ESP_E_IO;
    const uint8_t *b = f->bpb;
    uint32_t bps = le16(b + 11), spc = b[13], reserved = le16(b + 14), fats = b[16];
    uint32_t total = le16(b + 19) ? le16(b + 19) : le32(b + 32);
    uint32_t fat_size = le32(b + 36), root = le32(b + 44);
    /* FAT32 only: no fixed root directory, a 32-bit FAT size, and a root cluster */
    if (b[510] != 0x55 || b[511] != 0xaa || bps < 512 || bps > 4096 || (bps & (bps - 1)) || !spc ||
        (spc & (spc - 1)) || !reserved || !fats || fats > 2 || le16(b + 17) || le16(b + 22) ||
        !fat_size || root < 2)
        return XB_ESP_E_FAT;
    uint64_t data = reserved + (uint64_t)fats * fat_size;
    if (data >= total || (uint64_t)total * bps > part_blocks * d->bytes)
        return XB_ESP_E_FAT;
    uint64_t clusters = (total - data) / spc;
    if (clusters < 65525 || root - 2 >= clusters)
        return XB_ESP_E_FAT;
    f->bps = bps;
    f->spc = spc;
    f->fat_start = reserved;
    f->fat_size = fat_size;
    f->root_cluster = root;
    f->clusters = (uint32_t)clusters;
    f->data_start = data;
    return XB_ESP_OK;
}

/* The cluster after c, 0 at the end of the chain, or an error. */
static int fat_next(struct xb_fat *f, uint32_t c, uint32_t *next)
{
    uint8_t v[4];
    if (c < 2 || c - 2 >= f->clusters)
        return XB_ESP_E_CHAIN;
    if (fat_read_bytes(f, (uint64_t)f->fat_start * f->bps + 4ULL * c, v, 4))
        return XB_ESP_E_IO;
    uint32_t n = le32(v) & 0x0fffffff;
    if (n >= 0x0ffffff8) {
        *next = 0;
        return XB_ESP_OK;
    }
    if (n < 2 || n - 2 >= f->clusters)
        return XB_ESP_E_CHAIN;
    *next = n;
    return XB_ESP_OK;
}

/* Brent's cycle check on a cluster chain: no memory, no extra reads. A chain that comes back to
 * a cluster it has passed is caught within a few laps, whatever the file's size says. */
struct loop {
    uint32_t mark, power, lam;
};

static void loop_init(struct loop *l, uint32_t first)
{
    l->mark = first;
    l->power = 1;
    l->lam = 0;
}

static int loop_seen(struct loop *l, uint32_t c)
{
    if (c == l->mark)
        return 1;
    if (++l->lam == l->power) {
        l->mark = c;
        l->power <<= 1;
        l->lam = 0;
    }
    return 0;
}

static uint64_t cluster_off(struct xb_fat *f, uint32_t c)
{
    return (f->data_start + (uint64_t)(c - 2) * f->spc) * f->bps;
}

static char up(char c)
{
    return c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
}

/* Does the directory entry name match [name, name + len)? Long names: ASCII only. */
static int name_is(const char *lfn, int lfn_ok, const uint8_t *e, const char *name, size_t len)
{
    if (lfn_ok) {
        size_t i = 0;
        for (; lfn[i] && i < len; i++)
            if (up(lfn[i]) != up(name[i]))
                return 0;
        return i == len && !lfn[i];
    }
    char short_name[13];
    int n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++)
        short_name[n++] = (char)e[i];
    if (e[8] != ' ') {
        short_name[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++)
            short_name[n++] = (char)e[i];
    }
    if ((size_t)n != len)
        return 0;
    for (int i = 0; i < n; i++)
        if (up(short_name[i]) != up(name[i]))
            return 0;
    return 1;
}

/* Find one name in the directory that starts at cluster dir. */
static int dir_find(struct xb_fat *f, uint32_t dir, const char *name, size_t len, uint32_t *first,
                    uint32_t *size, int *is_dir)
{
    char lfn[261];
    int lfn_ok = 0;
    uint32_t c = dir, cbytes = f->spc * f->bps;
    struct loop lp;

    loop_init(&lp, dir);

    for (uint32_t hops = 0; c; hops++) {
        if (hops > f->clusters)
            return XB_ESP_E_CHAIN;
        for (uint32_t o = 0; o < cbytes; o += 32) {
            uint8_t e[32];
            if (fat_read_bytes(f, cluster_off(f, c) + o, e, 32))
                return XB_ESP_E_IO;
            if (!e[0])
                return XB_ESP_E_NOENT;
            if (e[0] == 0xe5) {
                lfn_ok = 0;
                continue;
            }
            if (e[11] == 0x0f) { /* long name piece: 13 UTF-16 units, ASCII kept, others refused */
                int seq = e[0] & 0x1f;
                static const int at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                if (!seq || seq > 20) {
                    lfn_ok = 0;
                    continue;
                }
                if (e[0] & 0x40) {
                    memset(lfn, 0, sizeof(lfn));
                    lfn_ok = 1;
                }
                for (int k = 0; k < 13 && lfn_ok; k++) {
                    uint16_t u = le16(e + at[k]);
                    int pos = (seq - 1) * 13 + k;
                    if (u == 0 || u == 0xffff)
                        break;
                    if (u > 0x7f || pos >= 260)
                        lfn_ok = 0;
                    else
                        lfn[pos] = (char)u;
                }
                continue;
            }
            if (e[11] & 0x08) { /* volume label */
                lfn_ok = 0;
                continue;
            }
            int hit = name_is(lfn, lfn_ok, e, name, len);
            lfn_ok = 0;
            if (hit) {
                *first = (uint32_t)le16(e + 20) << 16 | le16(e + 26);
                *size = le32(e + 28);
                *is_dir = !!(e[11] & 0x10);
                return XB_ESP_OK;
            }
        }
        int r = fat_next(f, c, &c);
        if (r)
            return r;
        if (c && loop_seen(&lp, c))
            return XB_ESP_E_CHAIN;
    }
    return XB_ESP_E_NOENT;
}

int xb_fat_lookup(struct xb_fat *f, const char *path, uint32_t *first_cluster, uint32_t *size)
{
    uint32_t c, s = 0;
    int is_dir = 1;

    if (!f || !f->d || !path || !*path || !first_cluster || !size)
        return XB_ESP_E_ARG;
    c = f->root_cluster;
    while (*path) {
        const char *end = path;
        while (*end && *end != '/')
            end++;
        if (end == path || !is_dir)
            return XB_ESP_E_NOENT;
        int r = dir_find(f, c, path, (size_t)(end - path), &c, &s, &is_dir);
        if (r)
            return r;
        path = *end ? end + 1 : end;
    }
    if (is_dir)
        return XB_ESP_E_NOENT;
    *first_cluster = c;
    *size = s;
    return XB_ESP_OK;
}

int xb_fat_read(struct xb_fat *f, uint32_t c, uint32_t size, xb_sink sink, void *ctx)
{
    uint8_t buf[512];
    uint32_t cbytes, left = size;

    if (!f || !f->d || !sink)
        return XB_ESP_E_ARG;
    if (!size)
        return XB_ESP_OK;
    cbytes = f->spc * f->bps;
    struct loop lp;
    loop_init(&lp, c);
    for (uint32_t hops = 0; left; hops++) {
        if (!c || hops > f->clusters)
            return XB_ESP_E_CHAIN;
        uint64_t off = cluster_off(f, c);
        for (uint32_t o = 0; o < cbytes && left; o += sizeof(buf)) {
            uint32_t n = left < sizeof(buf) ? left : sizeof(buf);
            if (fat_read_bytes(f, off + o, buf, n))
                return XB_ESP_E_IO;
            if (sink(ctx, buf, n))
                return XB_ESP_E_SINK;
            left -= n;
        }
        if (left) {
            int r = fat_next(f, c, &c);
            if (r)
                return r;
            if (c && loop_seen(&lp, c))
                return XB_ESP_E_CHAIN;
        }
    }
    return XB_ESP_OK;
}
