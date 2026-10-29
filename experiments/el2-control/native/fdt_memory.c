/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "native.h"
#include "libfdt.h"
#include <string.h>

int en_fdt_memory(const void *dt,size_t capacity,struct en_span arena,struct en_span loader,
                   struct en_span ctrr,struct en_span kernel,struct en_span *ram,unsigned *count)
{
    if(!dt||!ram||!count||!en_valid(arena)||!en_valid(loader)||!en_valid(kernel)||
       en_overlaps(arena,kernel)||fdt_check_full(dt,capacity)||
       fdt_node_check_compatible(dt,0,"apple,j713")||fdt_node_check_compatible(dt,0,"apple,t8132")||
       fdt_address_cells(dt,0)!=2||fdt_size_cells(dt,0)!=2) return 1;
    int node=fdt_path_offset(dt,"/memory"),len;
    const fdt64_t *reg=fdt_getprop(dt,node,"reg",&len);
    if(!reg||len<=0||len%16||len/16>(int)E2_MAX_RANGES) return 2;
    *count=(unsigned)len/16;
    int covered=0,kernel_covered=0;
    for(unsigned i=0;i<*count;i++) {
        ram[i]=(struct en_span){fdt64_ld(reg+2*i),fdt64_ld(reg+2*i+1)};
        if(!en_valid(ram[i])) return 3;
        if(en_contains(ram[i],loader)) covered=1;
        if(en_contains(ram[i],kernel)) kernel_covered=1;
        for(unsigned j=0;j<i;j++) if(en_overlaps(ram[i],ram[j])) return 3;
    }
    if(!covered||!kernel_covered) return 4;
    struct en_span dt_span={(uintptr_t)dt,capacity};
    if(en_overlaps(arena,dt_span)) return 5;
    node=fdt_path_offset(dt,"/chosen");
    const fdt64_t *lo=fdt_getprop(dt,node,"linux,initrd-start",&len);
    if(!lo||len!=8) return 6;
    uint64_t start=fdt64_ld(lo);
    const fdt64_t *hi=fdt_getprop(dt,node,"linux,initrd-end",&len);
    if(!hi||len!=8||fdt64_ld(hi)<=start) return 6;
    if(en_overlaps(arena,(struct en_span){start,fdt64_ld(hi)-start})) return 6;
    int nr=fdt_num_mem_rsv(dt);
    if(nr<0||nr>256) return 7;
    for(int i=0;i<nr;i++) {
        struct en_span r;
        if(fdt_get_mem_rsv(dt,i,&r.start,&r.size)||!en_valid(r)) return 7;
        /* Memreserve protects loader/CTRR from Linux; the boot CPU owns
         * these static BSS objects. Other overlapping reservations refuse. */
        int owned=(r.start==loader.start&&r.size==loader.size) ||
                  (en_valid(ctrr)&&r.start==ctrr.start&&r.size==ctrr.size&&en_contains(ctrr,arena));
        if(!owned&&en_overlaps(r,arena)) return 8;
    }
    int reserved=fdt_path_offset(dt,"/reserved-memory");
    if(reserved==-FDT_ERR_NOTFOUND) return 0;
    if(reserved<0||fdt_address_cells(dt,reserved)!=2||fdt_size_cells(dt,reserved)!=2) return 9;
    const void *ranges=fdt_getprop(dt,reserved,"ranges",&len);
    if(!ranges||len) return 9;
    unsigned entries=0;
    fdt_for_each_subnode(node,dt,reserved) {
        const char *status=fdt_getprop(dt,node,"status",&len);
        if(status&&!((len==9&&!memcmp(status,"disabled",9))||(len==5&&!memcmp(status,"okay",5)))) return 10;
        reg=fdt_getprop(dt,node,"reg",&len);
        if(!reg) { if(len!=-FDT_ERR_NOTFOUND) return 10; continue; }
        if(len<=0||len%16) return 10;
        for(int i=0;i<len/16;i++) {
            struct en_span r={fdt64_ld(reg+2*i),fdt64_ld(reg+2*i+1)};
            if(++entries>256||!en_valid(r)||en_overlaps(arena,r)) return 11;
        }
    }
    return node==-FDT_ERR_NOTFOUND?0:12;
}
