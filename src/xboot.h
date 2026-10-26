/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef XBOOT_H
#define XBOOT_H
#include <stddef.h>
#include <stdint.h>

#define XB_MAX_RANGES 64
#define XB_MAX_CPUS 24
#define XB_TRACE_LEN 12

/* Explicit values are retained by the versioned diagnostic wire ABI. */
enum xb_phase { XB_RESET = 0, XB_PREFLIGHT = 1, XB_DT_PREPARED = 2, XB_CHECKED = 3,
                XB_ARMED = 4, XB_CONSUMED = 5, XB_FAILED = 6 };
enum xb_error { XB_OK = 0, XB_E_PROFILE = 1, XB_E_PHASE = 2, XB_E_CAPACITY = 3,
                XB_E_RAM = 4, XB_E_IMAGE = 5, XB_E_DT = 6, XB_E_RESERVED = 7,
                XB_E_OVERLAP = 8, XB_E_CPU = 9, XB_E_OWNER = 10,
                XB_E_ADAPTER = 11, XB_E_RECORD = 12 };
enum xb_owner { XB_OWNER_UNKNOWN = 0, XB_OWNER_INHERITED_UNVERIFIED = 1,
                XB_OWNER_LINUX = 2, XB_OWNER_BOOT = 3 };

/* Half-open [start, start + size); arithmetic is checked before use. */
struct xb_range { uint64_t start, size; };
struct xb_cpu {
    uint64_t mpidr, release;
    struct xb_range stack;
    unsigned boot, alive, release_matches;
};
struct xb_profile {
    uint32_t chip;
    unsigned cpus;
    uint64_t image_align, dt_align;
    enum xb_owner mtp_dapf_owner, dart_owner;
};
struct xb_facts {
    uint32_t chip;
    unsigned compatible, fdt_valid, image_magic_valid;
    struct xb_range ram[XB_MAX_RANGES], reserved[XB_MAX_RANGES];
    size_t ram_count, reserved_count;
    struct xb_range firmware_reserved[XB_MAX_RANGES];
    size_t firmware_reserved_count;
    struct xb_range image, dt, loader, initrd, protected_memory;
    struct xb_cpu cpu[XB_MAX_CPUS];
    size_t cpu_count;
    enum xb_owner mtp_dapf_owner, dart_owner;
};
struct xb_event { enum xb_phase phase; enum xb_error error; };
struct xb_session {
    enum xb_phase phase;
    enum xb_error error;
    struct xb_event trace[XB_TRACE_LEN];
    size_t trace_count;
};

extern const struct xb_profile xb_j713;
int xb_range_valid(struct xb_range r);
int xb_contains(struct xb_range outer, struct xb_range inner);
int xb_overlaps(struct xb_range a, struct xb_range b);
void xb_reset(struct xb_session *s);
enum xb_error xb_fail(struct xb_session *s, enum xb_error error);
enum xb_error xb_begin(struct xb_session *s, const struct xb_profile *p,
                       uint32_t chip, unsigned compatible, unsigned fdt_valid);
enum xb_error xb_dt_prepared(struct xb_session *s);
enum xb_error xb_validate(const struct xb_profile *p, const struct xb_facts *f);
enum xb_error xb_check(struct xb_session *s, const struct xb_profile *p,
                       const struct xb_facts *f);
enum xb_error xb_arm(struct xb_session *s);
enum xb_error xb_consume(struct xb_session *s);
#endif
