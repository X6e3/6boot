/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "e0.h"
#include <assert.h>
#include <stdio.h>
static struct e0_tables tables;
int main(void)
{
    const uint64_t id = UINT64_C(2) << 32 | 5;
    const uint64_t base = UINT64_C(0x800000000);
    struct e0_config config;
    assert(!e0_s2_build(&tables,id,base,false,&config));
    assert(config.vtcr == UINT64_C(0x8005b560));
    assert(config.vttbr == ((uint64_t)E0_VMID << 48 | base + E0_L2_PA));
    for (unsigned i = 0; i < 2048; i++) {
        assert(tables.l2[i] == (i == 8 ? (base+E0_L3_PA)|3 : 0));
        if (i > 2) assert(!tables.l3[i]);
    }
    assert((tables.l3[0] & 0xfc0) == 0x740); /* RO, inner-shareable, AF */
    assert(!(tables.l3[0] & (UINT64_C(1) << 54)));
    assert((tables.l3[1] & (UINT64_C(1) << 54)) && (tables.l3[2] & (UINT64_C(1) << 54)));
    assert(!e0_s2_build(&tables,id,base,true,&config) && (tables.l3[3]&3)==3);
    assert(e0_s2_build(&tables,id,base+1,false,&config)==E0_ENVIRONMENT);
    assert(e0_s2_build(&tables,7,base,false,&config)==E0_ENVIRONMENT);
    assert(e0_s2_build(&tables,UINT64_C(1)<<32|5,base,false,&config)==E0_ENVIRONMENT);
    assert(e0_s2_build(&tables,0,base,false,&config)==E0_ENVIRONMENT);
    assert(!e0_s2_build(&tables,UINT64_C(1)<<20|5,base,false,&config));
    assert(!e0_s2_build(&tables,UINT64_C(3)<<32|6,base,false,&config));
    assert(e0_s2_build(&tables,id,UINT64_C(1)<<48,false,&config)==E0_ENVIRONMENT);
    const uint64_t esr = UINT64_C(0x24)<<26 | UINT64_C(1)<<25 | 0x47;
    const uint64_t hp = (uint64_t)E0_DENIED_IPA >> 8;
    assert(e0_fault_valid(esr,E0_DENIED_IPA,hp,0x10000900,0x10000900));
    const unsigned flip[] = {26,25,10,9,8,7,6,0};
    for (unsigned i = 0; i < sizeof(flip)/sizeof(flip[0]); i++)
        assert(!e0_fault_valid(esr^(UINT64_C(1)<<flip[i]),E0_DENIED_IPA,hp,1,1));
    assert(!e0_fault_valid(esr,E0_DENIED_IPA+8,hp,1,1));
    assert(!e0_fault_valid(esr,E0_DENIED_IPA,hp+16,1,1));
    assert(!e0_fault_valid(esr,E0_DENIED_IPA,hp,1,5));
    const uint64_t state_ipa=UINT64_C(0x408087b8), hp2=(state_ipa & ~UINT64_C(0xfff)) >> 8;
    assert(e0_state_fault_valid(esr^1,state_ipa,hp2,1,1,state_ipa));
    assert(!e0_state_fault_valid(esr,state_ipa,hp2,1,1,state_ipa));
    assert(!e0_state_fault_valid(esr^1,state_ipa+8,hp2,1,1,state_ipa));
    assert(!e0_state_fault_valid(esr^1,state_ipa,hp2+16,1,1,state_ipa));
    assert(!e0_state_fault_valid(esr^1,state_ipa,hp2,1,5,state_ipa));
    /* Architectural FIPA is IPA[47:12], even for a 16 KiB granule.
     * These are decoder inputs, not hardware claims for unaligned STRs. */
    const unsigned offsets[] = {0,0xfff,0x1000,0x2000,0x3fff};
    for (unsigned i = 0; i < sizeof(offsets)/sizeof(offsets[0]); i++) {
        uint64_t address = UINT64_C(0x40808000) + offsets[i];
        uint64_t fipa = (address >> 12) << 4;
        assert(e0_state_fault_valid(esr^1,address,fipa,1,1,address));
        assert(!e0_state_fault_valid(esr^1,address,fipa^0x10,1,1,address));
        assert(!e0_state_fault_valid(esr^1,address,fipa^0x20,1,1,address));
        assert(!e0_state_fault_valid(esr^1,address,fipa,1,5,address));
        assert(!e0_state_fault_valid(esr^1,address,fipa,1,1,address+8));
        assert(!e0_state_fault_valid((esr^1)|(1u<<10),address,fipa,1,1,address));
        assert(!e0_state_fault_valid((esr^1)|(1u<<7),address,fipa,1,1,address));
        assert(!e0_state_fault_valid((esr^1)^0x08,address,fipa,1,1,address)); /* permission */
        for (unsigned level = 0; level < 4; level++)
            assert(!e0_state_fault_valid((esr&~UINT64_C(63))|level,address,fipa,1,1,address));
    }
    /* A low IPA maps to a high PA without changing the 32-bit IPA geometry. */
    assert(!e0_s2_build(&tables,id,base,false,&config));
    assert((config.vtcr & 63) == 32);
    assert((tables.l3[0]&UINT64_C(0x0000ffffffffc000)) == base+E0_CODE_PA);
    assert((tables.l3[2]&UINT64_C(0x0000ffffffffc000)) == base+E0_ALLOWED_PA);
    assert(e0_s2_build(&tables,UINT64_C(2)<<32,base,false,&config)==E0_ENVIRONMENT);
    assert(!e0_s2_build(&tables,UINT64_C(2)<<32|1,base,false,&config));
    puts("E0_HOST_PASS: tables, granules/PA bounds, fault class/address/PC negatives");
}
