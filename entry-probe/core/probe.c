/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "xboot_probe.h"

struct range { uint64_t start, size; };

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p)
{
    return le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static int valid(struct range r)
{
    return r.size && r.start <= UINT64_MAX - r.size;
}

static int overlap(struct range a, struct range b)
{
    return a.start < b.start + b.size && b.start < a.start + a.size;
}

static int covered(const struct range *ram, unsigned n, struct range r)
{
    for (unsigned i = 0; i < n; i++)
        if (r.start >= ram[i].start && r.start + r.size <= ram[i].start + ram[i].size)
            return 1;
    return 0;
}

static enum xp_status validate(const uint8_t *p, size_t size, uint64_t el)
{
    if (size < XP_HEADER_SIZE || size > XP_HEADER_SIZE + 16u * XP_MAX_RAM)
        return XP_LENGTH;
    if (le32(p) != XP_CONTEXT_MAGIC || le32(p + 4) != (XP_HEADER_SIZE << 16 | XP_VERSION) ||
        le32(p + 8) != size || le32(p + 20) || le32(p + 60))
        return XP_ABI;
    if (le32(p + 12) != XP_CHIP || p[16] != 'J' || p[17] != '7' || p[18] != '1' || p[19] != '3')
        return XP_PLATFORM;
    if (el != 4 && el != 8) return XP_EL;
    unsigned n = le32(p + 56);
    if (!n || n > XP_MAX_RAM || size != XP_HEADER_SIZE + 16u * n)
        return XP_LENGTH;
    struct range loader = {le64(p + 24), le64(p + 32)};
    struct range adt = {le64(p + 40), le64(p + 48)};
    struct range ram[XP_MAX_RAM];
    if (!valid(loader) || !valid(adt)) return XP_RANGE;
    if (overlap(loader, adt)) return XP_OVERLAP;
    for (unsigned i = 0; i < n; i++) {
        ram[i] = (struct range){le64(p + 64 + 16 * i), le64(p + 72 + 16 * i)};
        if (!valid(ram[i])) return XP_RANGE;
        for (unsigned j = 0; j < i; j++)
            if (overlap(ram[i], ram[j])) return XP_OVERLAP;
    }
    if (!covered(ram, n, loader) || !covered(ram, n, adt)) return XP_COVERAGE;
    return XP_OK;
}

int xp_probe(const void *context, size_t size, void *result, size_t capacity,
             uint64_t el, uint64_t mpidr)
{
    uintptr_t a = (uintptr_t)context, b = (uintptr_t)result;
    if (!context || !result || capacity < XP_RESULT_SIZE ||
        size > UINTPTR_MAX - a || XP_RESULT_SIZE > UINTPTR_MAX - b ||
        (a < b + XP_RESULT_SIZE && b < a + size))
        return XP_ARGUMENT;
    const uint8_t *p = context;
    enum xp_status status = validate(p, size, el);
    uint8_t *out = result;
    for (unsigned i = 0; i < XP_RESULT_SIZE; i++) out[i] = 0;
    put32(out, XP_RESULT_MAGIC);
    put32(out + 4, XP_VERSION);
    put32(out + 8, XP_RESULT_SIZE);
    put32(out + 12, status);
    put32(out + 16, (uint32_t)el);
    put32(out + 20, (uint32_t)mpidr);
    put32(out + 24, (uint32_t)(mpidr >> 32));
    put32(out + 28, 1); /* metadata-only: never hardware initialization proof */
    if (status == XP_OK) {
        put32(out + 32, XP_CHIP);
        put32(out + 36, le32(p + 56));
    }
    return status;
}
