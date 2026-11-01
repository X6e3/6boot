/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host policy/publication tests use the repository's real libfdt sources. */
#include "visual_smoke.h"
#include "libfdt/libfdt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAGIC UINT32_C(0x53563045)
#define PROPERTY "6boot,e0-visual-result"

static unsigned checks;
#define CHECK(expression) do { \
    checks++; \
    if (!(expression)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
        exit(1); \
    } \
} while (0)

static struct e0_visual_smoke_v2 success(void)
{
    struct e0_visual_smoke_v2 r = {0};
    r.v1.magic = MAGIC;
    r.v1.version = 2;
    r.v1.size = (uint16_t)sizeof(r);
    r.v1.frames = 3;
    r.v1.hcr_before = r.v1.hcr_after = UINT64_C(0x488000000);
    r.v1.build = UINT64_C(0x123456789abcdef0);
    r.v1.ctr = UINT64_C(4) << 16;
    r.v1.framebuffer = UINT64_C(0x804000000);
    r.v1.width = 640;
    r.v1.height = 360;
    r.v1.stride = 2624;
    r.v1.depth = 32;
    r.outcome = E0VS_COMPLETE;
    r.stage = 8;
    r.failed_frame = 3;
    r.diagnostic_flags = 0x7c;
    r.safety = E0VS_SAFE;
    r.saved_par = r.restored_par = UINT64_C(0x123456789000);
    r.daif_before = r.daif_after = UINT64_C(0x3c0);
    r.current_el = 8;
    r.sctlr = 1;
    r.tcr = (UINT64_C(3) << 32) | (UINT64_C(2) << 14) | 16;
    r.extent = UINT64_C(2624) * 360;
    r.cache_line = 64;
    return r;
}

static struct e0_visual_smoke_v2 published_success(void)
{
    struct e0_visual_smoke_v2 r = success();
    r.publication = E0VS_PUB_COMPLETE;
    r.publication_step = 2;
    r.decision = E0VS_CONTINUE;
    return r;
}

static void new_tree(void *dt, int bytes, const char *node)
{
    CHECK(fdt_create_empty_tree(dt, bytes) == 0);
    if (node)
        CHECK(fdt_add_subnode(dt, 0, node) >= 0);
}

static struct e0_visual_smoke_v2 read_record(const void *dt)
{
    struct e0_visual_smoke_v2 r;
    int node = fdt_path_offset(dt, "/chosen"), bytes = 0;
    CHECK(node >= 0);
    const void *p = fdt_getprop(dt, node, PROPERTY, &bytes);
    CHECK(p && bytes == (int)sizeof(r));
    /* Properties have four-byte alignment; the record contains uint64_t. */
    memcpy(&r, p, sizeof(r));
    return r;
}

static void check_pending(const void *dt)
{
    struct e0_visual_smoke_v2 expected = {0};
    expected.v1.magic = MAGIC;
    expected.v1.version = 2;
    expected.v1.size = (uint16_t)sizeof(expected);
    struct e0_visual_smoke_v2 actual = read_record(dt);
    CHECK(memcmp(&actual, &expected, sizeof(actual)) == 0);
    CHECK(e0_visual_smoke_decision(&actual, 1, NULL) == E0VS_BLOCK_CONTEXT);
}

static void check_failed(const struct e0_visual_smoke_v2 *r, int error,
                         unsigned step, unsigned decision)
{
    CHECK(r->publication == E0VS_PUB_FAILED);
    CHECK(r->publication_error == error);
    CHECK(r->publication_step == step);
    CHECK(r->decision == decision);
    CHECK(e0_visual_smoke_decision(r, 1, NULL) == decision);
}

static void test_success_and_safe_refusals(void)
{
    _Alignas(8) unsigned char dt[4096];
    new_tree(dt, sizeof(dt), "chosen");
    CHECK(e0_visual_smoke_reserve(dt) == 0);
    check_pending(dt);
    CHECK(fdt_pack(dt) == 0);
    const uint32_t bytes = fdt_totalsize(dt);
    struct e0_visual_smoke_v2 r = success();
    CHECK(e0_visual_smoke_publish(dt, &r, NULL) == 0);
    CHECK(fdt_totalsize(dt) == bytes); /* Finalization needs no spare space. */
    CHECK(r.publication == E0VS_PUB_COMPLETE && !r.publication_error);
    CHECK(r.publication_step == 2 && r.decision == E0VS_CONTINUE);
    struct e0_visual_smoke_v2 receipt = read_record(dt);
    CHECK(memcmp(&receipt, &r, sizeof(r)) == 0);
    CHECK(e0_visual_smoke_decision(&receipt, 1, NULL) == E0VS_CONTINUE);
    CHECK(e0_visual_smoke_decision(&receipt, 0, NULL) == E0VS_BLOCK_P3);

    for (uint32_t outcome = E0VS_REFUSED; outcome <= E0VS_FAILED; outcome++) {
        for (uint32_t frames = 0; frames <= 3; frames++) {
            CHECK(e0_visual_smoke_reserve(dt) == 0);
            r = success();
            r.outcome = outcome;
            r.v1.error = 2;
            r.v1.frames = frames;
            r.failed_frame = frames;
            r.reason = 2;
            r.stage = 1;
            r.detail = 4;
            r.v1.width = 399;
            r.diagnostic_flags = frames ? 0x7c : 12;
            CHECK(e0_visual_smoke_publish(dt, &r, NULL) == 0);
            CHECK(r.decision == E0VS_CONTINUE);
            receipt = read_record(dt);
            CHECK(receipt.outcome == outcome && receipt.reason == r.reason);
            CHECK(receipt.v1.error == 2 && receipt.v1.frames == frames);
            CHECK(receipt.decision == E0VS_CONTINUE);
        }
    }
    puts("PASS: exact pending slot, in-place final receipt, safe REFUSED/FAILED continue");
}

static void test_context_priority(void)
{
    struct e0_visual_smoke_v2 r;
    const uint32_t unsafe_safety[] = {0, 14, 13, 11, 7, 31};
    for (size_t i = 0; i < sizeof(unsafe_safety) / sizeof(unsafe_safety[0]); i++) {
        r = published_success();
        r.safety = unsafe_safety[i];
        CHECK(e0_visual_smoke_decision(&r, 1, NULL) == E0VS_BLOCK_CONTEXT);
        CHECK(e0_visual_smoke_decision(&r, 0, NULL) == E0VS_BLOCK_P3);
    }
    r = published_success();
    r.outcome = E0VS_REFUSED;
    r.v1.error = 2;
    r.v1.frames = 0;
    r.reason = 10;
    r.safety = 0;
    CHECK(e0_visual_smoke_decision(&r, 1, NULL) == E0VS_BLOCK_CONTEXT);
    for (unsigned i = 0; i < 64; i++) {
        r = published_success();
        r.context_changed = UINT64_C(1) << i;
        CHECK(e0_visual_smoke_decision(&r, 1, NULL) == E0VS_BLOCK_CONTEXT);
    }
    r = published_success();
    r.v1.hcr_after ^= UINT64_C(1) << 2;
    CHECK(e0_visual_smoke_decision(&r, 1, NULL) == E0VS_BLOCK_CONTEXT);
    r = published_success();
    r.v1.hcr_before |= 1;
    r.v1.hcr_after |= 1;
    CHECK(e0_visual_smoke_decision(&r, 1, NULL) == E0VS_BLOCK_CONTEXT);
    r = published_success();
    r.restored_par ^= 1;
    CHECK(e0_visual_smoke_decision(&r, 1, NULL) == E0VS_BLOCK_CONTEXT);
    r = published_success();
    r.daif_after ^= UINT64_C(1) << 6;
    CHECK(e0_visual_smoke_decision(&r, 1, NULL) == E0VS_BLOCK_CONTEXT);
    r = published_success();
    r.diagnostic_flags = 128;
    CHECK(e0_visual_smoke_decision(&r, 1, NULL) == E0VS_BLOCK_CONTEXT);
    r = published_success();
    r.v1.error = 3;
    CHECK(e0_visual_smoke_decision(&r, 1, NULL) == E0VS_BLOCK_CONTEXT);
    r.publication = E0VS_PUB_FAILED;
    r.publication_error = -FDT_ERR_NOTFOUND;
    CHECK(e0_visual_smoke_decision(&r, 1, NULL) == E0VS_BLOCK_CONTEXT);
    CHECK(e0_visual_smoke_decision(&r, 0, NULL) == E0VS_BLOCK_P3);
    CHECK(e0_visual_smoke_decision(NULL, 0, NULL) == E0VS_BLOCK_P3);
    CHECK(e0_visual_smoke_decision(NULL, 1, NULL) == E0VS_BLOCK_EVIDENCE);

    _Alignas(8) unsigned char dt[4096];
    new_tree(dt, sizeof(dt), "chosen");
    CHECK(e0_visual_smoke_reserve(dt) == 0);
    r = success();
    r.outcome = E0VS_FAILED;
    r.reason = 15;
    r.stage = 10;
    r.v1.error = 3;
    r.v1.hcr_after ^= 4;
    CHECK(e0_visual_smoke_publish(dt, &r, NULL) == 0);
    CHECK(r.decision == E0VS_BLOCK_CONTEXT);
    struct e0_visual_smoke_v2 receipt = read_record(dt);
    CHECK(receipt.publication == E0VS_PUB_COMPLETE);
    CHECK(receipt.decision == E0VS_BLOCK_CONTEXT && receipt.v1.error == 3);
    puts("PASS: P3 priority, PAR/DAIF/HCR/host guards, error 2 requires restored context");
}

static void test_incomplete_evidence(void)
{
    for (unsigned fault = 0; fault < 5; fault++) {
        struct e0_visual_smoke_v2 r = success();
        unsigned expected = E0VS_BLOCK_EVIDENCE;
        switch (fault) {
        case 0: r.stage = 0; break;
        case 1: r.diagnostic_flags = 0; expected = E0VS_BLOCK_CONTEXT; break;
        case 2: r.diagnostic_flags = 12; break;
        case 3: r.extent = 1; break;
        default: r.detail = 1; break;
        }
        _Alignas(8) unsigned char dt[4096];
        new_tree(dt, sizeof(dt), "chosen");
        CHECK(e0_visual_smoke_reserve(dt) == 0);
        CHECK(e0_visual_smoke_publish(dt, &r, NULL) == 0);
        CHECK(r.decision == expected);
        CHECK(r.outcome == E0VS_COMPLETE && !r.v1.error && !r.reason);
        struct e0_visual_smoke_v2 receipt = read_record(dt);
        CHECK(memcmp(&receipt, &r, sizeof(r)) == 0);
        CHECK(e0_visual_smoke_decision(&receipt, 1, NULL) == expected);
    }
    puts("PASS: incomplete draw/context evidence is retained but cannot authorize handoff");
}

static void test_invalid_result(void)
{
    struct e0_visual_smoke_v2 r;
    for (unsigned fault = 0; fault < 15; fault++) {
        r = published_success();
        switch (fault) {
        case 0: r.v1.magic = 0; break;
        case 1: r.v1.version = 1; break;
        case 2: r.v1.size = 80; break;
        case 3: r.outcome = 0; break;
        case 4: r.outcome = 4; break;
        case 5: r.v1.frames = 2; break;
        case 6: r.v1.error = 2; break;
        case 7: r.reason = 1; break;
        case 8: r.v1.frames = 4; break;
        case 9: r.v1.error = 4; break;
        case 10: r.reason = 16; break;
        case 11: r.stage = 11; break;
        case 12: r.outcome = E0VS_REFUSED; break;
        case 13: r.outcome = E0VS_FAILED; r.v1.error = 2; break;
        default: r.outcome = E0VS_REFUSED; r.reason = 1; break;
        }
        CHECK(e0_visual_smoke_decision(&r, 1, NULL) == E0VS_BLOCK_EVIDENCE);
        CHECK(e0_visual_smoke_decision(&r, 0, NULL) == E0VS_BLOCK_P3);
        _Alignas(8) unsigned char dt[4096];
        new_tree(dt, sizeof(dt), "chosen");
        CHECK(e0_visual_smoke_reserve(dt) == 0);
        CHECK(e0_visual_smoke_publish(dt, &r, NULL) == -FDT_ERR_BADVALUE);
        check_failed(&r, -FDT_ERR_BADVALUE, 2, E0VS_BLOCK_EVIDENCE);
        check_pending(dt);
    }
    r = published_success();
    r.publication = E0VS_PUB_PENDING;
    CHECK(e0_visual_smoke_decision(&r, 1, NULL) == E0VS_BLOCK_EVIDENCE);
    r = published_success();
    r.publication = 3;
    CHECK(e0_visual_smoke_decision(&r, 1, NULL) == E0VS_BLOCK_EVIDENCE);
    r = published_success();
    r.publication_error = -FDT_ERR_NOSPACE;
    CHECK(e0_visual_smoke_decision(&r, 1, NULL) == E0VS_BLOCK_EVIDENCE);
    r = published_success();
    r.publication_step = 0;
    CHECK(e0_visual_smoke_decision(&r, 1, NULL) == E0VS_BLOCK_EVIDENCE);
    puts("PASS: uninitialized, unsupported, and inconsistent results fail closed");
}

static void test_publication_failures(void)
{
    _Alignas(8) unsigned char dt[4096];
    struct e0_visual_smoke_v2 r = success();
    new_tree(dt, sizeof(dt), "chosen");
    CHECK(e0_visual_smoke_publish(dt, &r, NULL) == -FDT_ERR_NOTFOUND);
    check_failed(&r, -FDT_ERR_NOTFOUND, 2, E0VS_BLOCK_EVIDENCE);

    /* Every wrong slot length is refused, including a stale record truncated
     * one byte short and a larger slot which must never be silently accepted. */
    const int sizes[] = {0, 1, 3, 4, 80, 255, 257, 512};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        new_tree(dt, sizeof(dt), "chosen");
        unsigned char old[512] = {0};
        struct e0_visual_smoke_v2 complete = published_success();
        memcpy(old, &complete, sizeof(complete));
        int node = fdt_path_offset(dt, "/chosen");
        CHECK(fdt_setprop(dt, node, PROPERTY, old, sizes[i]) == 0);
        r = success();
        r.outcome = E0VS_REFUSED;
        r.v1.frames = 1;
        r.v1.error = 2;
        r.reason = 10;
        CHECK(e0_visual_smoke_publish(dt, &r, NULL) == -FDT_ERR_NOSPACE);
        check_failed(&r, -FDT_ERR_NOSPACE, 2, E0VS_BLOCK_EVIDENCE);
        CHECK(r.outcome == E0VS_REFUSED && r.v1.error == 2 && r.reason == 10);
        int bytes = -1;
        const unsigned char *slot = fdt_getprop(dt, node, PROPERTY, &bytes);
        CHECK(slot && bytes == sizes[i]);
        for (int b = 0; b < (bytes < 4 ? bytes : 4); b++)
            CHECK(slot[b] == 0);
    }

    new_tree(dt, sizeof(dt), "wrong-node");
    int other = fdt_path_offset(dt, "/wrong-node");
    struct e0_visual_smoke_v2 old = published_success();
    CHECK(fdt_setprop(dt, other, PROPERTY, &old, sizeof(old)) == 0);
    CHECK(e0_visual_smoke_reserve(dt) == -FDT_ERR_NOTFOUND);
    r = success();
    CHECK(e0_visual_smoke_publish(dt, &r, NULL) == -FDT_ERR_NOTFOUND);
    check_failed(&r, -FDT_ERR_NOTFOUND, 2, E0VS_BLOCK_EVIDENCE);
    int bytes = -1;
    const void *unrelated = fdt_getprop(dt, other, PROPERTY, &bytes);
    CHECK(unrelated && bytes == (int)sizeof(old));
    CHECK(memcmp(unrelated, &old, sizeof(old)) == 0);

    /* No space to reserve is a real libfdt NOSPACE error. The caller preserves
     * that first publication stage rather than relabeling it as final-write. */
    new_tree(dt, sizeof(dt), "chosen");
    CHECK(fdt_pack(dt) == 0);
    int ret = e0_visual_smoke_reserve(dt);
    CHECK(ret == -FDT_ERR_NOSPACE);
    r = success();
    r.outcome = E0VS_FAILED;
    r.v1.error = 2;
    r.v1.frames = 2;
    r.reason = 10;
    r.publication = E0VS_PUB_FAILED;
    r.publication_error = ret;
    r.publication_step = 1;
    CHECK(e0_visual_smoke_publish(dt, &r, NULL) == ret);
    check_failed(&r, ret, 1, E0VS_BLOCK_EVIDENCE);
    CHECK(r.outcome == E0VS_FAILED && r.reason == 10 && r.v1.error == 2);

    /* Corrupt or already complete exact-sized slots are not reservations. */
    for (unsigned fault = 0; fault < 4; fault++) {
        new_tree(dt, sizeof(dt), "chosen");
        CHECK(e0_visual_smoke_reserve(dt) == 0);
        old = read_record(dt);
        switch (fault) {
        case 0: old.v1.magic ^= 1; break;
        case 1: old.v1.version = 1; break;
        case 2: old.outcome = E0VS_COMPLETE; break;
        default: old = published_success(); break;
        }
        int node = fdt_path_offset(dt, "/chosen");
        CHECK(fdt_setprop_inplace(dt, node, PROPERTY, &old, sizeof(old)) == 0);
        r = success();
        CHECK(e0_visual_smoke_publish(dt, &r, NULL) == -FDT_ERR_BADSTATE);
        check_failed(&r, -FDT_ERR_BADSTATE, 2, E0VS_BLOCK_EVIDENCE);
        CHECK(r.outcome == E0VS_COMPLETE && !r.reason && !r.v1.error);
        check_pending(dt);
    }

    /* A context failure keeps its original reason/error and takes priority
     * over the independent missing-receipt failure. */
    new_tree(dt, sizeof(dt), "chosen");
    r = success();
    r.outcome = E0VS_FAILED;
    r.v1.error = 3;
    r.reason = 15;
    r.safety = E0VS_SAFE & ~E0VS_PAR_OK;
    CHECK(e0_visual_smoke_publish(dt, &r, NULL) == -FDT_ERR_NOTFOUND);
    check_failed(&r, -FDT_ERR_NOTFOUND, 2, E0VS_BLOCK_CONTEXT);
    CHECK(r.outcome == E0VS_FAILED && r.v1.error == 3 && r.reason == 15);

    new_tree(dt, sizeof(dt), "chosen");
    CHECK(e0_visual_smoke_reserve(dt) == 0);
    r = success();
    CHECK(e0_visual_smoke_publish(dt, &r, NULL) == 0);
    CHECK(e0_visual_smoke_publish(dt, &r, NULL) == -FDT_ERR_BADSTATE);
    check_failed(&r, -FDT_ERR_BADSTATE, 2, E0VS_BLOCK_EVIDENCE);
    check_pending(dt); /* No stale safe COMPLETE receipt after any failure. */
    CHECK(e0_visual_smoke_publish(dt, NULL, NULL) == -FDT_ERR_BADVALUE);
    check_pending(dt);

    r = success();
    CHECK(e0_visual_smoke_reserve(NULL) == -FDT_ERR_BADMAGIC);
    CHECK(e0_visual_smoke_publish(NULL, &r, NULL) == -FDT_ERR_BADMAGIC);
    check_failed(&r, -FDT_ERR_BADMAGIC, 2, E0VS_BLOCK_EVIDENCE);
    new_tree(dt, sizeof(dt), "chosen");
    fdt_set_magic(dt, 0);
    CHECK(e0_visual_smoke_reserve(dt) == -FDT_ERR_BADMAGIC);
    r = success();
    CHECK(e0_visual_smoke_publish(dt, &r, NULL) == -FDT_ERR_BADMAGIC);
    check_failed(&r, -FDT_ERR_BADMAGIC, 2, E0VS_BLOCK_EVIDENCE);
    puts("PASS: missing/short/wrong-node/full FDT failures preserve first error and invalidate stale receipts");
}

static void test_translation_publication(void)
{
    /* One complete owned-table receipt; physical visibility is not encoded. */
    struct e0_visual_smoke_v2 initial = success();
    initial.diagnostic_flags |= E0VS_TRANSLATION_REQUIRED;
    initial.ttbr0 = UINT64_C(0x800000000);
    initial.mair = UINT64_C(0x44ff);
    const uint64_t first = (initial.v1.framebuffer + 90 * initial.v1.stride) & ~UINT64_C(0x3fff);
    const uint64_t end = initial.v1.framebuffer + 270 * initial.v1.stride;
    const unsigned count = (unsigned)((end - first + 16383) / 16384);
    struct fb_e0_translation proof = {
        .magic = FB_E0_XLAT_MAGIC, .version = 1, .size = sizeof(proof), .build = initial.v1.build,
        .va = first, .par = (UINT64_C(0x44) << 56) | first | 0xb00, .root = initial.ttbr0,
        .descriptors = {UINT64_C(0x800004003), UINT64_C(0x800008003),
                        UINT64_C(0x80000c003), first | 0x607}, .pa = first,
        .isar1 = UINT64_C(1) << 56, .mair_alias = initial.mair,
        .last_va = first + (count - 1) * UINT64_C(16384), .resolved_mair = 0x44,
        .samples = count, .page_shift = 14, .level = 3, .status = FB_E0_WALK_OK,
        .flags = FB_E0_XLAT_VALID | FB_E0_XLAT_MMFR3
    };
    for (unsigned fault = 0; fault < 4; fault++) {
        _Alignas(8) unsigned char dt[4096];
        new_tree(dt, sizeof(dt), "chosen");
        CHECK(e0_visual_smoke_reserve(dt) == 0);
        struct e0_visual_smoke_v2 r = initial;
        struct fb_e0_translation t = proof;
        int node = fdt_path_offset(dt, "/chosen");
        int expected = 0;
        if (fault == 1) {
            CHECK(fdt_delprop(dt, node, "6boot,e0-translation-result") == 0);
            expected = -FDT_ERR_NOTFOUND;
        } else if (fault == 2) {
            CHECK(fdt_setprop(dt, node, "6boot,e0-translation-result", "bad", 4) == 0);
            expected = -FDT_ERR_NOSPACE;
        } else if (fault == 3) {
            CHECK(fdt_setprop_inplace(dt, node, "6boot,e0-translation-result", &proof, sizeof(proof)) == 0);
            expected = -FDT_ERR_BADSTATE;
        }
        CHECK(fdt_pack(dt) == 0);
        uint32_t bytes = fdt_totalsize(dt);
        CHECK(e0_visual_smoke_publish(dt, &r, &t) == expected);
        CHECK(fdt_totalsize(dt) == bytes);
        CHECK(r.outcome == initial.outcome && !r.reason && !r.v1.error);
        if (!fault) {
            CHECK(e0_visual_smoke_decision(&r, 1, &t) == E0VS_CONTINUE);
            CHECK(e0_visual_smoke_decision(&r, 1, NULL) == E0VS_BLOCK_EVIDENCE);
            CHECK(t.publication == E0VS_PUB_COMPLETE && !t.publication_error);
            int size = 0;
            const void *receipt = fdt_getprop(dt, node, "6boot,e0-translation-result", &size);
            CHECK(receipt && size == (int)sizeof(t) && !memcmp(receipt, &t, sizeof(t)));
        } else {
            CHECK(r.publication_error == expected && t.publication_error == expected);
            CHECK(r.publication == E0VS_PUB_FAILED && t.publication == E0VS_PUB_FAILED);
            CHECK(e0_visual_smoke_decision(&r, 1, &t) == E0VS_BLOCK_EVIDENCE);
            check_pending(dt); /* No stale COMPLETE primary receipt survives. */
        }
    }
    /* Primary reservation can succeed while adding the second slot fails.
     * Both that partial state and an older final receipt must be invalidated. */
    _Alignas(8) unsigned char dt[4096];
    new_tree(dt, sizeof(dt), "chosen");
    int node = fdt_path_offset(dt, "/chosen");
    CHECK(fdt_setprop(dt, node, PROPERTY, &initial, sizeof(initial)) == 0);
    CHECK(fdt_pack(dt) == 0);
    CHECK(e0_visual_smoke_reserve(dt) == -FDT_ERR_NOSPACE);
    check_pending(dt);
    int size = 0;
    CHECK(!fdt_getprop(dt, node, "6boot,e0-translation-result", &size) && size == -FDT_ERR_NOTFOUND);
    puts("PASS: paired receipts, partial reservation, missing/short/stale companion, no final-copy allocation");
}

static void test_boot_identity(void)
{
    const uint64_t build = UINT64_C(0x123456789abcdef0);
    static const char good[] = "root=fixture x6e3.fbsmoke=123456789abcdef0 quiet";
    CHECK(e0_visual_smoke_bootargs(good, sizeof(good), build));
    CHECK(!e0_visual_smoke_bootargs(good, sizeof(good) - 1, build));
    CHECK(!e0_visual_smoke_bootargs(good, sizeof(good), build + 1));
    CHECK(!e0_visual_smoke_bootargs(NULL, 0, build));
    static const char *bad[] = {"", "root=fixture", "x6e3.fbsmoke=123456789abcdef",
        "x6e3.fbsmoke=123456789abcdef00", "x6e3.fbsmoke=123456789abcdef0x",
        "x6e3.fbsmoke=123456789abcdef0 x6e3.fbsmoke=123456789abcdef0"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(!e0_visual_smoke_bootargs(bad[i], (int)strlen(bad[i]) + 1, build));
    static const char hidden[] = "x6e3.fbsmoke=123456789abcdef0\0extra";
    CHECK(!e0_visual_smoke_bootargs(hidden, sizeof(hidden), build));
    puts("PASS: pre-render boot token rejects old, missing, duplicate and truncated identity");
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--decision")) {
        struct e0_visual_smoke_v2 r;
        struct fb_e0_translation t;
        if (fread(&r, 1, sizeof(r), stdin) != sizeof(r))
            return 2;
        size_t count = fread(&t, 1, sizeof(t), stdin);
        if ((count && count != sizeof(t)) || fgetc(stdin) != EOF) return 2;
        printf("%u\n", e0_visual_smoke_decision(&r, 1, count ? &t : NULL));
        return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "--attribute")) {
        printf("%u\n", fb_e0_attribute(strtoull(argv[2], NULL, 0),
                                      (unsigned)strtoul(argv[3], NULL, 0),
                                      strtoull(argv[4], NULL, 0)));
        return 0;
    }
    if (argc != 1)
        return 2;
    CHECK(sizeof(struct e0_visual_smoke_v2) == 256);
    test_success_and_safe_refusals();
    test_context_priority();
    test_incomplete_evidence();
    test_invalid_result();
    test_publication_failures();
    test_translation_publication();
    test_boot_identity();
    printf("E0VS_RECORD_PASS: %u checks; actual libfdt; no hardware execution\n", checks);
    return 0;
}
