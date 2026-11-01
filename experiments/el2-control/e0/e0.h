/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SIXBOOT_E0_H
#define SIXBOOT_E0_H
#include "el2_exception.h"
#define E0_ARENA_SIZE 0x40000
#define E0_CODE_PA 0x04000
#define E0_STACK_PA 0x18000
#define E0_ALLOWED_PA 0x1c000
#define E0_GUARD_PA 0x20000
#define E0_L2_PA 0x10000
#define E0_L3_PA 0x14000
#define E0_CODE_IPA 0x10000000
#define E0_STACK_IPA 0x10004000
#define E0_ALLOWED_IPA 0x10008000
#define E0_DENIED_IPA 0x1000c000
#define E0_VALUE 0x1234
#define E0_GUARD_VALUE 0x1357
#define E0_STORE_VALUE 0xbad
#define E0_VMID 0x7f
#define E0_S_MODE 0
#define E0_S_RETURNED 16
#define E0_S_CONFIG 64
#define E0_OK 0
#define E0_ENVIRONMENT 1
#define E0_UNEXPECTED 2
#define E0_RESTORE_FAILED 3
#define E0_CONTEXT_LOST 4
#define E0_DENY 0
#define E0_ALLOW 1
#define E0_WRONG_ADDRESS 2
#define E0_MISSING_DENIAL 3
#define E0_STATE_DENY 4
#define E0_CANARY UINT64_C(0x6e376e376e376e37)
/* Private saved register block; caller frame is separate. */
#define EO_CALL_SP 0
#define EO_VBAR 8
#define EO_HCR 16
#define EO_VTCR 24
#define EO_VTTBR 32
#define EO_HPFAR 40
#define EO_ELR 48
#define EO_SPSR 56
#define EO_ESR 64
#define EO_FAR 72
#define EO_DAIF 80
#define EO_SP1 88
#define EO_SCTLR12 96
#define EO_VBAR12 104
#define EO_ELR12 112
#define EO_SPSR12 120
#define EO_ESR12 128
#define EO_FAR12 136
#define EO_PAR 144
#define EO_NZCV 152
#define EO_SIZE 160
#ifndef __ASSEMBLER__
#include <stdbool.h>
struct e0_tables { uint64_t l2[2048], l3[2048]; };
struct e0_config { uint64_t vtcr, vttbr; };
struct e0_state {
    uint64_t mode, phase, returned, hpfar_valid;
    uint64_t hpfar[4];
    struct e0_config config;
    uint64_t mmfr0;
};
extern struct e0_state e0_state;
extern uint64_t e0_state_canary;
extern uint64_t e0_status, e0_restoring, e0_saved[EO_SIZE / 8];
extern struct e2_exception_session e2_exception_session;
extern struct e2_exception_trace e2_exception_trace;
int e0_s2_build(struct e0_tables *t, uint64_t mmfr0, uint64_t base,
                bool map_guard, struct e0_config *out);
/* The fixed pool has one L3 table: only the 32 MiB CODE_IPA window is
 * supported. Tables stay immutable from publication until final TLBI. */
#define E0_READ 1u
#define E0_WRITE 2u
#define E0_EXEC 4u
int e0_s2_config(uint64_t mmfr0, uint64_t base, unsigned root_offset,
                 unsigned vmid, struct e0_config *out);
int e0_s2_page(struct e0_tables *t, uint64_t ipa, uint64_t pa,
               unsigned permissions, unsigned pa_bits);
int e0_fault_valid(uint64_t esr, uint64_t far, uint64_t hpfar,
                    uint64_t pc, uint64_t expected_pc);
int e0_state_fault_valid(uint64_t esr, uint64_t far, uint64_t hp,
                         uint64_t pc, uint64_t expected_pc, uint64_t state_ipa);
int e0_prepare(unsigned mode);
extern uint8_t e0_arena_start[], e0_guest_vectors[], __e2_stack_end[];
/* Only OK/ENVIRONMENT/UNEXPECTED return. Restore failures record status
 * and park at EL2; they never return a partially restored context. */
int sixboot_e0_run(unsigned mode);
#endif
#endif
