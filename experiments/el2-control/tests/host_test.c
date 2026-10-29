/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "el2_control.h"
#include "el2_trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Synthetic coordinates only: never device load-address recommendations. */
#define TEST_BASE UINT64_C(0x100000000)
static uint8_t request[E2_REQUEST_MAX + 16];
static uint8_t output[E2_RESULT_SIZE + 32];
static struct e2_snapshot snapshot;
static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static void put64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const uint8_t *p)
{
    return get32(p) | (uint64_t)get32(p + 4) << 32;
}
static void range(unsigned index, uint64_t base, uint64_t size)
{
    put64(request + E2_HEADER_SIZE + 16u * index, base);
    put64(request + E2_HEADER_SIZE + 16u * index + 8, size);
}
static void fixture(void)
{
    memset(request, 0, sizeof(request));
    put32(request, E2_REQUEST_MAGIC);
    put32(request + 4, E2_HEADER_SIZE << 16 | E2_VERSION);
    put32(request + 8, E2_HEADER_SIZE + 32);
    put32(request + 12, E2_PREFLIGHT);
    put32(request + 16, E2_CHIP);
    memcpy(request + 20, "J713", 4);
    put32(request + 24, E2_REQUIRED_FLAGS);
    put64(request + 32, TEST_BASE);
    put64(request + 40, E2_ARENA_SIZE);
    put32(request + 48, 1);
    put32(request + 56, 1);
    put64(request + 64, UINT64_C(0x80000000));
    range(0, TEST_BASE - 0x100000, 0x1000000);
    range(1, TEST_BASE - 0x20000, 0x10000);
    snapshot = (struct e2_snapshot){
        .current_el = 8, .spsel = 1, .active_sp = TEST_BASE - 0x1000,
        .sp_el0 = 0x12345000, .vbar_el2 = TEST_BASE - 0x8000,
        .hcr_el2 = E2_HCR_E2H | E2_HCR_TGE | E2_HCR_RW,
        .daif = E2_DAIF_MASK, .mpidr_el1 = UINT64_C(0x80000000),
    };
}
static void guards(void)
{
    for (unsigned i = 0; i < 8; i++) CHECK(output[i] == 0xa5);
    for (size_t i = 8 + E2_RESULT_SIZE; i < sizeof(output); i++)
        CHECK(output[i] == 0xa5);
}
static void expect_size(int expected, size_t size, uint64_t load)
{
    uint8_t before[sizeof(request)];
    memcpy(before, request, sizeof(request));
    memset(output, 0xa5, sizeof(output));
    int status = e2_foundation_run(request, size, output + 8, E2_RESULT_SIZE,
                                    &snapshot, load);
    CHECK(status == expected);
    CHECK(memcmp(before, request, sizeof(request)) == 0);
    guards();
    CHECK(get32(output + 8) == E2_RESULT_MAGIC);
    CHECK(get32(output + 12) == (E2_RESULT_SIZE << 16 | E2_VERSION));
    CHECK(get32(output + 16) == (uint32_t)expected);
    CHECK(get32(output + 20) == (E2_RESULT_PREFLIGHT_ONLY |
          (expected == E2_READY ? E2_RESULT_LAYOUT_VALID : 0)));
    for (unsigned i = 96; i < E2_RESULT_SIZE; i++) CHECK(output[8 + i] == 0);
    if (expected != E2_READY) {
        CHECK(get64(output + 88) == 0);
        CHECK(get64(output + 96) == 0);
    }
}
static void expect(int expected)
{
    expect_size(expected, E2_HEADER_SIZE + 32, TEST_BASE);
}
static void valid_record(void)
{
    fixture(); expect(E2_READY);
    const uint64_t fields[] = {snapshot.current_el, snapshot.spsel, snapshot.active_sp,
        snapshot.sp_el0, snapshot.vbar_el2, snapshot.hcr_el2, snapshot.daif, snapshot.mpidr_el1};
    for (unsigned i = 0; i < 8; i++) CHECK(get64(output + 24 + 8u * i) == fields[i]);
    CHECK(get64(output + 88) == TEST_BASE);
    CHECK(get64(output + 96) == E2_ARENA_SIZE);
}
static void invalid_arguments(void)
{
    fixture(); memset(output, 0xa5, sizeof(output));
    CHECK(e2_foundation_run(NULL, 112, output + 8, 128, &snapshot, TEST_BASE) == E2_ARGUMENT);
    CHECK(e2_foundation_run(request, 112, NULL, 128, &snapshot, TEST_BASE) == E2_ARGUMENT);
    CHECK(e2_foundation_run(request, 112, output + 8, 127, &snapshot, TEST_BASE) == E2_ARGUMENT);
    CHECK(e2_foundation_run(request, 112, output + 8, 128, NULL, TEST_BASE) == E2_ARGUMENT);
    CHECK(e2_foundation_run((void *)(UINTPTR_MAX - 4), 16, output + 8, 128,
                            &snapshot, TEST_BASE) == E2_ARGUMENT);
    CHECK(e2_foundation_run(request, 112, (void *)(UINTPTR_MAX - 64), 128,
                            &snapshot, TEST_BASE) == E2_ARGUMENT);
    CHECK(e2_foundation_run(request, 112, output + 8, 128,
          (const struct e2_snapshot *)((const uint8_t *)&snapshot + 1), TEST_BASE) == E2_ARGUMENT);
    CHECK(e2_foundation_run(request, 112, output + 8, 128, &snapshot, 0) == E2_ARGUMENT);
    CHECK(e2_foundation_run(request, 112, output + 8, 128, &snapshot, TEST_BASE + 1) == E2_ARGUMENT);
    CHECK(e2_foundation_run(request, 112, output + 8, 128, &snapshot,
                            UINT64_C(0xffffffffffff0000)) == E2_ARGUMENT);
    for (size_t i = 0; i < sizeof(output); i++) CHECK(output[i] == 0xa5);
    uint8_t before[sizeof(request)]; memcpy(before, request, sizeof(request));
    CHECK(e2_foundation_run(request, 112, request, 128, &snapshot, TEST_BASE) == E2_ARGUMENT);
    CHECK(e2_foundation_run(request, 112, request + 111, 128, &snapshot, TEST_BASE) == E2_ARGUMENT);
    CHECK(memcmp(before, request, sizeof(request)) == 0);
    struct e2_snapshot old = snapshot;
    CHECK(e2_foundation_run(request, 112, &snapshot, 128, &snapshot, TEST_BASE) == E2_ARGUMENT);
    CHECK(memcmp(&old, &snapshot, sizeof(old)) == 0);
}
static void caller_frame_aliases(void)
{
    /* Mapped host bytes model the caller save frame. An error result must
     * not overwrite saved x18..x30, even when the request itself is invalid. */
    _Alignas(16) uint8_t memory[4096], before[4096];
    fixture(); memset(memory, 0xa5, sizeof(memory)); memcpy(before, memory, sizeof(memory));
    snapshot.active_sp = (uintptr_t)memory + 2048;
    const unsigned at[] = {1935, 1936, 2047};
    for (unsigned i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
        CHECK(e2_foundation_run(request, 112, memory + at[i], E2_RESULT_SIZE,
                                &snapshot, TEST_BASE) == E2_ARGUMENT);
        CHECK(memcmp(memory, before, sizeof(memory)) == 0);
        /* The exception entry uses the complete 2048-byte output span. */
        CHECK(e2_buffers_check(request, 112, memory + at[i], 2048, 2048,
                               &snapshot, TEST_BASE) == E2_ARGUMENT);
    }
    CHECK(e2_buffers_check(request, 112, memory + 2048, 2048, 2048,
                           &snapshot, TEST_BASE) == E2_READY);
    CHECK(e2_buffers_check(request, 112, memory + 1808, 128, 128,
                           &snapshot, TEST_BASE) == E2_READY);
    fixture(); snapshot.active_sp = (uintptr_t)request + 112;
    CHECK(e2_buffers_check(request, 112, output, sizeof(output), E2_RESULT_SIZE,
                           &snapshot, TEST_BASE) == E2_ARGUMENT);
}
static void truncated_and_header(void)
{
    for (size_t n = 0; n < E2_HEADER_SIZE; n++) { fixture(); expect_size(E2_LENGTH, n, TEST_BASE); }
    fixture(); expect_size(E2_LENGTH, E2_REQUEST_MAX + 1, TEST_BASE);
    const unsigned reserved[] = {28, 52, 60, 72, 76};
    for (unsigned i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        fixture(); put32(request + reserved[i], 1); expect(E2_ABI);
    }
    fixture(); put32(request, 0); expect(E2_ABI);
    fixture(); request[4] = 2; expect(E2_ABI);
    fixture(); request[6] = 64; expect(E2_ABI);
    fixture(); put32(request + 8, 113); expect(E2_ABI);
}
static void platform_mode_flags(void)
{
    fixture(); put32(request + 16, 0x8112); expect(E2_PLATFORM);
    fixture(); request[23] = '5'; expect(E2_PLATFORM);
    fixture(); put32(request + 12, 2); expect(E2_MODE); /* No guest-mode fallthrough. */
    fixture(); put32(request + 12, UINT32_MAX); expect(E2_MODE);
    const uint32_t bad_flags[] = {0, E2_PRIVATE_ARENA, E2_EXCLUSIONS_COMPLETE, 7, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(bad_flags) / sizeof(bad_flags[0]); i++) {
        fixture(); put32(request + 24, bad_flags[i]); expect(E2_OWNERSHIP);
    }
}
static void environment_checks(void)
{
    const uint64_t bad_el[] = {0, 2, 4, 12};
    for (unsigned i = 0; i < 4; i++) { fixture(); snapshot.current_el = bad_el[i]; expect(E2_ENVIRONMENT); }
    fixture(); snapshot.spsel = 0; expect(E2_ENVIRONMENT);
    fixture(); snapshot.active_sp = 0; expect(E2_ENVIRONMENT);
    fixture(); snapshot.active_sp++; expect(E2_ENVIRONMENT);
    for (unsigned bit = 6; bit <= 9; bit++) {
        fixture(); snapshot.daif &= ~(UINT64_C(1) << bit); expect(E2_ENVIRONMENT);
    }
    const uint64_t host[] = {E2_HCR_E2H, E2_HCR_TGE, E2_HCR_RW};
    for (unsigned i = 0; i < 3; i++) {
        fixture(); snapshot.hcr_el2 &= ~host[i]; expect(E2_ENVIRONMENT);
    }
    fixture(); snapshot.hcr_el2 |= E2_HCR_VM; expect(E2_ENVIRONMENT);
    fixture(); snapshot.mpidr_el1++; expect(E2_ENVIRONMENT);
    fixture(); snapshot.hcr_el2 |= UINT64_C(1) << 41; expect(E2_READY);
}
static void range_counts(void)
{
    const uint32_t bad[] = {0, E2_MAX_RANGES + 1, UINT32_MAX};
    for (unsigned i = 0; i < 3; i++) {
        fixture(); put32(request + 48, bad[i]); expect(E2_LENGTH);
        fixture(); put32(request + 56, bad[i]); expect(E2_LENGTH);
    }
    fixture(); put32(request + 48, 2); expect(E2_LENGTH);
}
static void arena_geometry(void)
{
    fixture(); put64(request + 32, 0); expect(E2_RANGE);
    fixture(); put64(request + 32, TEST_BASE + 1); expect(E2_RANGE);
    fixture(); put64(request + 40, E2_ARENA_SIZE - 1); expect(E2_RANGE);
    fixture(); put64(request + 40, E2_ARENA_SIZE + 1); expect(E2_RANGE);
    fixture(); put64(request + 32, UINT64_C(0xffffffffffff0000)); expect(E2_RANGE);
    fixture(); expect_size(E2_LOAD_ADDRESS, 112, TEST_BASE + E2_ARENA_ALIGN);
}
static void ram_and_exclusions(void)
{
    fixture(); range(0, TEST_BASE, 0); expect(E2_RANGE);
    fixture(); range(0, UINT64_MAX - 8, 16); expect(E2_RANGE);
    fixture(); range(0, TEST_BASE + E2_ARENA_SIZE, 0x40000); expect(E2_COVERAGE);
    fixture(); range(0, TEST_BASE, E2_ARENA_SIZE - 1); expect(E2_COVERAGE);
    fixture(); range(0, TEST_BASE, E2_ARENA_SIZE); expect(E2_READY);
    fixture(); range(1, TEST_BASE - 0x1000, 0x1000); expect(E2_READY);
    fixture(); range(1, TEST_BASE + E2_ARENA_SIZE, 0x1000); expect(E2_READY);
    fixture(); range(1, 0x1000, 0); expect(E2_RANGE);
    fixture(); range(1, UINT64_MAX, 1); expect(E2_RANGE);
    for (unsigned i = 0; i < 8; i++) {
        fixture(); range(1, TEST_BASE + (uint64_t)i * E2_PAGE_SIZE, 1); expect(E2_OVERLAP);
    }
    fixture(); range(1, TEST_BASE - 1, 2); expect(E2_OVERLAP);
    fixture(); range(1, TEST_BASE + E2_ARENA_SIZE - 1, 2); expect(E2_OVERLAP);
}
static void multiple_ranges(void)
{
    fixture(); put32(request + 8, 128); put32(request + 48, 2);
    range(0, TEST_BASE, 0x10000); range(1, TEST_BASE + 0x10000, 0x10000);
    range(2, TEST_BASE - 0x10000, 0x1000);
    expect_size(E2_COVERAGE, 128, TEST_BASE); /* No spanning multiple RAM claims. */
    range(0, TEST_BASE, E2_ARENA_SIZE); expect_size(E2_OVERLAP, 128, TEST_BASE);
    range(1, TEST_BASE + E2_ARENA_SIZE, 0x10000); expect_size(E2_READY, 128, TEST_BASE);
    fixture(); put32(request + 8, 128); put32(request + 56, 2);
    range(2, TEST_BASE - 0x20000, 0x10000); expect_size(E2_READY, 128, TEST_BASE);
}
static void buffer_inside_arena(void)
{
    fixture();
    uint8_t *allocation = malloc(E2_ARENA_SIZE + E2_ARENA_ALIGN);
    CHECK(allocation != NULL);
    uintptr_t base = ((uintptr_t)allocation + E2_ARENA_ALIGN - 1) &
                     ~((uintptr_t)E2_ARENA_ALIGN - 1);
    uint8_t *owned = (uint8_t *)base;
    memset(owned, 0xa5, E2_ARENA_SIZE);
    /* Even a forged request claiming a different arena cannot turn an error
     * report into a write into the real code, state, stack or trace pages. */
    for (unsigned i = 0; i < 8; i++) {
        uint8_t *inside = owned + (size_t)i * E2_PAGE_SIZE;
        CHECK(e2_foundation_run(request, 112, inside, 128, &snapshot, base) == E2_ARGUMENT);
        for (unsigned j = 0; j < E2_RESULT_SIZE; j++) CHECK(inside[j] == 0xa5);
    }
    memcpy(owned + E2_GUEST_OFFSET, request, 112);
    memset(output, 0xa5, sizeof(output));
    CHECK(e2_foundation_run(owned + E2_GUEST_OFFSET, 112, output + 8, 128,
                            &snapshot, base) == E2_ARGUMENT);
    for (size_t i = 0; i < sizeof(output); i++) CHECK(output[i] == 0xa5);
    free(allocation);
}
static void unaligned_buffers(void)
{
    fixture(); uint8_t unaligned[E2_REQUEST_MAX + 1];
    memcpy(unaligned + 1, request, 112); memset(output, 0xa5, sizeof(output));
    CHECK(e2_foundation_run(unaligned + 1, 112, output + 1, 128,
                            &snapshot, TEST_BASE) == E2_READY);
    CHECK(get32(output + 1) == E2_RESULT_MAGIC);
    CHECK(output[0] == 0xa5 && output[129] == 0xa5);
}
static void derived_layout(void)
{
    struct e2_layout layout, before;
    memset(&layout, 0xa5, sizeof(layout)); before = layout;
    CHECK(e2_layout_make(TEST_BASE, E2_ARENA_SIZE, NULL) == E2_ARGUMENT);
    CHECK(e2_layout_make(TEST_BASE + 1, E2_ARENA_SIZE, &layout) == E2_RANGE);
    CHECK(memcmp(&layout, &before, sizeof(layout)) == 0);
    CHECK(e2_layout_make(TEST_BASE, E2_ARENA_SIZE, &layout) == E2_READY);
    uint64_t end = TEST_BASE;
    for (unsigned i = 0; i < E2_SLOT_COUNT; i++) {
        CHECK(layout.region[i].base == end);
        end += layout.region[i].size;
    }
    CHECK(end == TEST_BASE + E2_ARENA_SIZE);
    CHECK(layout.region[E2_STACK].base == TEST_BASE + E2_STACK_OFFSET);
    CHECK(layout.region[E2_GUEST].base == TEST_BASE + E2_GUEST_OFFSET);
    CHECK(layout.region[E2_TRACE].base == TEST_BASE + E2_TRACE_OFFSET);
}
static void bounded_trace(void)
{
    struct { uint64_t a; struct e2_trace t; uint64_t b; } guarded;
    guarded.a = guarded.b = UINT64_C(0x1122334455667788);
    e2_trace_reset(&guarded.t);
    CHECK(e2_trace_push(NULL, E2_EVENT_ENTRY, 0, 0, 0) == E2_ARGUMENT);
    CHECK(e2_trace_push(&guarded.t, 0, 0, 0, 0) == E2_ARGUMENT);
    CHECK(guarded.t.count == 0 && guarded.t.dropped == 0);
    for (unsigned i = 0; i < E2_TRACE_CAPACITY; i++) {
        CHECK(e2_trace_push(&guarded.t, E2_EVENT_CHECK, E2_RANGE, i, i + 9u) == E2_READY);
        CHECK(guarded.t.event[i].sequence == i + 1);
        CHECK(guarded.t.event[i].ticks == i && guarded.t.event[i].argument == i + 9u);
        CHECK(guarded.t.event[i].reserved == 0);
    }
    struct e2_event last = guarded.t.event[E2_TRACE_CAPACITY - 1];
    CHECK(e2_trace_push(&guarded.t, E2_EVENT_RETURN, 0, 0, 0) == E2_CAPACITY);
    CHECK(guarded.t.count == E2_TRACE_CAPACITY && guarded.t.dropped == 1);
    CHECK(memcmp(&last, &guarded.t.event[E2_TRACE_CAPACITY - 1], sizeof(last)) == 0);
    guarded.t.dropped = UINT32_MAX;
    CHECK(e2_trace_push(&guarded.t, E2_EVENT_RETURN, 0, 0, 0) == E2_CAPACITY);
    CHECK(guarded.t.dropped == UINT32_MAX);
    guarded.t.count = E2_TRACE_CAPACITY + 1;
    CHECK(e2_trace_push(&guarded.t, E2_EVENT_ENTRY, 0, 0, 0) == E2_ARGUMENT);
    CHECK(guarded.a == UINT64_C(0x1122334455667788) && guarded.a == guarded.b);
    e2_trace_reset(&guarded.t);
    const uint8_t *bytes = (const uint8_t *)&guarded.t;
    for (size_t i = 0; i < sizeof(guarded.t); i++) CHECK(bytes[i] == 0);
}
static void bounded_parser_mutations(void)
{
    uint32_t seed = 0x8132713;
    for (unsigned trial = 0; trial < 1000; trial++) {
        fixture(); seed = seed * 1664525u + 1013904223u;
        unsigned at = seed % 112u;
        request[at] ^= (uint8_t)((seed >> 16) | 1u);
        uint8_t before[sizeof(request)]; memcpy(before, request, sizeof(request));
        memset(output, 0xa5, sizeof(output));
        int status = e2_foundation_run(request, 112, output + 8, 128, &snapshot, TEST_BASE);
        CHECK(status >= E2_READY && status <= E2_CAPACITY && status != E2_ARGUMENT);
        CHECK(get32(output + 16) == (uint32_t)status);
        CHECK(memcmp(before, request, sizeof(request)) == 0);
        guards();
    }
}

int main(void)
{
    valid_record(); invalid_arguments(); caller_frame_aliases(); truncated_and_header(); platform_mode_flags();
    environment_checks(); range_counts(); arena_geometry(); ram_and_exclusions();
    multiple_ranges(); buffer_inside_arena(); unaligned_buffers(); derived_layout();
    bounded_trace(); bounded_parser_mutations();
    printf("PASS: 15 host scenarios, %u assertions (synthetic state; no device access)\n", checks);
    return 0;
}
