/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "xboot_boottrace.h"

void xbt_reset(struct xbt_state *s) { *s = (struct xbt_state){.flags=1}; }
static void drop(struct xbt_state *s)
{ if (s->dropped != UINT32_MAX) s->dropped++; }
int xbt_append(struct xbt_state *s, unsigned stage, unsigned kind,
               int32_t result, uint64_t ticks, unsigned time_valid)
{
    if (!s || stage<1 || stage>XBT_STAGE_LAST || kind<XBT_BEGIN || kind>XBT_POINT ||
        time_valid>1 || (time_valid && !s->frequency)) return -1;
    if (s->count>=XBT_CAPACITY) { drop(s); return -1; }
    struct xbt_event *e=&s->events[s->count];
    *e=(struct xbt_event){.sequence=s->count+1,.stage=stage,.kind=kind,
                        .flags=time_valid,.ticks=time_valid?ticks:0,.result=result};
    s->count++; return 0;
}
static void put32(uint8_t *p,uint32_t v)
{ for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(24-8*i)); }
static void put64(uint8_t *p,uint64_t v)
{ put32(p,(uint32_t)(v>>32));put32(p+4,(uint32_t)v); }
int xbt_encode(const struct xbt_state *s, void *out, size_t capacity)
{
    if(!s || !out || capacity<XBT_BYTES || s->count>XBT_CAPACITY ||
       (s->flags!=1 && s->flags!=3))return -1;
    uint8_t *p=out;
    for(unsigned i=0;i<XBT_BYTES;i++)p[i]=0;
    put32(p,0x58424c47);put32(p+4,1);put32(p+8,XBT_BYTES);
    put32(p+12,s->count);put32(p+16,XBT_CAPACITY);put32(p+20,s->dropped);
    put64(p+24,s->frequency);put32(p+32,s->flags); /* metadata-only, optional MMU cutoff */
    put32(p+36,s->uart_bytes_dropped);
    for(unsigned i=0;i<s->count;i++) {
        const struct xbt_event *e=&s->events[i];uint8_t *q=p+48+32*i;
        put32(q,e->sequence);put32(q+4,e->stage);put32(q+8,e->kind);put32(q+12,e->flags);
        put64(q+16,e->ticks);put32(q+24,(uint32_t)e->result);
    }
    return 0;
}
const char *xbt_kind_name(unsigned k)
{ return k==XBT_BEGIN?"BEGIN":k==XBT_END?"END":k==XBT_POINT?"POINT":"INVALID"; }
const char *xbt_stage_name(unsigned stage)
{
    static const char *const names[]={"invalid","stage2_entry","uart_setup",
      "cpu_setup","exception_setup","firmware_metadata","heap_setup","gxf_setup",
      "memory_controller","mmu_setup","interrupt_controller","power_manager",
      "display_setup","framebuffer_setup","cpufreq_fixup","sep_transport_setup",
      "payload_load","cpufreq_setup","secondary_cpus","mitigations","linux_dt",
      "cache_enable","static_tunables","clock_setup","usb_setup","pcie_setup",
      "handoff_checks","nvme_stop","exceptions_stop","usb_stop","display_stop",
      "framebuffer_stop","mmu_stop","kernel_branch","console_release"};
    return stage<=XBT_STAGE_LAST?names[stage]:names[0];
}
