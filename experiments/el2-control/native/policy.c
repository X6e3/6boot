/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "native.h"
uint64_t en_get(const uint8_t *p,unsigned bytes)
{ uint64_t v=0; for(unsigned i=0;i<bytes;i++) v|=(uint64_t)p[i]<<(8*i); return v; }
void en_put(uint8_t *p,uint64_t value,unsigned bytes)
{ for(unsigned i=0;i<bytes;i++) p[i]=(uint8_t)(value>>(8*i)); }
int en_valid(struct en_span r) { return r.size && r.start<=UINT64_MAX-r.size; }
int en_contains(struct en_span a,struct en_span b)
{ return en_valid(a)&&en_valid(b)&&a.start<=b.start&&b.start+b.size<=a.start+a.size; }
int en_overlaps(struct en_span a,struct en_span b)
{ return en_valid(a)&&en_valid(b)&&a.start<b.start+b.size&&b.start<a.start+a.size; }
int en_page_descriptor(uint64_t old,uint64_t address,int code,uint64_t *replacement)
{
    const uint64_t mask=UINT64_C(0x0003ffffffffc000);
    const uint64_t permissions=(UINT64_C(3)<<6)|(UINT64_C(3)<<53);
    if(!replacement||address%E2_PAGE_SIZE||(old&3)!=3||(old&mask)!=address||
       (old&((UINT64_C(1)<<51)|(UINT64_C(1)<<52)))||!(old&(UINT64_C(1)<<10))||
       ((old>>2)&7)!=0) return 0;
    *replacement=(old&~permissions)|(code ? (UINT64_C(1)<<7)|(UINT64_C(1)<<54) : UINT64_C(3)<<53);
    return 1;
}
int en_owned(struct en_span arena,struct en_span storage,struct en_span bss,struct en_span loader,
             const struct en_span *ram,unsigned count)
{
    if(!arena.start || arena.start%E2_ARENA_ALIGN || arena.size!=E2_ARENA_SIZE ||
       !en_contains(storage,arena)||!en_contains(bss,storage)||!en_contains(loader,bss)||
       !ram||!count||count>E2_MAX_RANGES) return 0;
    for(unsigned i=0;i<count;i++) if(en_contains(ram[i],loader)) return 1;
    return 0;
}
int en_result(unsigned mode,const struct en_call *c,const uint8_t *r,size_t n)
{
    if(!c||!r||c->flags!=EN_ALL||c->ret> E2_GUEST_SIGNATURE||c->ret==E2_CONTEXT_LOST||c->ret==E2_RESTORE_FAILED)
        return EN_FATAL;
    if(c->ret>=E2_ARGUMENT&&c->ret<=E2_CAPACITY) return EN_REFUSED;
    if(mode==EN_PREFLIGHT) {
        if(n<E2_RESULT_SIZE||en_get(r,4)!=E2_RESULT_MAGIC||en_get(r+4,2)!=E2_VERSION||
           en_get(r+6,2)!=E2_RESULT_SIZE) return EN_FAILED;
        return c->ret==E2_READY&&en_get(r+8,4)==E2_READY&&en_get(r+12,4)==3 ? EN_PASS:EN_FAILED;
    }
    if(mode!=EN_BRK&&mode!=EN_GUEST) return EN_FATAL;
    if(n<E2_EXCEPTION_RESULT_SIZE||en_get(r,4)!=E2_EXCEPTION_MAGIC||
       en_get(r+4,2)!=E2_EXCEPTION_VERSION||en_get(r+6,2)!=E2_EXCEPTION_RESULT_SIZE)
        return EN_FATAL; /* Do not trust a missing record after vector installation. */
    uint64_t flags=en_get(r+12,4);
    if(!(flags&E2_X_RESTORED)||en_get(r+32,8)!=E2_R_ALL||en_get(r+160,8)||en_get(r+168,8))
        return EN_FATAL;
    if(mode==EN_BRK)
        return c->ret==E2_EXPECTED&&en_get(r+8,4)==E2_EXPECTED&&flags==7&&
               en_get(r+16,4)==1&&en_get(r+24,8)==0 ? EN_PASS:EN_FAILED;
    return c->ret==E2_GUEST_DONE&&en_get(r+8,4)==E2_EXPECTED&&flags==31&&
           en_get(r+16,4)==4&&!en_get(r+24,8)&&en_get(r+192,8)==E2_GUEST_DONE&&
           !en_get(r+200,8)&&en_get(r+208,8)==3&&en_get(r+248,8)==1 ? EN_PASS:EN_FAILED;
}
void en_record(uint8_t out[EN_RECORD_BYTES],unsigned mode,unsigned status,unsigned called,
               const struct en_call *c)
{
    for(unsigned i=0;i<EN_RECORD_BYTES;i++) out[i]=0;
    en_put(out,0x45324e41,4); en_put(out+4,1,2); en_put(out+6,EN_RECORD_BYTES,2);
    en_put(out+8,mode,4); en_put(out+12,status,4); en_put(out+16,called,4);
    en_put(out+24,c->flags,8); en_put(out+32,c->ret,8);
    for(unsigned i=0;i<24;i++) { en_put(out+64+8*i,c->before[i],8); en_put(out+256+8*i,c->after[i],8); }
    for(unsigned i=0;i<12;i++) en_put(out+448+8*i,c->gpr[i],8);
    const uint64_t tail[]={c->observed_sp,c->expected_sp,c->ret,c->original_sp,c->flags,c->original_daif};
    for(unsigned i=0;i<6;i++) en_put(out+544+8*i,tail[i],8);
}
