/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "boottrace_adapter.h"
#include "fb.h"
#include "uart.h"
#include "utils.h"
#include "libfdt/libfdt.h"
static struct xbt_state state;
static int uart_ready,fb_ready,timer_ready;
static void *trace_dt;
static size_t trace_capacity;
/* Only the boot CPU calls this adapter; publication is not reentrant. */
static uint8_t trace_wire[XBT_BYTES];
static void publish(void)
{
    uint8_t *wire=trace_wire;
    if(!trace_dt || xbt_encode(&state,wire,XBT_BYTES))return;
    /* Diagnostic only: failed publication never changes boot authorization. */
    if(fdt_check_full(trace_dt,trace_capacity))return;
    int chosen=fdt_path_offset(trace_dt,"/chosen");
    if(chosen>=0)fdt_setprop_inplace(trace_dt,chosen,XBT_PROPERTY,wire,XBT_BYTES);
}
void xboot_trace_init(void)
{
    xbt_reset(&state);uart_ready=fb_ready=timer_ready=0;
    trace_dt=NULL;trace_capacity=0;
}
void xboot_trace_uart_ready(int ready) { uart_ready=ready; }
void xboot_trace_timer_ready(void)
{
    state.frequency=mrs(CNTFRQ_EL0);
    timer_ready=state.frequency!=0;
}
void xboot_trace_fb_ready(void) { fb_ready=1; }
void xboot_trace_outputs_off(void) { uart_ready=fb_ready=0; }
void xboot_trace_dt_clear(void) { trace_dt=NULL;trace_capacity=0; }
void xboot_trace_dt_prepare(void *dt,size_t capacity)
{
    uint8_t *wire=trace_wire;xboot_trace_dt_clear();
    state.flags=1;
    if(!dt || fdt_check_full(dt,capacity) || xbt_encode(&state,wire,XBT_BYTES))return;
    int chosen=fdt_path_offset(dt,"/chosen");
    if(chosen<0 || fdt_setprop(dt,chosen,XBT_PROPERTY,wire,XBT_BYTES))return;
    trace_dt=dt;trace_capacity=capacity;
}
void xboot_trace_freeze_dt(void)
{
    /* Publish the cutoff while translation remains enabled.
     * No new trace calls are placed after mmu_shutdown in the boot path. */
    state.flags=3;
    publish();
    trace_dt=NULL;trace_capacity=0;
    uart_ready=fb_ready=timer_ready=0;
}
void xboot_trace(unsigned stage,unsigned kind,int result)
{
    uint64_t ticks=timer_ready?get_ticks():0;
    if(xbt_append(&state,stage,kind,result,ticks,timer_ready)) { publish();return; }
    if(!uart_ready && !fb_ready && !trace_dt)return;
    char line[128];int n;
    /* Raw counter avoids division and invented durations before timer readiness. */
    if(result==XBT_NO_RESULT)
        n=snprintf(line,sizeof(line),"6boot S2 #%03u %s %s tick=%llu%s\r\n",
          state.count,xbt_kind_name(kind),xbt_stage_name(stage),(unsigned long long)ticks,timer_ready?"":"(unknown)");
    else
        n=snprintf(line,sizeof(line),"6boot S2 #%03u %s %s ret=%d tick=%llu%s\r\n",
          state.count,xbt_kind_name(kind),xbt_stage_name(stage),result,(unsigned long long)ticks,timer_ready?"":"(unknown)");
    if(n<0) { publish();return; }
    size_t size=(size_t)n<sizeof(line)?(size_t)n:sizeof(line)-1;
    if(uart_ready) {
        size_t sent=uart_write_bounded(line,size,65536);
        uint32_t lost=(uint32_t)(size-sent);
        state.uart_bytes_dropped=lost>UINT32_MAX-state.uart_bytes_dropped?UINT32_MAX:state.uart_bytes_dropped+lost;
    }
    if(fb_ready)fb_boot_status(line,size,kind==XBT_END && result!=XBT_NO_RESULT && result!=0);
    publish();
}
