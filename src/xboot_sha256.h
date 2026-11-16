/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef XBOOT_SHA256_H
#define XBOOT_SHA256_H
#include <stddef.h>
#include <stdint.h>

/* SHA-256 (FIPS 180-4), for checking a trial image against its record. Integer only. */
struct xb_sha256 {
    uint32_t h[8];
    uint64_t bytes;
    uint8_t block[64];
    size_t used;
};

void xb_sha256_init(struct xb_sha256 *s);
void xb_sha256_update(struct xb_sha256 *s, const void *data, size_t len);
void xb_sha256_final(struct xb_sha256 *s, uint8_t out[32]);
#endif
