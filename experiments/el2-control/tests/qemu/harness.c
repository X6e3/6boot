/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "el2_exception.h"
#ifdef E2_TEST_NATIVE
#include "native.h"
struct en_call el2_native_call_state __attribute__((aligned(16)));
#endif

#define ARENA UINT64_C(0x40800000)
static uint8_t request[112], result[E2_EXCEPTION_RESULT_SIZE];
extern uint64_t test_observed[16];
extern int test_call(uint64_t entry, void *req, size_t size, void *out, size_t cap);
extern void test_exit(unsigned status) __attribute__((noreturn));

static void print(const char *p)
{
    register uint64_t x0 __asm__("x0") = 4; /* SYS_WRITE0 */
    register const char *x1 __asm__("x1") = p;
    __asm__ volatile("hlt #0xf000" : "+r"(x0), "+r"(x1) :: "memory");
}
static void hex(uint64_t v)
{
    char text[19] = "0x0000000000000000";
    const char *digits = "0123456789abcdef";
    for (unsigned i = 0; i < 16; i++) text[17-i] = digits[(v >> (4*i)) & 15];
    print(text);
}
#ifdef E2_TEST_NATIVE
static void wire(const char *key,const uint8_t *data,unsigned size)
{
    const char *digits="0123456789abcdef";char line[129];
    print(key);
    for(unsigned off=0;off<size;) {
        unsigned used=0;
        while(used<128 && off<size) {
            line[used++]=digits[data[off]>>4];line[used++]=digits[data[off++]&15];
        }
        line[used]=0;print(line);
    }
    print("\n");
}
#endif
static void put32(uint8_t *p, uint32_t v)
{ for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(i*8)); }
static void put64(uint8_t *p, uint64_t v)
{ for (unsigned i=0;i<8;i++) p[i]=(uint8_t)(v>>(i*8)); }
static uint64_t get(const uint8_t *p, unsigned n)
{ uint64_t v=0; for(unsigned i=0;i<n;i++) v|=(uint64_t)p[i]<<(8*i); return v; }

struct state { uint64_t value[18]; };
static const char *const names[] = {"VBAR_EL2", "DAIF", "HCR_EL2", "SP_EL0", "SP_EL1",
    "SCTLR_EL12", "VBAR_EL12", "ELR_EL12", "SPSR_EL12", "ELR_EL2", "SPSR_EL2",
    "ESR_EL2", "FAR_EL2", "PAR_EL1", "SPSel", "PAN", "ESR_EL12", "FAR_EL12"};
static void snapshot(struct state *s)
{
#define READ(i, reg) __asm__ volatile("mrs %0, " #reg : "=r"(s->value[i]))
    READ(0, VBAR_EL2); READ(1, DAIF); READ(2, HCR_EL2); READ(3, SP_EL0); READ(4, SP_EL1);
    READ(5, SCTLR_EL12); READ(6, VBAR_EL12); READ(7, ELR_EL12); READ(8, SPSR_EL12);
    READ(9, ELR_EL2); READ(10, SPSR_EL2); READ(11, ESR_EL2); READ(12, FAR_EL2);
    READ(13, PAR_EL1); READ(14, SPSel); READ(15, PAN); READ(16, ESR_EL12); READ(17, FAR_EL12);
#undef READ
}
static unsigned one(const char *name, uint64_t offset, int wanted)
{
    struct state before, after;
    for(unsigned i=0;i<sizeof(result);i++) result[i]=0xa5;
    snapshot(&before);
    int ret=test_call(ARENA+offset,request,sizeof(request),result,sizeof(result));
    snapshot(&after);
    unsigned errors=0;
    print("CASE "); print(name); print(" ret="); hex((uint64_t)(unsigned)ret);
    print(" status="); hex(get(result+8,4)); print(" flags="); hex(get(result+12,4));
    if(offset) {
        print(" count="); hex(get(result+16,4)); print(" restore="); hex(get(result+32,8));
        print(" guest="); hex(get(result+192,8));
    }
    print("\n");
    if(ret!=wanted) errors++;
    for(unsigned i=0;i<18;i++) if(before.value[i]!=after.value[i]) {
        print("CHANGED "); print(names[i]); print(" before="); hex(before.value[i]);
        print(" after="); hex(after.value[i]); print("\n");
        /* Restore-fault injection intentionally leaves guest/HCR/PAR state
         * dirty. It must report failure, while still returning its caller. */
        if(!E2_TEST_PARTIAL || i==0 || i==1 || i==3 || i==9 || i==10 || i==11 || i==12 || i==14 || i==15)
            errors++;
    }
    for(unsigned i=0;i<12;i++) if(test_observed[i]!=0x700u+18u+i) {
        print("CALLEE_GPR_MISMATCH "); hex(i+18); print("\n"); errors++;
    }
    if(test_observed[12]!=test_observed[13]) { print("CALLER_SP_MISMATCH\n"); errors++; }
    if(offset) {
        if(get(result,4)!=E2_EXCEPTION_MAGIC || get(result+4,2)!=E2_TEST_RESULT_VERSION ||
           get(result+6,2)!=E2_EXCEPTION_RESULT_SIZE) errors++;
        if(wanted==E2_RESTORE_FAILED) {
            if(get(result+8,4)!=E2_RESTORE_FAILED) errors++;
            if(E2_TEST_PARTIAL && ((get(result+12,4)&E2_X_RESTORED) || get(result+32,8)==E2_R_ALL)) errors++;
            if(E2_TEST_PARTIAL) {
                const uint8_t *q=result+E2_OVERFLOW_OFFSET;
                if(get(q,4)!=E2_OVERFLOW_MAGIC || get(q+8,8)!=0x02000000 ||
                   get(q+16,8)!=ARENA+E2_TEST_RESTORE_PC || get(result+24,8)!=1) errors++;
                print("OVERFLOW esr="); hex(get(q+8,8)); print(" elr="); hex(get(q+16,8)); print("\n");
            }
            if(!E2_TEST_PARTIAL && (!(get(result+12,4)&E2_X_RESTORED) || get(result+32,8)!=E2_R_ALL)) errors++;
        } else if(get(result+8,4)!=E2_EXPECTED || get(result+32,8)!=E2_R_ALL) errors++;
    } else if(get(result,4)!=E2_RESULT_MAGIC || get(result+8,4)!=E2_READY || get(result+12,4)!=3) errors++;
    if(!offset) {
        uint64_t features[4];
        __asm__ volatile("mrs %0, ID_AA64MMFR0_EL1" : "=r"(features[0]));
        __asm__ volatile("mrs %0, ID_AA64MMFR1_EL1" : "=r"(features[1]));
        __asm__ volatile("mrs %0, ID_AA64MMFR2_EL1" : "=r"(features[2]));
        __asm__ volatile("mrs %0, ID_AA64PFR0_EL1" : "=r"(features[3]));
        if(get(result+4,2)!=E2_RESULT_VERSION || get(result+6,2)!=E2_RESULT_SIZE) errors++;
        for(unsigned i=0;i<4;i++) if(get(result+128+8*i,8)!=features[i]) errors++;
    }
    if(wanted==E2_GUEST_DONE) {
        if(!(get(result+12,4)&E2_X_GUEST_DONE) || get(result+16,4)!=(E2_TEST_HAS_SIGNATURE ? 4u : 3u)) errors++;
        const unsigned states[]={E2_EXPECTED,E2_GUEST_HVC,E2_GUEST_SIGNATURE,E2_GUEST_DONE};
        const uint64_t esrs[]={UINT64_C(0xf2006e32),UINT64_C(0x5a006e33),UINT64_C(0x5a006e37),UINT64_C(0x5a006e34)};
        const uint64_t pcs[]={ARENA+E2_TEST_BRK_PC,ARENA+E2_TEST_PROBE_PC,
                              ARENA+E2_TEST_SIGNATURE_PC,ARENA+E2_TEST_DONE_PC};
        for(unsigned i=0;i<(E2_TEST_HAS_SIGNATURE ? 4u : 3u);i++) {
            unsigned event=(!E2_TEST_HAS_SIGNATURE && i==2) ? 3u : i;
            const uint8_t *frame=result+E2_EXCEPTION_HEADER_SIZE+E2_FRAME_SIZE*i;
            if(get(frame+EF_STATUS,8)!=states[event] || get(frame+EF_ESR,8)!=esrs[event]) errors++;
            if(get(frame+EF_ELR,8)!=pcs[event] || (get(frame+EF_SPSR,8)&0x1f)!=(event ? 5u : 9u)) errors++;
            uint64_t resume=event==0 ? pcs[event]+4 : event==3 ? ARENA+E2_TEST_RETURN_PC : pcs[event];
            if(get(frame+EF_RESUME_ELR,8)!=resume || get(frame+EF_FLAGS,8)!=E2_F_ESR_VALID) errors++;
            for(unsigned reg=0;reg<31;reg++) {
                uint64_t value=(event ? E2_GUEST_PATTERN : 0x100u)+reg;
                if(reg==0 && event==2) value=E2_HVC_REPLY;
                if(reg==0 && event==3) value=0;
                if(get(frame+8*reg,8)!=value) errors++;
            }
            print("TRACE esr="); hex(get(frame+EF_ESR,8)); print(" elr="); hex(get(frame+EF_ELR,8)); print("\n");
        }
        if(E2_TEST_HAS_SIGNATURE && (!(get(result+12,4)&E2_X_SIGNATURE) || get(result+248,8)!=1)) errors++;
    } else if(wanted!=E2_EXPECTED && wanted!=E2_READY && (get(result+12,4)&(E2_X_GUEST_DONE|E2_X_SIGNATURE))) errors++;
#ifdef E2_TEST_NATIVE
    unsigned mode=offset==0 ? EN_PREFLIGHT : offset==E2_TEST_BRK_ENTRY ? EN_BRK : EN_GUEST;
    int status=en_result(mode,&el2_native_call_state,result,sizeof(result));
    int expected=wanted==E2_RESTORE_FAILED ? EN_FATAL :
        (wanted==E2_READY||wanted==E2_EXPECTED||wanted==E2_GUEST_DONE) ? EN_PASS : EN_FAILED;
    uint64_t expected_flags=wanted==E2_RESTORE_FAILED ? (E2_TEST_PARTIAL ? 14u : 15u) : EN_ALL;
    print("NATIVE_GATE flags="); hex(el2_native_call_state.flags);
    print(" status="); hex((uint64_t)(unsigned)status); print("\n");
    if(status!=expected || el2_native_call_state.flags!=expected_flags) errors++;
    uint8_t native_record[EN_RECORD_BYTES];
    en_record(native_record,mode,(unsigned)status,1,&el2_native_call_state);
    wire("WIRE_NATIVE=",native_record,sizeof(native_record));
    wire("WIRE_RESULT=",result,offset ? sizeof(result) : E2_RESULT_SIZE);
#endif
    print(errors ? "CASE_FAIL\n" : "CASE_PASS\n");
    return errors;
}
void test_exception_report(uint64_t vector, uint64_t esr, uint64_t elr, uint64_t spsr)
{
    print("HARNESS_EXCEPTION vector="); hex(vector); print(" esr="); hex(esr);
    print(" elr="); hex(elr); print(" spsr="); hex(spsr); print("\n");
}
void test_main(void)
{
    uint64_t el, mpidr;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(mpidr));
    print("PLATFORM QEMU virt; synthetic J713 request\nQEMU_TCG_START EL="); hex(el); print("\n");
    if(el!=8) test_exit(97);
#ifdef E2_TEST_NATIVE
    if(en_protect_arena(ARENA+1)==0 || en_protect_arena(UINT64_C(0x42000000))==0) { print("NATIVE_BAD_MAPPING_ACCEPTED\n");test_exit(93); }
    if(en_protect_arena(ARENA)) { print("NATIVE_MAPPING_REFUSED\n");test_exit(96); }
    uint64_t par;
    __asm__ volatile("at s1e2w, %0; isb"::"r"(ARENA):"memory");
    __asm__ volatile("mrs %0, PAR_EL1":"=r"(par));
    if(!(par&1)) { print("CODE_STILL_WRITABLE\n");test_exit(95); }
    __asm__ volatile("at s1e2w, %0; isb"::"r"(ARENA+E2_PAGE_SIZE):"memory");
    __asm__ volatile("mrs %0, PAR_EL1":"=r"(par));
    if(par&1) { print("DATA_NOT_WRITABLE\n");test_exit(94); }
    __asm__ volatile("msr PAR_EL1, %0; isb"::"r"(UINT64_C(0xabcd0000)):"memory");
    print("NATIVE_MAPPING_RX_RW_PASS\n");
#endif
    put32(request,E2_REQUEST_MAGIC); put32(request+4,E2_HEADER_SIZE<<16|E2_VERSION);
    put32(request+8,sizeof(request)); put32(request+12,E2_PREFLIGHT); put32(request+16,E2_CHIP);
    request[20]='J'; request[21]='7'; request[22]='1'; request[23]='3';
    put32(request+24,E2_REQUIRED_FLAGS); put64(request+32,ARENA); put64(request+40,E2_ARENA_SIZE);
    put32(request+48,1); put32(request+56,1); put64(request+64,mpidr);
    put64(request+80,0x40000000); put64(request+88,0x8000000);
    put64(request+96,0x40400000); put64(request+104,0x400000);
#if defined(E2_TEST_NATIVE) && E2_TEST_GUEST_EXPECTED == 22
    /* Exercise mask restoration, rather than only the already-masked case. */
    __asm__ volatile("msr daifclr, #2; isb" ::: "memory");
#endif
    unsigned failed=0;
#ifdef E2_TEST_NATIVE
    extern void test_int_result(void);
    int int_ret=test_call((uint64_t)test_int_result,request,sizeof(request),result,sizeof(result));
    if(int_ret || el2_native_call_state.ret) failed++;
    print(failed ? "INT_RETURN_FAIL\n" : "INT_RETURN_PASS\n");
#endif
    for(unsigned i=0;i<sizeof(result);i++) result[i]=0xa5;
    int short_ret=test_call(ARENA,request,sizeof(request),result,E2_RESULT_SIZE-1);
    if(short_ret!=E2_ARGUMENT) failed++;
    for(unsigned i=0;i<sizeof(result);i++) if(result[i]!=0xa5) { failed++; break; }
    print(failed ? "SHORT_CAPACITY_FAIL\n" : "SHORT_CAPACITY_PASS\n");
    failed+=one("preflight",0,E2_READY);
    failed+=one("brk",E2_TEST_BRK_ENTRY,E2_TEST_BRK_EXPECTED);
    failed+=one("guest",E2_TEST_GUEST_ENTRY,E2_TEST_GUEST_EXPECTED);
    print(failed ? "QEMU_TCG_FAIL\n" : "QEMU_TCG_PASS\n");
    test_exit(failed ? 1u : 0u);
}
