/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "native.h"
#include "types.h"
#include "utils.h"
#include "memory.h"

/* Fixed to the inherited m1n1 16 KiB, 48-bit VA, four-level host tables.
 * Refuse blocks, contiguous hints and unfamiliar table control bits. No
 * allocation, structural table replacement or inherited mmu_map() calls.
 * Caller has independently proven exclusive ownership of all eight pages. */
int en_protect_arena(uint64_t arena)
{
    const uint64_t mask=UINT64_C(0x0003ffffffffc000);
    uint64_t tcr=mrs(TCR_EL2),ttbr=mrs(TTBR0_EL2);
    if((ttbr&UINT64_C(0xffff000000003fff)) || !(ttbr&mask) ||
       FIELD_GET(TCR_T0SZ,tcr)!=TCR_T0SZ_48BIT || FIELD_GET(TCR_TG0,tcr)!=TCR_TG0_16K ||
       FIELD_GET(TCR_IPS,tcr)>5 || (tcr&(TCR_DS|TCR_HA|TCR_HD|TCR_EPD0|TCR_TBI0)) ||
       arena%E2_ARENA_ALIGN || arena>UINT64_C(0xffffffffffff)-E2_ARENA_SIZE) return -1;
    if((ttbr&mask)<arena+E2_ARENA_SIZE && arena<(ttbr&mask)+E2_PAGE_SIZE) return -1;
    volatile uint64_t *slot[8]; uint64_t replacement[8];
    for(unsigned i=0;i<8;i++) {
        uint64_t address=arena+E2_PAGE_SIZE*i;
        volatile uint64_t *table=(void *)(uintptr_t)(ttbr&mask);
        const unsigned shifts[]={47,36,25};
        for(unsigned level=0;level<3;level++) {
            unsigned index=(unsigned)((address>>shifts[level])&(level?2047u:1u));
            uint64_t d=table[index];
            if((d&3)!=3 || (d&~(mask|3))) return -1;
            uint64_t base=d&mask;
            if(base<arena+E2_ARENA_SIZE && arena<base+E2_PAGE_SIZE) return -1;
            table=(void *)(uintptr_t)base;
        }
        slot[i]=table+((address>>14)&2047);
        if(!en_page_descriptor(*slot[i],address,i==0,&replacement[i])) return -1;
    }
    uint64_t daif=mrs(DAIF);
    __asm__ volatile("msr daifset, #0xf":::"memory");
    for(unsigned i=0;i<8;i++) *slot[i]=0;
    __asm__ volatile("dsb ish; tlbi alle2is; dsb ish; isb":::"memory");
    for(unsigned i=0;i<8;i++) *slot[i]=replacement[i];
    __asm__ volatile("dsb ish; isb":::"memory");
    msr(DAIF,daif);
    return 0;
}
