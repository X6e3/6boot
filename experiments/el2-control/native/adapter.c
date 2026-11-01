/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "build_cfg.h"
#include "types.h"
#include "utils.h"
#include "adt.h"
#include "memory.h"
#include "gxf.h"
#include "smp.h"
#include "kboot.h"
#include "xnuboot.h"
#include "string.h"
#include "fb.h"
#include "libfdt/libfdt.h"
#include "el2_native.h"
#include "native.h"
#include "el2_native_image.h"

/* Static loader-owned storage remains reserved with the loader through
 * Linux handoff. Alignment slack avoids assuming the loader's load alignment. */
static uint8_t arena_storage[E2_ARENA_SIZE+E2_ARENA_ALIGN] __attribute__((aligned(0x4000)));
static uint8_t caller_stack[E2_PAGE_SIZE] __attribute__((aligned(0x4000)));
static uint8_t request[E2_REQUEST_MAX],result[E2_EXCEPTION_RESULT_SIZE],record[EN_RECORD_BYTES];
static uint8_t ownership[176],mappings[1056];
struct en_call el2_native_call_state __attribute__((aligned(16)));
static struct en_span kernel_range,ctrr_range;
static bool blocked=true,attempted;
extern char _base[],_end[],_bss_start[],_bss_end[];

void el2_native_set_kernel(const void *kernel)
{
    const struct kernel_header *k=kernel;
    kernel_range=(struct en_span){(uintptr_t)kernel,k->image_size};
}
void el2_native_set_ctrr(uint64_t start,uint64_t size) { ctrr_range=(struct en_span){start,size}; }
int el2_native_handoff_allowed(void) { return !blocked; }

static unsigned mode_of(void *dt,int chosen)
{
    int len; const char *s=fdt_getprop(dt,chosen,"6boot,el2-mode",&len);
    if(s&&len==10&&!memcmp(s,"preflight",10)) return EN_PREFLIGHT;
    if(s&&len==4&&!memcmp(s,"brk",4)) return EN_BRK;
    if(s&&len==6&&!memcmp(s,"guest",6)) return EN_GUEST;
    return 0;
}
static int prop(void *dt,int node,const char *name,const void *bytes,unsigned size)
{ return fdt_setprop(dt,node,name,bytes,(int)size); }
static int publish(void *dt,int node,unsigned mode,unsigned status,unsigned called,uint64_t arena,uint64_t ticks,unsigned reason)
{
    en_record(record,mode,status,called,&el2_native_call_state);
    return prop(dt,node,"6boot,el2-result",result,called&&mode==EN_PREFLIGHT?E2_RESULT_SIZE:sizeof(result)) ||
        prop(dt,node,"6boot,el2-native-state",record,sizeof(record)) ||
        prop(dt,node,"6boot,el2-ownership",ownership,sizeof(ownership)) ||
        prop(dt,node,"6boot,el2-mappings",mappings,sizeof(mappings)) ||
        fdt_setprop_u32(dt,node,"6boot,el2-adapter-status",status) ||
        fdt_setprop_u32(dt,node,"6boot,el2-refusal-reason",reason) ||
        fdt_setprop_u32(dt,node,"6boot,el2-called",called) ||
        fdt_setprop_u32(dt,node,"6boot,el2-return",(uint32_t)el2_native_call_state.ret) ||
        fdt_setprop_u32(dt,node,"6boot,el2-safe",!blocked) ||
        fdt_setprop_u64(dt,node,"6boot,el2-load-address",arena) ||
        fdt_setprop_u64(dt,node,"6boot,el2-ticks",ticks);
}
/* Probe already-mapped owned RAM, preserving PAR and the incoming masks.
 * No mapping, device or firmware access is attempted on a refusal. */
static int mapped(uint64_t base,size_t size,int prepared)
{
    uint64_t saved=mrs(PAR_EL1),daif=mrs(DAIF); int ok=1;
    uint64_t flags=en_get(mappings+8,4)|(prepared?4u:1u);
    en_put(mappings+8,flags,4);
    __asm__ volatile("msr daifset, #0xf":::"memory");
    for(uint64_t p=base;p<base+size;p+=SZ_4K) {
        unsigned index=(unsigned)((p-base)/SZ_4K),offset=32+32*index+(prepared?16u:0u);
        __asm__ volatile("at s1e2r, %0; isb"::"r"(p):"memory");
        uint64_t par=mrs(PAR_EL1);
        en_put(mappings+offset,par,8);
        if((par&1)||((par>>12)&0xffffffffffULL)!=(p>>12)||(par>>56)!=0xff) {ok=0;break;}
        __asm__ volatile("at s1e2w, %0; isb"::"r"(p):"memory");
        par=mrs(PAR_EL1);en_put(mappings+offset+8,par,8);
        if(prepared&&p<base+E2_PAGE_SIZE) {if(!(par&1)) {ok=0;break;}}
        else if((par&1)||((par>>12)&0xffffffffffULL)!=(p>>12)) {ok=0;break;}
        en_put(mappings+(prepared?20:16),index+1,4);
    }
    msr_sync(PAR_EL1,saved);
    if(mrs(PAR_EL1)!=saved) { blocked=true;ok=0; }
    if(ok) en_put(mappings+8,flags|(prepared?8u:2u),4);
    if(!blocked) msr(DAIF,daif);
    return ok;
}

int el2_native_run(void *dt)
{
    int node=fdt_path_offset(dt,"/chosen");
    if(attempted||node<0) return -1;
    attempted=true;
    unsigned mode=mode_of(dt,node),status=EN_REFUSED,called=0,reason=0;
    uint64_t arena=ALIGN_UP((uintptr_t)arena_storage,E2_ARENA_ALIGN),ticks=0;
    memset(&el2_native_call_state,0,sizeof(el2_native_call_state));
    memset(result,0xa5,sizeof(result)); memset(request,0,sizeof(request));
    memset(ownership,0,sizeof(ownership));memset(mappings,0,sizeof(mappings));
    const struct en_span owners[]={
        {(uintptr_t)_base,(uintptr_t)_end-(uintptr_t)_base},
        {(uintptr_t)_bss_start,(uintptr_t)_bss_end-(uintptr_t)_bss_start},
        {(uintptr_t)arena_storage,sizeof(arena_storage)},{arena,E2_ARENA_SIZE},
        {(uintptr_t)caller_stack,sizeof(caller_stack)},{(uintptr_t)request,sizeof(request)},
        {(uintptr_t)result,sizeof(result)},{(uintptr_t)&el2_native_call_state,sizeof(el2_native_call_state)},
        kernel_range,ctrr_range};
    en_put(ownership,0x45324f57,4);en_put(ownership+4,1,2);en_put(ownership+6,sizeof(ownership),2);
    en_put(ownership+8,10,4);
    for(unsigned i=0;i<10;i++) {en_put(ownership+16+16*i,owners[i].start,8);en_put(ownership+24+16*i,owners[i].size,8);}
    en_put(mappings,0x45324d50,4);en_put(mappings+4,1,2);en_put(mappings+6,sizeof(mappings),2);
    en_put(mappings+12,32,4);en_put(mappings+24,arena,8);
    /* Preallocate all result slots before entering the experimental vectors. */
    if(fdt_setprop_string(dt,node,"6boot,el2-build-id",EL2_NATIVE_BUILD_ID)||
       fdt_setprop_string(dt,node,"6boot,el2-module-sha256",EL2_NATIVE_IMAGE_SHA)||
       prop(dt,node,"6boot,el2-request",request,sizeof(request))||
       publish(dt,node,mode,EN_REFUSED,0,arena,0,0)) return -1;
    blocked=false; /* No module state has changed yet; refusals can reach Linux for capture. */
    if(!mode) {reason=1;goto finish;}
    if(chip_id!=E2_CHIP||!is_boot_cpu()||get_page_size()!=E2_PAGE_SIZE||mrs(CurrentEL)!=8) {reason=2;goto finish;}
    uint64_t hcr=mrs(HCR_EL2);
    if(mrs(SPSel)!=1||(hcr&(E2_HCR_E2H|E2_HCR_TGE|E2_HCR_RW))!=(E2_HCR_E2H|E2_HCR_TGE|E2_HCR_RW)||
       (hcr&E2_HCR_VM)||!supports_pan()||gxf_enabled()) {reason=3;goto finish;}
    struct en_span ram[E2_MAX_RANGES]; unsigned count=0;
    struct en_span region={arena,E2_ARENA_SIZE},loader={(uintptr_t)_base,(uintptr_t)_end-(uintptr_t)_base};
    struct en_span storage={(uintptr_t)arena_storage,sizeof(arena_storage)};
    struct en_span bss={(uintptr_t)_bss_start,(uintptr_t)_bss_end-(uintptr_t)_bss_start};
    int fdt_error=en_fdt_memory(dt,(size_t)fdt_totalsize(dt),region,loader,ctrr_range,kernel_range,ram,&count);
    if(fdt_error) {reason=100u+(unsigned)fdt_error;goto finish;}
    if(!en_owned(region,storage,bss,loader,ram,count)||EL2_NATIVE_IMAGE_BYTES>E2_PAGE_SIZE) {reason=4;goto finish;}
    for(int i=0;i<MAX_CPUS;i++) {
        if(secondary_stacks[i]&&en_overlaps(region,(struct en_span){(uintptr_t)secondary_stacks[i],SECONDARY_STACK_SIZE})) {reason=5;goto finish;}
        if(i<MAX_EL3_CPUS&&secondary_stacks_el3[i]&&
           en_overlaps(region,(struct en_span){(uintptr_t)secondary_stacks_el3[i],SECONDARY_STACK_SIZE})) {reason=5;goto finish;}
    }
    if(!mapped(arena,E2_ARENA_SIZE,0)) {reason=6;goto finish;}
    memset((void *)arena,0,E2_ARENA_SIZE);
    memcpy((void *)arena,el2_native_image,EL2_NATIVE_IMAGE_BYTES);
    dc_cvau_range((void *)arena,E2_PAGE_SIZE); sysop("dsb ish");
    ic_ivau_range((void *)arena,E2_PAGE_SIZE); sysop("dsb ish; isb");
    /* These pages belong exclusively to this loader object and stay reserved;
     * no inherited device/DART mapping is changed or temporary page freed. */
    if(en_protect_arena(arena)) {reason=7;goto finish;}
    if(!mapped(arena,E2_ARENA_SIZE,1)) {reason=7;goto finish;}
    en_put(request,E2_REQUEST_MAGIC,4);en_put(request+4,E2_VERSION,2);en_put(request+6,E2_HEADER_SIZE,2);
    size_t size=E2_HEADER_SIZE+16u*(count+2u);
    en_put(request+8,size,4);en_put(request+12,E2_PREFLIGHT,4);en_put(request+16,chip_id,4);
    memcpy(request+20,"J713",4);en_put(request+24,E2_REQUIRED_FLAGS,4);
    en_put(request+32,arena,8);en_put(request+40,E2_ARENA_SIZE,8);
    en_put(request+48,count,4);en_put(request+56,2,4);en_put(request+64,mrs(MPIDR_EL1),8);
    for(unsigned i=0;i<count;i++) {en_put(request+80+16u*i,ram[i].start,8);en_put(request+88+16u*i,ram[i].size,8);}
    /* Conservative exclusions cover every address outside our owned arena.
     * Ownership and reservations were checked independently above. */
    unsigned x=80+16u*count;
    en_put(request+x,0,8);en_put(request+x+8,arena,8);
    en_put(request+x+16,arena+E2_ARENA_SIZE,8);en_put(request+x+24,UINT64_MAX-arena-E2_ARENA_SIZE,8);
    if(prop(dt,node,"6boot,el2-request",request,(unsigned)size)) {blocked=true;reason=8;goto finish;}
    uint64_t entry=arena+(mode==EN_PREFLIGHT?EL2_NATIVE_PREFLIGHT:mode==EN_BRK?EL2_NATIVE_BRK:EL2_NATIVE_GUEST);
    printf("6boot EL2_NATIVE BEGIN mode=%u build=%s arena=%lx entry=%lx\n",mode,EL2_NATIVE_BUILD_ID,arena,entry);
    uint64_t t=mrs(CNTPCT_EL0);
    called=1;
    el2_native_invoke(entry,request,size,result,sizeof(result),caller_stack+sizeof(caller_stack));
    ticks=mrs(CNTPCT_EL0)-t;
    status=(unsigned)en_result(mode,&el2_native_call_state,result,sizeof(result));
    blocked=status==EN_FATAL;
finish:
    if(reason) {status=blocked?EN_FATAL:EN_REFUSED;el2_native_call_state.ret=E2_ENVIRONMENT;}
    if(publish(dt,node,mode,status,called,arena,ticks,reason)) blocked=true;
    /* Preserve the caller's framebuffer activation/silent state. */
    printf("6boot EL2_NATIVE END mode=%u adapter=%u called=%u ret=%lu safe=%u reason=%u outer=%lx ticks=%lu\n",
           mode,status,called,el2_native_call_state.ret,!blocked,reason,el2_native_call_state.flags,ticks);
    if(blocked) {next_stage.entry=NULL;printf("6boot EL2_NATIVE STOP: kernel handoff blocked; restore REL3 manually\n");return -1;}
    return 0;
}
