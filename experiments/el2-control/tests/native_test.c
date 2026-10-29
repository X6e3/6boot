/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../native/native.h"
#include "libfdt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static const struct en_span loader={UINT64_C(0x100200000),0x80000};
static const struct en_span bss={UINT64_C(0x100210000),0x60000};
static const struct en_span storage={UINT64_C(0x100220000),0x30000};
static const struct en_span arena={UINT64_C(0x100220000),E2_ARENA_SIZE};
static const struct en_span kernel={UINT64_C(0x100400000),0x800000};
static const struct en_span ram0={UINT64_C(0x100000000),0x10000000};
static unsigned char tree[8192] __attribute__((aligned(8)));
static void ranges(void)
{
    CHECK(en_owned(arena,storage,bss,loader,&ram0,1));
    struct en_span bad=arena;bad.start++;
    CHECK(!en_owned(bad,storage,bss,loader,&ram0,1));
    bad=storage;bad.size=1;CHECK(!en_owned(arena,bad,bss,loader,&ram0,1));
    bad=bss;bad.start=storage.start+1;CHECK(!en_owned(arena,storage,bad,loader,&ram0,1));
    bad=ram0;bad.size=1;CHECK(!en_owned(arena,storage,bss,loader,&bad,1));
    CHECK(!en_valid((struct en_span){UINT64_MAX,1}));
    CHECK(!en_overlaps((struct en_span){0,0},arena));
    CHECK(!en_overlaps((struct en_span){arena.start-16,16},arena));
    CHECK(en_overlaps((struct en_span){arena.start-16,17},arena));
}
static void wire(uint8_t *r,unsigned kind)
{
    memset(r,0,E2_EXCEPTION_RESULT_SIZE);
    en_put(r,E2_EXCEPTION_MAGIC,4);en_put(r+4,E2_EXCEPTION_VERSION,2);en_put(r+6,E2_EXCEPTION_RESULT_SIZE,2);
    en_put(r+8,E2_EXPECTED,4);en_put(r+12,kind==EN_GUEST?31:7,4);
    en_put(r+16,kind==EN_GUEST?4:1,4);en_put(r+32,E2_R_ALL,8);
    en_put(r+192,E2_GUEST_DONE,8);en_put(r+208,3,8);en_put(r+248,1,8);
}
static void admission(void)
{
    struct en_call c={.flags=EN_ALL,.ret=E2_GUEST_DONE};uint8_t r[E2_EXCEPTION_RESULT_SIZE];wire(r,EN_GUEST);
    CHECK(en_result(EN_GUEST,&c,r,sizeof(r))==EN_PASS);
    c.flags^=EN_GPRS;CHECK(en_result(EN_GUEST,&c,r,sizeof(r))==EN_FATAL);c.flags=EN_ALL;
    c.ret=E2_CONTEXT_LOST;CHECK(en_result(EN_GUEST,&c,r,sizeof(r))==EN_FATAL);
    c.ret=E2_RESTORE_FAILED;CHECK(en_result(EN_GUEST,&c,r,sizeof(r))==EN_FATAL);
    c.ret=UINT64_MAX;CHECK(en_result(EN_GUEST,&c,r,sizeof(r))==EN_FATAL);
    c.ret=E2_GUEST_UNEXPECTED;en_put(r+12,7,4);CHECK(en_result(EN_GUEST,&c,r,sizeof(r))==EN_FAILED);
    en_put(r+32,E2_R_ALL&~E2_R_GUEST,8);CHECK(en_result(EN_GUEST,&c,r,sizeof(r))==EN_FATAL);
    wire(r,EN_GUEST);c.ret=E2_GUEST_DONE;en_put(r+4,1,2);CHECK(en_result(EN_GUEST,&c,r,sizeof(r))==EN_FATAL);
    wire(r,EN_BRK);c.ret=E2_EXPECTED;CHECK(en_result(EN_BRK,&c,r,sizeof(r))==EN_PASS);
    c.ret=E2_ENVIRONMENT;memset(r,0xa5,sizeof(r));CHECK(en_result(EN_GUEST,&c,r,sizeof(r))==EN_REFUSED);
    memset(r,0,sizeof(r));en_put(r,E2_RESULT_MAGIC,4);en_put(r+4,E2_VERSION,2);en_put(r+6,E2_RESULT_SIZE,2);en_put(r+12,3,4);
    c.ret=0;CHECK(en_result(EN_PREFLIGHT,&c,r,sizeof(r))==EN_PASS);
    en_put(r+8,E2_RANGE,4);CHECK(en_result(EN_PREFLIGHT,&c,r,sizeof(r))==EN_FAILED);
}
static void encoding(void)
{
    uint8_t out[EN_RECORD_BYTES+2];memset(out,0xa5,sizeof(out));struct en_call c={0};
    for(unsigned i=0;i<24;i++)c.before[i]=c.after[i]=0x12340000u+i;
    for(unsigned i=0;i<12;i++)c.gpr[i]=0x712u+i;
    c.ret=22;c.flags=EN_ALL;c.original_daif=0x80;c.observed_sp=c.expected_sp=0x4000;
    en_record(out+1,EN_GUEST,EN_PASS,1,&c);
    CHECK(out[0]==0xa5&&out[sizeof(out)-1]==0xa5);
    CHECK(en_get(out+1,4)==0x45324e41&&en_get(out+7,2)==EN_RECORD_BYTES);
    CHECK(en_get(out+1+16,4)==1&&en_get(out+1+24,8)==EN_ALL);
    for(unsigned i=0;i<24;i++) CHECK(en_get(out+1+64+8*i,8)==c.before[i]&&en_get(out+1+256+8*i,8)==c.after[i]);
    CHECK(en_get(out+1+584,8)==0x80);
}
static void descriptors(void)
{
    uint64_t old=arena.start|0x703,new=0;
    CHECK(en_page_descriptor(old,arena.start,1,&new));
    CHECK((new&0xc0)==0x80 && (new&(UINT64_C(3)<<53))==(UINT64_C(1)<<54));
    CHECK(en_page_descriptor(old,arena.start,0,&new));
    CHECK((new&0xc0)==0 && (new&(UINT64_C(3)<<53))==(UINT64_C(3)<<53));
    CHECK(!en_page_descriptor(old,arena.start+E2_PAGE_SIZE,1,&new));
    CHECK(!en_page_descriptor(old|4,arena.start,1,&new));
    CHECK(!en_page_descriptor(old|(UINT64_C(1)<<52),arena.start,1,&new));
    CHECK(!en_page_descriptor(old|(UINT64_C(1)<<51),arena.start,1,&new));
    CHECK(!en_page_descriptor(old&~UINT64_C(0x400),arena.start,1,&new));
    CHECK(!en_page_descriptor(old&~UINT64_C(2),arena.start,1,&new));
}
static void fixture(void)
{
    CHECK(fdt_create_empty_tree(tree,sizeof(tree))==0);
    const char compatible[]="apple,j713\0apple,t8132";
    CHECK(fdt_setprop(tree,0,"compatible",compatible,sizeof(compatible))==0);
    CHECK(fdt_setprop_u32(tree,0,"#address-cells",2)==0&&fdt_setprop_u32(tree,0,"#size-cells",2)==0);
    int m=fdt_add_subnode(tree,0,"memory");CHECK(m>=0);
    fdt64_t reg[]={cpu_to_fdt64(ram0.start),cpu_to_fdt64(ram0.size)};
    CHECK(fdt_setprop(tree,m,"reg",reg,sizeof(reg))==0);
    int ch=fdt_add_subnode(tree,0,"chosen");CHECK(ch>=0);
    CHECK(fdt_setprop_u64(tree,ch,"linux,initrd-start",kernel.start+kernel.size)==0);
    CHECK(fdt_setprop_u64(tree,ch,"linux,initrd-end",kernel.start+kernel.size+0x100000)==0);
    CHECK(fdt_add_mem_rsv(tree,loader.start,loader.size)==0);
}
static int facts(struct en_span ctrr,struct en_span k)
{ struct en_span r[E2_MAX_RANGES];unsigned n=0;return en_fdt_memory(tree,sizeof(tree),arena,loader,ctrr,k,r,&n); }
static void reservations(void)
{
    fixture();CHECK(facts((struct en_span){0},kernel)==0);
    CHECK(fdt_add_mem_rsv(tree,arena.start+16,16)==0);CHECK(facts((struct en_span){0},kernel)==8);
    fixture();struct en_span ctrr={loader.start,0x100000};
    CHECK(fdt_add_mem_rsv(tree,ctrr.start,ctrr.size)==0);CHECK(facts(ctrr,kernel)==0);
    CHECK(facts((struct en_span){0},kernel)==8);
    fixture();CHECK(facts((struct en_span){0},arena)==1);
    int chosen=fdt_path_offset(tree,"/chosen");CHECK(chosen>=0);
    CHECK(fdt_setprop_u64(tree,chosen,"linux,initrd-start",arena.start)==0);
    CHECK(fdt_setprop_u64(tree,chosen,"linux,initrd-end",arena.start+16)==0);CHECK(facts((struct en_span){0},kernel)==6);
}
static void reserved_nodes(void)
{
    fixture();int n=fdt_add_subnode(tree,0,"reserved-memory");CHECK(n>=0);
    CHECK(fdt_setprop_u32(tree,n,"#address-cells",2)==0&&fdt_setprop_u32(tree,n,"#size-cells",2)==0);
    CHECK(fdt_setprop(tree,n,"ranges",NULL,0)==0);
    int child=fdt_add_subnode(tree,n,"device");CHECK(child>=0);
    fdt64_t reg[]={cpu_to_fdt64(arena.start),cpu_to_fdt64(16)};
    CHECK(fdt_setprop(tree,child,"reg",reg,sizeof(reg))==0);CHECK(facts((struct en_span){0},kernel)==11);
    CHECK(fdt_setprop_string(tree,child,"status","disabled")==0);CHECK(facts((struct en_span){0},kernel)==11);
    fixture();CHECK(fdt_setprop_u32(tree,0,"#address-cells",1)==0);CHECK(facts((struct en_span){0},kernel)==1);
}
int main(void)
{ ranges();admission();encoding();descriptors();reservations();reserved_nodes();printf("PASS: native policy/FDT/encoding, %u checks (host metadata)\n",checks);return 0; }
