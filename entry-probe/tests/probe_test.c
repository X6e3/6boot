/* SPDX-License-Identifier: GPL-2.0-only */
#include "xboot_probe.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put32(unsigned char *p, uint32_t n)
{ for (unsigned i = 0; i < 4; i++) p[i] = (unsigned char)(n >> (8*i)); }
static void put64(unsigned char *p, uint64_t n)
{ put32(p, (uint32_t)n); put32(p+4, (uint32_t)(n>>32)); }
static uint32_t get32(const unsigned char *p)
{ return p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static void fixture(unsigned char *p)
{
    memset(p, 0, 320);put32(p, XP_CONTEXT_MAGIC);put32(p+4, 64u<<16|1);
    put32(p+8, 80);put32(p+12, XP_CHIP);memcpy(p+16,"J713",4);
    put64(p+24,0x110000);put64(p+32,0x4000);
    put64(p+40,0x120000);put64(p+48,0x1000);put32(p+56,1);
    put64(p+64,0x100000);put64(p+72,0x100000);
}
static unsigned checks;
static void expect(unsigned char *p,size_t size,uint64_t el,int status)
{
    unsigned char out[80];memset(out,0xCC,sizeof(out));
    assert(xp_probe(p,size,out+1,64,el,0x8000000001ULL)==status);
    assert(get32(out+1)==XP_RESULT_MAGIC && get32(out+13)==(uint32_t)status);
    assert(get32(out+5)==XP_VERSION && get32(out+9)==XP_RESULT_SIZE);
    assert(get32(out+17)==(uint32_t)el && get32(out+21)==1 && get32(out+25)==0x80);
    assert(get32(out+29)==1 && out[0]==0xCC && out[65]==0xCC);
    for(unsigned i=40;i<64;i++)assert(out[i+1]==0);
    if(status!=XP_OK)assert(get32(out+33)==0 && get32(out+37)==0);
    checks++;
}
int main(void)
{
    unsigned char p[320],out[64],saved[320];
    fixture(p);memcpy(saved,p,sizeof(p));expect(p,80,4,XP_OK);assert(!memcmp(p,saved,sizeof(p)));
    expect(p,80,8,XP_OK);expect(p,80,0,XP_EL);expect(p,80,12,XP_EL);
    /* Every truncation exercises the parser under ASan/UBSan. */
    for(unsigned n=0;n<80;n++) {
        unsigned char *small=malloc(n?n:1);assert(small);memcpy(small,p,n);
        assert(xp_probe(small,n,out,sizeof(out),4,0)!=XP_OK);free(small);checks++;
    }
    put32(p+8,81);expect(p,80,4,XP_ABI);
    fixture(p);put32(p+4,64u<<16|2);expect(p,80,4,XP_ABI);
    fixture(p);put32(p,0);expect(p,80,4,XP_ABI);
    fixture(p);put32(p+20,1);expect(p,80,4,XP_ABI);
    fixture(p);put32(p+60,1);expect(p,80,4,XP_ABI);
    fixture(p);p[16]='X';expect(p,80,4,XP_PLATFORM);
    fixture(p);put32(p+12,0x8103);expect(p,80,4,XP_PLATFORM);
    fixture(p);put32(p+56,0);expect(p,80,4,XP_LENGTH);
    fixture(p);put32(p+56,17);expect(p,80,4,XP_LENGTH);
    fixture(p);put64(p+32,0);expect(p,80,4,XP_RANGE);
    fixture(p);put64(p+24,UINT64_MAX-4);expect(p,80,4,XP_RANGE);
    fixture(p);put64(p+72,UINT64_MAX);expect(p,80,4,XP_RANGE);
    fixture(p);put64(p+40,0x110100);expect(p,80,4,XP_OVERLAP);
    fixture(p);put64(p+40,0x200000);expect(p,80,4,XP_COVERAGE);
    fixture(p);put32(p+8,96);put32(p+56,2);put64(p+80,0x180000);put64(p+88,0x1000);expect(p,96,4,XP_OVERLAP);
    fixture(p);put32(p+8,96);put32(p+56,2);put64(p+80,0x200000);put64(p+88,0x1000);expect(p,96,4,XP_OK);
    fixture(p);put32(p+8,96);put32(p+56,2);put64(p+64,0x100000);put64(p+72,0x11800);put64(p+80,0x111800);put64(p+88,0x10000);expect(p,96,4,XP_COVERAGE);
    fixture(p);memset(out,0xCC,sizeof(out));
    assert(xp_probe(NULL,80,out,64,4,0)==XP_ARGUMENT && out[0]==0xCC);checks++;
    assert(xp_probe(p,80,NULL,64,4,0)==XP_ARGUMENT);checks++;
    assert(xp_probe(p,80,out,63,4,0)==XP_ARGUMENT && out[0]==0xCC);checks++;
    memcpy(saved,p,sizeof(p));assert(xp_probe(p,80,p+16,64,4,0)==XP_ARGUMENT && !memcmp(p,saved,sizeof(p)));checks++;
    assert(xp_probe(p,SIZE_MAX,out,64,4,0)==XP_ARGUMENT && out[0]==0xCC);checks++;
    assert(xp_probe(p,80,(void *)(UINTPTR_MAX-31),64,4,0)==XP_ARGUMENT);checks++;
    expect(p,321,4,XP_LENGTH);
    /* Unaligned input and result addresses are supported. */
    unsigned char unaligned[321];memcpy(unaligned+1,p,320);expect(unaligned+1,80,4,XP_OK);
    unsigned char oversized[81];memcpy(oversized,p,80);oversized[80]=0;
    expect(oversized,81,4,XP_ABI);
    fixture(p);put32(p+8,320);put32(p+56,16);
    for(unsigned i=1;i<16;i++) {
        put64(p+64+16*i,0x300000+0x10000*i);put64(p+72+16*i,0x1000);
    }
    expect(p,320,4,XP_OK);
    printf("PASS: %u input/ABI/range/EL/output checks (synthetic metadata, no device access)\n",checks);
    return 0;
}
