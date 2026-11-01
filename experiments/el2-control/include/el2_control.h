/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SIXBOOT_EL2_CONTROL_H
#define SIXBOOT_EL2_CONTROL_H

/* Constants used by both C and the standalone assembly entry. */
#define E2_READY 0
#define E2_ARGUMENT 1
#define E2_LENGTH 2
#define E2_ABI 3
#define E2_ENVIRONMENT 4
#define E2_PLATFORM 5
#define E2_MODE 6
#define E2_RANGE 7
#define E2_OVERLAP 8
#define E2_COVERAGE 9
#define E2_OWNERSHIP 10
#define E2_LOAD_ADDRESS 11
#define E2_CAPACITY 12

#define E2_PAGE_SIZE 0x4000
#define E2_ARENA_ALIGN 0x10000
#define E2_ARENA_SIZE 0x20000
#define E2_CODE_OFFSET 0x00000
#define E2_STATE_OFFSET 0x04000
#define E2_STACK_OFFSET 0x08000
#define E2_GUEST_OFFSET 0x0c000
#define E2_GUEST_STACK_OFFSET 0x10000
#define E2_TRACE_OFFSET 0x14000
#define E2_RESERVED_OFFSET 0x18000

#define E2_SNAP_EL 0
#define E2_SNAP_SPSEL 8
#define E2_SNAP_ACTIVE_SP 16
#define E2_SNAP_SP_EL0 24
#define E2_SNAP_VBAR 32
#define E2_SNAP_HCR 40
#define E2_SNAP_DAIF 48
#define E2_SNAP_MPIDR 56
#define E2_SNAP_MMFR0 64
#define E2_SNAP_MMFR1 72
#define E2_SNAP_MMFR2 80
#define E2_SNAP_PFR0 88
#define E2_SNAPSHOT_SIZE 96
#define E2_DAIF_MASK 0x3c0
#define E2_CALL_FRAME_SIZE 112
#define E2_RESULT_VERSION 2
#define E2_RESULT_SIZE 160

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>

#define E2_REQUEST_MAGIC UINT32_C(0x45324354) /* E2CT */
#define E2_RESULT_MAGIC UINT32_C(0x45325253) /* E2RS */
#define E2_VERSION 1u
#define E2_HEADER_SIZE 80u
#define E2_MAX_RANGES 16u
#define E2_REQUEST_MAX (E2_HEADER_SIZE + 32u * E2_MAX_RANGES)
#define E2_CHIP UINT32_C(0x8132)
#define E2_PREFLIGHT 1u
#define E2_PRIVATE_ARENA 1u
#define E2_EXCLUSIONS_COMPLETE 2u
#define E2_REQUIRED_FLAGS (E2_PRIVATE_ARENA | E2_EXCLUSIONS_COMPLETE)
#define E2_RESULT_PREFLIGHT_ONLY 1u
#define E2_RESULT_LAYOUT_VALID 2u
#define E2_HCR_E2H (UINT64_C(1) << 34)
#define E2_HCR_RW (UINT64_C(1) << 31)
#define E2_HCR_TGE (UINT64_C(1) << 27)
#define E2_HCR_VM UINT64_C(1)

/* Native, 8-byte-aligned scratch state. The request/result use explicit LE
 * bytes instead; do not serialize this structure by copying native padding. */
struct e2_snapshot {
    uint64_t current_el, spsel, active_sp, sp_el0;
    uint64_t vbar_el2, hcr_el2, daif, mpidr_el1;
    uint64_t mmfr0_el1, mmfr1_el1, mmfr2_el1, pfr0_el1;
};
#define E2_OFFSET(field, offset) \
    _Static_assert(offsetof(struct e2_snapshot, field) == offset, "snapshot ABI: " #field)
E2_OFFSET(current_el, E2_SNAP_EL);
E2_OFFSET(spsel, E2_SNAP_SPSEL);
E2_OFFSET(active_sp, E2_SNAP_ACTIVE_SP);
E2_OFFSET(sp_el0, E2_SNAP_SP_EL0);
E2_OFFSET(vbar_el2, E2_SNAP_VBAR);
E2_OFFSET(hcr_el2, E2_SNAP_HCR);
E2_OFFSET(daif, E2_SNAP_DAIF);
E2_OFFSET(mpidr_el1, E2_SNAP_MPIDR);
E2_OFFSET(mmfr0_el1, E2_SNAP_MMFR0);
E2_OFFSET(mmfr1_el1, E2_SNAP_MMFR1);
E2_OFFSET(mmfr2_el1, E2_SNAP_MMFR2);
E2_OFFSET(pfr0_el1, E2_SNAP_PFR0);
_Static_assert(sizeof(struct e2_snapshot) == E2_SNAPSHOT_SIZE, "snapshot size");
#undef E2_OFFSET

struct e2_range { uint64_t base, size; };
enum e2_slot {
    E2_CODE, E2_STATE, E2_STACK, E2_GUEST, E2_GUEST_STACK, E2_TRACE,
    E2_RESERVED, E2_SLOT_COUNT
};
struct e2_layout { struct e2_range region[E2_SLOT_COUNT]; };

/* All addresses are in one identity-mapped RAM coordinate system. This checks
 * declarations only: no allocation, mapping, permissions or physical probing.
 * An invalid layout leaves *out untouched. */
int e2_layout_make(uint64_t arena, uint64_t size, struct e2_layout *out);

/* Host-testable preflight. Request/result byte buffers may be unaligned.
 * Snapshot is native/aligned. All supplied spans must already be accessible.
 * ARGUMENT returns without writing result, including request/result aliasing
 * the actual image arena (regardless of a forged declared arena).
 * Other returns encode E2_RESULT_SIZE LE bytes.
 * Neither declared ranges nor addresses contained in them are dereferenced.
 * image_base comes from the actual entry, not from the untrusted request.
 * READY denotes validated preflight only; no EL1/exception experiment runs.
 */
int e2_foundation_run(const void *request, size_t request_size,
                      void *result, size_t result_capacity,
                      const struct e2_snapshot *snapshot, uint64_t image_base);

/* Shared argument checks for preflight and the larger exception result.
 * Does not write output or dereference declared physical ranges. */
int e2_buffers_check(const void *request, size_t request_size, void *result,
                      size_t capacity, size_t required,
                      const struct e2_snapshot *snapshot, uint64_t image_base);

/* Separate bare-metal entry; never linked into the default boot path. The
 * loader owns mappings/coherency and checks complete arena ownership first.
 * Single boot CPU, non-recursive; EL2h, fully masked DAIF, VHE host mode.
 * Early assembly ENVIRONMENT refusal does not write a result. */
int sixboot_el2_entry(const void *request, size_t request_size,
                      void *result, size_t result_capacity);
#endif
#endif
