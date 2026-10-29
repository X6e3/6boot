/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "el2_control.h"

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get64(const uint8_t *p)
{
    return get32(p) | (uint64_t)get32(p + 4) << 32;
}

static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static int valid(struct e2_range r)
{
    return r.size && r.base <= UINT64_MAX - r.size;
}

static int overlaps(struct e2_range a, struct e2_range b)
{
    return a.base < b.base + b.size && b.base < a.base + a.size;
}

static struct e2_range range_at(const uint8_t *p)
{
    return (struct e2_range){get64(p), get64(p + 8)};
}

int e2_layout_make(uint64_t arena, uint64_t size, struct e2_layout *out)
{
    if (!out) return E2_ARGUMENT;
    if (!arena || arena % E2_ARENA_ALIGN || size != E2_ARENA_SIZE ||
        !valid((struct e2_range){arena, size})) return E2_RANGE;
    /* One complete 16 KiB page for every slot except the final two-page reserve. */
    for (unsigned i = 0; i < E2_SLOT_COUNT; i++) {
        out->region[i].base = arena + (uint64_t)i * E2_PAGE_SIZE;
        out->region[i].size = i == E2_RESERVED ? 2u * E2_PAGE_SIZE : E2_PAGE_SIZE;
    }
    return E2_READY;
}

static int environment(const struct e2_snapshot *s)
{
    const uint64_t host = E2_HCR_E2H | E2_HCR_TGE | E2_HCR_RW;
    if (s->current_el != 8 || s->spsel != 1 || !s->active_sp ||
        s->active_sp % 16 || (s->daif & E2_DAIF_MASK) != E2_DAIF_MASK ||
        (s->hcr_el2 & host) != host || (s->hcr_el2 & E2_HCR_VM))
        return E2_ENVIRONMENT;
    return E2_READY;
}

static int validate(const uint8_t *p, size_t size,
                    const struct e2_snapshot *s, uint64_t image_base,
                    uintptr_t request, uintptr_t result)
{
    if (size < E2_HEADER_SIZE || size > E2_REQUEST_MAX) return E2_LENGTH;
    if (get32(p) != E2_REQUEST_MAGIC ||
        get32(p + 4) != (E2_HEADER_SIZE << 16 | E2_VERSION) ||
        get32(p + 8) != size || get32(p + 28) || get32(p + 52) ||
        get32(p + 60) || get64(p + 72)) return E2_ABI;
    if (get32(p + 16) != E2_CHIP || p[20] != 'J' || p[21] != '7' ||
        p[22] != '1' || p[23] != '3') return E2_PLATFORM;
    if (get32(p + 12) != E2_PREFLIGHT) return E2_MODE;
    if (get32(p + 24) != E2_REQUIRED_FLAGS) return E2_OWNERSHIP;
    unsigned nr = get32(p + 48), nx = get32(p + 56);
    if (!nr || nr > E2_MAX_RANGES || !nx || nx > E2_MAX_RANGES ||
        size != E2_HEADER_SIZE + 16u * (nr + nx)) return E2_LENGTH;
    int status = environment(s);
    if (status) return status;
    if (get64(p + 64) != s->mpidr_el1) return E2_ENVIRONMENT;

    struct e2_range arena = range_at(p + 32);
    struct e2_layout layout;
    if (e2_layout_make(arena.base, arena.size, &layout)) return E2_RANGE;
    if (arena.base != image_base) return E2_LOAD_ADDRESS;
    /* The caller's 112-byte saved-register frame must be outside our arena,
     * including when this module is called from P3's separate 16 KiB stack. */
    if (s->active_sp < E2_CALL_FRAME_SIZE ||
        overlaps(arena, (struct e2_range){s->active_sp - E2_CALL_FRAME_SIZE, E2_CALL_FRAME_SIZE}))
        return E2_ENVIRONMENT;
    if (overlaps(arena, (struct e2_range){request, size}) ||
        overlaps(arena, (struct e2_range){result, E2_RESULT_SIZE}))
        return E2_OVERLAP;

    const uint8_t *ram = p + E2_HEADER_SIZE;
    int covered = 0;
    for (unsigned i = 0; i < nr; i++) {
        struct e2_range r = range_at(ram + 16u * i);
        if (!valid(r)) return E2_RANGE;
        if (arena.base >= r.base && arena.base + arena.size <= r.base + r.size)
            covered = 1;
        for (unsigned j = 0; j < i; j++)
            if (overlaps(r, range_at(ram + 16u * j))) return E2_OVERLAP;
    }
    if (!covered) return E2_COVERAGE;
    const uint8_t *excluded = ram + 16u * nr;
    for (unsigned i = 0; i < nx; i++) {
        struct e2_range r = range_at(excluded + 16u * i);
        if (!valid(r)) return E2_RANGE;
        if (overlaps(arena, r)) return E2_OVERLAP;
    }
    return E2_READY;
}

int e2_buffers_check(const void *request, size_t size, void *result,
                      size_t capacity, size_t required,
                      const struct e2_snapshot *s, uint64_t image_base)
{
    uintptr_t a = (uintptr_t)request, b = (uintptr_t)result, c = (uintptr_t)s;
    if (!request || !result || !s || required < E2_RESULT_SIZE ||
        required > E2_PAGE_SIZE || capacity < required ||
        c % _Alignof(struct e2_snapshot) || size > UINTPTR_MAX - a ||
        required > UINTPTR_MAX - b || sizeof(*s) > UINTPTR_MAX - c)
        return E2_ARGUMENT;
    /* Protect our real code/state/stack before decoding any claimed arena.
     * In particular, an error record must never overwrite our own module. */
    struct e2_range image = {image_base, E2_ARENA_SIZE};
    if (!image_base || image_base % E2_ARENA_ALIGN || !valid(image) ||
        overlaps(image, (struct e2_range){a, size}) ||
        overlaps(image, (struct e2_range){b, required}))
        return E2_ARGUMENT;
    if (overlaps((struct e2_range){a, size}, (struct e2_range){b, required}) ||
        overlaps((struct e2_range){c, sizeof(*s)}, (struct e2_range){b, required}))
        return E2_ARGUMENT;
    /* Never write even an error record over the caller's saved registers/LR.
     * The assembly also rejects both spans before it saves that frame, so
     * an aliased request cannot be corrupted by the first STP either. */
    if (s->active_sp >= E2_CALL_FRAME_SIZE) {
        struct e2_range caller = {s->active_sp - E2_CALL_FRAME_SIZE, E2_CALL_FRAME_SIZE};
        if (overlaps(caller, (struct e2_range){a, size}) ||
            overlaps(caller, (struct e2_range){b, required}))
            return E2_ARGUMENT;
    }
    return E2_READY;
}

int e2_foundation_run(const void *request, size_t size,
                      void *result, size_t capacity,
                      const struct e2_snapshot *s, uint64_t image_base)
{
    int args = e2_buffers_check(request, size, result, capacity, E2_RESULT_SIZE, s, image_base);
    if (args) return args;
    uintptr_t a = (uintptr_t)request, b = (uintptr_t)result;
    const uint8_t *p = request;
    int status = validate(p, size, s, image_base, a, b);
    uint8_t *out = result;
    for (unsigned i = 0; i < E2_RESULT_SIZE; i++) out[i] = 0;
    put32(out, E2_RESULT_MAGIC);
    put32(out + 4, E2_RESULT_SIZE << 16 | E2_VERSION);
    put32(out + 8, (uint32_t)status);
    put32(out + 12, E2_RESULT_PREFLIGHT_ONLY |
          (status == E2_READY ? E2_RESULT_LAYOUT_VALID : 0));
    put64(out + 16, s->current_el);
    put64(out + 24, s->spsel);
    put64(out + 32, s->active_sp);
    put64(out + 40, s->sp_el0);
    put64(out + 48, s->vbar_el2);
    put64(out + 56, s->hcr_el2);
    put64(out + 64, s->daif);
    put64(out + 72, s->mpidr_el1);
    if (status == E2_READY) {
        put64(out + 80, get64(p + 32));
        put64(out + 88, get64(p + 40));
    }
    return status;
}
