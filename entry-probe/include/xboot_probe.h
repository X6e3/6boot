/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XBOOT_PROBE_H
#define XBOOT_PROBE_H
#include <stddef.h>
#include <stdint.h>

/* Private callable probe ABI; this is not Apple's boot_args protocol. */
#define XP_CONTEXT_MAGIC UINT32_C(0x58504354)
#define XP_RESULT_MAGIC UINT32_C(0x58505242)
#define XP_VERSION 1u
#define XP_HEADER_SIZE 64u
#define XP_RESULT_SIZE 64u
#define XP_MAX_RAM 16u
#define XP_CHIP UINT32_C(0x8132)

enum xp_status {
    XP_OK = 0, XP_ARGUMENT = 1, XP_LENGTH = 2, XP_ABI = 3,
    XP_PLATFORM = 4, XP_EL = 5, XP_RANGE = 6, XP_OVERLAP = 7,
    XP_COVERAGE = 8
};

/* Context and output must be accessible for the stated lengths. Address fields
 * are declarations only and are never dereferenced. Output must not alias input.
 * ARGUMENT errors do not write output. Other results have a 64-byte LE record.
 * This function performs no MMIO, firmware, allocation or system-register access.
 */
int xp_probe(const void *context, size_t context_size,
             void *result, size_t result_capacity,
             uint64_t current_el, uint64_t mpidr);

/* EL1/EL2 AAPCS64 callable probe, not a drop-in iBoot/m1n1 image. Requires an
 * accessible caller stack, our mapped 16 KiB stack and legal MPIDR_EL1 reads.
 * Non-reentrant: single boot CPU, interrupts already masked by the caller.
 * Own stack and callee/platform registers restored; status returned in x0.
 */
int xboot_probe_entry(const void *context, size_t context_size,
                      void *result, size_t result_capacity);
#endif
