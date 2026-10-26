/* SPDX-License-Identifier: GPL-2.0-only */
/* Prototype adapter: reads FDT and existing m1n1 software state. No MMIO,
 * new sysreg access, firmware commands, GPIO operations or allocation. */
#include "m1n1_adapter.h"
#include "xboot.h"
#include "xboot_record.h"
#include "kboot.h"
#include "smp.h"
#include "string.h"
#include "utils.h"
#include "libfdt/libfdt.h"

static struct xb_session session;
static struct xb_facts facts;
static struct xb_range protected_memory;
static u64 recorded_release[MAX_CPUS], recorded_mpidr[MAX_CPUS];
static bool attempted;
static void *armed_kernel, *armed_dt;
static void *record_dt;
static size_t record_capacity;
static struct xb_record_meta record_meta;
#define INPUT_DT_LIMIT 0x200000

/* Packing moves properties. Find the slot each time, and only write its fixed
 * payload. The captured DT allocation is cleared before a retry can free it. */
static int publish_record(void)
{
    uint8_t bytes[XB_RECORD_BYTES];
    if (!record_dt || fdt_check_full(record_dt, record_capacity) ||
        xb_encode_record(&session, &record_meta, bytes, sizeof(bytes)))
        return -1;
    int node = fdt_path_offset(record_dt, "/chosen");
    if (node < 0)
        return -1;
    return fdt_setprop_inplace(record_dt, node, XB_RECORD_PROPERTY, bytes, sizeof(bytes));
}

static int reject(enum xb_error error)
{
    next_stage.entry = NULL;
    xb_fail(&session, error);
    /* A broken/missing slot must not replace the original failure cause. */
    if (record_dt)
        publish_record();
    printf("XBOOT: reject phase=%u first_error=%u events=%lu\n",
           (unsigned)session.phase, (unsigned)session.error, session.trace_count);
    return -1;
}

int xboot_input_ok(const void *fdt)
{
    return chip_id == xb_j713.chip && fdt && !fdt_check_header(fdt) &&
           fdt_totalsize(fdt) <= INPUT_DT_LIMIT &&
           !fdt_check_full(fdt, fdt_totalsize(fdt)) &&
           !fdt_node_check_compatible(fdt, 0, "apple,j713") &&
           !fdt_node_check_compatible(fdt, 0, "apple,t8132");
}

int xboot_begin_dt(const void *fdt)
{
    next_stage.entry = NULL;
    attempted = true;
    armed_kernel = armed_dt = NULL;
    record_dt = NULL;
    record_capacity = 0;
    record_meta = (struct xb_record_meta){.flags = XB_RECORD_METADATA_ONLY};
    protected_memory = (struct xb_range){0};
    for (int i = 0; i < MAX_CPUS; i++)
        recorded_release[i] = recorded_mpidr[i] = 0;
    xb_reset(&session);
    if (!fdt || fdt_check_header(fdt) || fdt_totalsize(fdt) > INPUT_DT_LIMIT ||
        fdt_check_full(fdt, fdt_totalsize(fdt)))
        return reject(XB_E_DT);
    if (xb_begin(&session, &xb_j713, chip_id,
                 !fdt_node_check_compatible(fdt, 0, "apple,j713") &&
                 !fdt_node_check_compatible(fdt, 0, "apple,t8132"), 1))
        return reject(session.error);
    printf("XBOOT: J713 preflight accepted; declared policy MTP-DAPF=Linux DART=inherited-unverified\n");
    record_meta.chip = chip_id;
    return 0;
}

int xboot_prepare_record(void *dt, size_t capacity)
{
    uint8_t bytes[XB_RECORD_BYTES];
    if (!attempted || session.phase != XB_PREFLIGHT || !dt ||
        fdt_check_full(dt, capacity) ||
        xb_encode_record(&session, &record_meta, bytes, sizeof(bytes)))
        return reject(XB_E_DT);
    int node = fdt_path_offset(dt, "/chosen");
    if (node < 0 || fdt_setprop(dt, node, XB_RECORD_PROPERTY, bytes, sizeof(bytes)))
        return reject(XB_E_RECORD);
    record_dt = dt;
    record_capacity = capacity;
    return 0;
}

void xboot_capture_ctrr(u64 start, u64 size)
{
    /* Called with the values already read by dt_set_memory; no repeat sysreg read. */
    protected_memory = (struct xb_range){start, size};
}

void xboot_capture_release(int cpu, u64 mpidr, u64 release)
{
    /* smp_get_release_addr mutates spin-table args. Do not call it from a checker. */
    if (cpu >= 0 && cpu < MAX_CPUS) {
        recorded_mpidr[cpu] = mpidr;
        recorded_release[cpu] = release;
    }
}

int xboot_dt_prepared(void)
{
    if (xb_dt_prepared(&session))
        return reject(session.error);
    if (publish_record())
        return reject(XB_E_RECORD);
    return 0;
}

void xboot_dt_failed(int result)
{
    printf("XBOOT: DT preparation failed upstream_result=%d\n", result);
    reject(XB_E_DT); /* Preserve an earlier preflight failure, if any. */
}

static int read_u64_prop(const void *dt, int node, const char *name, u64 *value)
{
    int len;
    const fdt64_t *p = fdt_getprop(dt, node, name, &len);
    if (!p || len != 8)
        return -1;
    *value = fdt64_ld(p);
    return 0;
}

static int collect(void *kernel, void *dt, size_t capacity, void *initrd, size_t initrd_size)
{
    facts = (struct xb_facts){0};
    facts.chip = chip_id;
    facts.compatible = xboot_input_ok(dt);
    if (!dt || !capacity || fdt_check_full(dt, capacity) ||
        fdt_address_cells(dt, 0) != 2 || fdt_size_cells(dt, 0) != 2)
        return -1;
    facts.fdt_valid = 1;
    facts.dt = (struct xb_range){(u64)dt, capacity};
    facts.loader = (struct xb_range){(u64)_base, (u64)_end - (u64)_base};
    facts.initrd = (struct xb_range){(u64)initrd, initrd_size};
    facts.protected_memory = protected_memory;
    int node = fdt_path_offset(dt, "/memory"), len;
    const fdt64_t *reg = fdt_getprop(dt, node, "reg", &len);
    if (!reg || len <= 0 || len % 16 || len / 16 > XB_MAX_RANGES)
        return -1;
    facts.ram_count = len / 16;
    for (size_t i = 0; i < facts.ram_count; i++)
        facts.ram[i] = (struct xb_range){fdt64_ld(reg + 2 * i), fdt64_ld(reg + 2 * i + 1)};
    int count = fdt_num_mem_rsv(dt);
    if (count < 0 || count > XB_MAX_RANGES)
        return -1;
    facts.reserved_count = count;
    for (int i = 0; i < count; i++)
        if (fdt_get_mem_rsv(dt, i, &facts.reserved[i].start, &facts.reserved[i].size))
            return -1;
    int reserved = fdt_path_offset(dt, "/reserved-memory");
    if (reserved >= 0) {
        if (fdt_address_cells(dt, reserved) != 2 || fdt_size_cells(dt, reserved) != 2)
            return -1;
        const void *ranges = fdt_getprop(dt, reserved, "ranges", &len);
        if (!ranges || len != 0)
            return -1; /* Do not guess translations for a non-identity bus. */
        fdt_for_each_subnode(node, dt, reserved) {
            const char *status = fdt_getprop(dt, node, "status", &len);
            if (status && len == 9 && !memcmp(status, "disabled", 9))
                continue;
            if (status && (len != 5 || memcmp(status, "okay", 5)))
                return -1;
            reg = fdt_getprop(dt, node, "reg", &len);
            if (!reg) {
                if (len != -FDT_ERR_NOTFOUND)
                    return -1;
                continue; /* Dynamic size-only nodes are a later Linux allocation. */
            }
            if (len <= 0 || len % 16)
                return -1;
            for (int i = 0; i < len / 16; i++) {
                if (facts.firmware_reserved_count == XB_MAX_RANGES)
                    return -1;
                facts.firmware_reserved[facts.firmware_reserved_count++] =
                    (struct xb_range){fdt64_ld(reg + 2 * i), fdt64_ld(reg + 2 * i + 1)};
            }
        }
        if (node != -FDT_ERR_NOTFOUND)
            return -1;
    } else if (reserved != -FDT_ERR_NOTFOUND) {
        return -1;
    }
    /* Check that a complete Image header lies in declared RAM before reading it.
     * Allocation validity is still inherited from the upstream payload loader. */
    bool header_in_ram = false;
    for (size_t i = 0; i < facts.ram_count; i++)
        if (xb_contains(facts.ram[i], (struct xb_range){(u64)kernel, sizeof(struct kernel_header)}))
            header_in_ram = true;
    if (!header_in_ram)
        return -1;
    const struct kernel_header *image = kernel;
    facts.image_magic_valid = image->magic == 0x644d5241 && image->image_size >= sizeof(*image);
    facts.image = (struct xb_range){(u64)kernel, image->image_size};
    node = fdt_path_offset(dt, "/chosen");
    u64 start, end;
    if (read_u64_prop(dt, node, "linux,initrd-start", &start) ||
        read_u64_prop(dt, node, "linux,initrd-end", &end) ||
        !xb_range_valid(facts.initrd) || start != facts.initrd.start ||
        end != facts.initrd.start + facts.initrd.size)
        return -1;
    int cpus = fdt_path_offset(dt, "/cpus");
    if (cpus < 0 || fdt_address_cells(dt, cpus) != 2 ||
        boot_cpu_idx < 0 || boot_cpu_idx >= MAX_CPUS)
        return -1;
    fdt_for_each_subnode(node, dt, cpus) {
        const char *name = fdt_get_name(dt, node, NULL);
        if (!name || strncmp(name, "cpu@", 4))
            continue;
        const char *status = fdt_getprop(dt, node, "status", &len);
        if (status && len == 9 && !memcmp(status, "disabled", 9))
            continue;
        if (status && (len != 5 || memcmp(status, "okay", 5)))
            return -1;
        if (facts.cpu_count >= XB_MAX_CPUS)
            return -1;
        struct xb_cpu *c = &facts.cpu[facts.cpu_count++];
        if (read_u64_prop(dt, node, "reg", &c->mpidr))
            return -1;
        int idx;
        for (idx = 0; idx < MAX_CPUS; idx++)
            if ((idx == boot_cpu_idx || smp_is_alive(idx)) && smp_get_mpidr(idx) == c->mpidr)
                break;
        if (idx == MAX_CPUS)
            return -1;
        c->boot = idx == boot_cpu_idx;
        c->alive = c->boot || smp_is_alive(idx);
        if (!c->boot) {
            if (read_u64_prop(dt, node, "cpu-release-addr", &c->release))
                return -1;
            c->release_matches = recorded_release[idx] == c->release &&
                                 recorded_mpidr[idx] == c->mpidr && c->release;
            c->stack = (struct xb_range){(u64)secondary_stacks[idx], SECONDARY_STACK_SIZE};
        }
    }
    if (node != -FDT_ERR_NOTFOUND)
        return -1;
    facts.mtp_dapf_owner = XB_OWNER_LINUX;
    facts.dart_owner = XB_OWNER_INHERITED_UNVERIFIED;
    return 0;
}

int xboot_check_handoff(void *kernel, void *dt, size_t capacity, void *initrd, size_t size)
{
    if (!attempted || (session.phase != XB_DT_PREPARED && session.phase != XB_CHECKED))
        return reject(XB_E_PHASE);
    if (collect(kernel, dt, capacity, initrd, size))
        return reject(XB_E_ADAPTER);
    if (xb_check(&session, &xb_j713, &facts))
        return reject(session.error);
    if (record_dt != dt || record_capacity != capacity)
        return reject(XB_E_RECORD);
    /* Only summarize a successful check; a later failed collect cannot publish
     * partially checked counts as though they were verified. */
    record_meta.cpu_count = facts.cpu_count;
    record_meta.ram_count = facts.ram_count;
    record_meta.reserved_count = facts.reserved_count;
    record_meta.firmware_reserved_count = facts.firmware_reserved_count;
    record_meta.mtp_dapf_owner = facts.mtp_dapf_owner;
    record_meta.dart_owner = facts.dart_owner;
    if (record_meta.checks == UINT32_MAX)
        return reject(XB_E_CAPACITY);
    record_meta.checks++;
    if (publish_record())
        return reject(XB_E_RECORD);
    printf("XBOOT: handoff metadata checked; CPUs=%lu reservations=%lu\n",
           facts.cpu_count, facts.reserved_count);
    return 0;
}

int xboot_arm_handoff(void *kernel, void *dt)
{
    if (xb_arm(&session))
        return reject(session.error);
    if (publish_record())
        return reject(XB_E_RECORD);
    armed_kernel = kernel;
    armed_dt = dt;
    return 0;
}

void xboot_proxy_override(void)
{
    /* Explicit P_VECTOR is an existing privileged operator escape, not an
     * automatic fallback. Keep the failure trace and print the bypass. */
    record_meta.flags |= XB_RECORD_PROXY_BYPASS;
    if (record_dt)
        publish_record();
    record_dt = NULL;
    record_capacity = 0;
    printf("XBOOT: explicit privileged proxy vector; contract bypassed phase=%u error=%u\n",
           (unsigned)session.phase, (unsigned)session.error);
    attempted = false;
    armed_kernel = armed_dt = NULL;
    next_stage.entry = NULL;
}

int xboot_vector_allowed(void *kernel, u64 dt, u64 arg1, u64 arg2, u64 arg3, u64 arg4)
{
    if (!attempted)
        return 1; /* Stage1 chainload / raw privileged proxy is outside this prototype. */
    if (kernel != armed_kernel || (void *)dt != armed_dt || arg1 || arg2 || arg3 || arg4 ||
        xb_consume(&session)) {
        reject(XB_E_PHASE);
        return 0;
    }
    if (publish_record()) {
        reject(XB_E_RECORD);
        return 0;
    }
    printf("XBOOT: branch authorized once; events=%lu\n", session.trace_count);
    return 1;
}
