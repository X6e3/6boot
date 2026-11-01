/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SIXBOOT_EL2_NATIVE_H
#define SIXBOOT_EL2_NATIVE_H
#include "../include/el2_exception.h"
#define EN_CONTEXT 1
#define EN_GPRS 2
#define EN_SP 4
#define EN_RETURNED 8
#define EN_DAIF 16
#define EN_ALL 31
#define EN_SYSTEM_COUNT 26
#define EN_RECORD_VERSION 2
#define EN_PAN_OFFSET (15 * 8)
#define EN_BEFORE 0
#define EN_AFTER 208
#define EN_GPR 416
#define EN_OBS_SP 512
#define EN_EXPECT_SP 520
#define EN_RET 528
#define EN_ORIG_SP 536
#define EN_FLAGS 544
#define EN_ORIG_DAIF 552
#define EN_CALL_BYTES 560
#define EN_RECORD_BYTES 624
#define EN_PREFLIGHT 1
#define EN_BRK 2
#define EN_GUEST 3
#define EN_PASS 0
#define EN_FAILED 1
#define EN_REFUSED 2
#define EN_FATAL 3
#ifndef __ASSEMBLER__
struct en_call {
    uint64_t before[EN_SYSTEM_COUNT], after[EN_SYSTEM_COUNT], gpr[12];
    uint64_t observed_sp, expected_sp, ret, original_sp, flags, original_daif;
};
struct en_span { uint64_t start, size; };
_Static_assert(sizeof(struct en_call)==EN_CALL_BYTES,"native call size");
_Static_assert(offsetof(struct en_call,after)==EN_AFTER,"native after offset");
_Static_assert(offsetof(struct en_call,gpr)==EN_GPR,"native GPR offset");
_Static_assert(offsetof(struct en_call,flags)==EN_FLAGS,"native flags offset");
_Static_assert(offsetof(struct en_call,original_daif)==EN_ORIG_DAIF,"native DAIF offset");
int en_valid(struct en_span r);
int en_contains(struct en_span outer,struct en_span inner);
int en_overlaps(struct en_span a,struct en_span b);
int en_page_descriptor(uint64_t old,uint64_t address,int code,uint64_t *replacement);
int en_protect_arena(uint64_t arena);
int en_owned(struct en_span arena,struct en_span storage,struct en_span bss,struct en_span loader,
             const struct en_span *ram,unsigned count);
int en_result(unsigned mode,const struct en_call *call,const uint8_t *result,size_t size);
int en_fdt_memory(const void *dt,size_t capacity,struct en_span arena,struct en_span loader,
                   struct en_span ctrr,struct en_span kernel,struct en_span *ram,unsigned *count);
void en_record(uint8_t out[EN_RECORD_BYTES],unsigned mode,unsigned status,unsigned called,
               const struct en_call *call);
uint64_t en_get(const uint8_t *p,unsigned bytes);
void en_put(uint8_t *p,uint64_t value,unsigned bytes);
extern struct en_call el2_native_call_state;
int el2_native_invoke(uint64_t entry,const void *request,size_t size,void *result,size_t capacity,
                      void *stack_top);
#endif
#endif
