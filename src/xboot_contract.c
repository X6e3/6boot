/* SPDX-License-Identifier: GPL-2.0-only */
#include "xboot.h"

int xb_range_valid(struct xb_range r)
{
    return r.size && r.start <= UINT64_MAX - r.size;
}

int xb_contains(struct xb_range outer, struct xb_range inner)
{
    return xb_range_valid(outer) && xb_range_valid(inner) &&
           inner.start >= outer.start &&
           inner.start + inner.size <= outer.start + outer.size;
}

int xb_overlaps(struct xb_range a, struct xb_range b)
{
    return xb_range_valid(a) && xb_range_valid(b) &&
           a.start < b.start + b.size && b.start < a.start + a.size;
}

static int covered(const struct xb_range *ranges, size_t count, struct xb_range r)
{
    for (size_t i = 0; i < count; i++)
        if (xb_contains(ranges[i], r))
            return 1;
    return 0;
}

static void record(struct xb_session *s)
{
    if (s->trace_count < XB_TRACE_LEN)
        s->trace[s->trace_count++] = (struct xb_event){s->phase, s->error};
}

void xb_reset(struct xb_session *s)
{
    *s = (struct xb_session){0};
    record(s);
}

enum xb_error xb_fail(struct xb_session *s, enum xb_error error)
{
    /* Preserve the first cause. Phase transitions cannot clear a failure;
     * a new explicit prepare operation starts a session with xb_reset(). */
    if (s->phase != XB_FAILED) {
        s->phase = XB_FAILED;
        s->error = error == XB_OK ? XB_E_ADAPTER : error;
        record(s);
    }
    return s->error;
}

static enum xb_error step(struct xb_session *s, enum xb_phase from, enum xb_phase to)
{
    if (s->phase != from)
        return xb_fail(s, XB_E_PHASE);
    s->phase = to;
    record(s);
    return XB_OK;
}

enum xb_error xb_begin(struct xb_session *s, const struct xb_profile *p,
                       uint32_t chip, unsigned compatible, unsigned fdt_valid)
{
    if (!p || chip != p->chip || !compatible)
        return xb_fail(s, XB_E_PROFILE);
    if (!fdt_valid)
        return xb_fail(s, XB_E_DT);
    return step(s, XB_RESET, XB_PREFLIGHT);
}

enum xb_error xb_dt_prepared(struct xb_session *s)
{
    return step(s, XB_PREFLIGHT, XB_DT_PREPARED);
}

enum xb_error xb_validate(const struct xb_profile *p, const struct xb_facts *f)
{
    if (!p || !f || f->chip != p->chip || !f->compatible)
        return XB_E_PROFILE;
    if (!f->ram_count || f->ram_count > XB_MAX_RANGES ||
        f->reserved_count > XB_MAX_RANGES || f->cpu_count > XB_MAX_CPUS ||
        f->firmware_reserved_count > XB_MAX_RANGES)
        return XB_E_CAPACITY;
    for (size_t i = 0; i < f->ram_count; i++) {
        if (!xb_range_valid(f->ram[i]))
            return XB_E_RAM;
        for (size_t j = 0; j < i; j++)
            if (xb_overlaps(f->ram[i], f->ram[j]))
                return XB_E_RAM;
    }
    for (size_t i = 0; i < f->reserved_count; i++)
        if (!xb_range_valid(f->reserved[i]))
            return XB_E_RESERVED;
    for (size_t i = 0; i < f->firmware_reserved_count; i++)
        if (!xb_range_valid(f->firmware_reserved[i]))
            return XB_E_RESERVED;
    if (!p->image_align || !p->dt_align || !f->image_magic_valid ||
        f->image.start % p->image_align ||
        !covered(f->ram, f->ram_count, f->image))
        return XB_E_IMAGE;
    for (size_t i = 0; i < f->reserved_count; i++)
        if (xb_overlaps(f->image, f->reserved[i]))
            return XB_E_OVERLAP;
    for (size_t i = 0; i < f->firmware_reserved_count; i++)
        if (xb_overlaps(f->image, f->firmware_reserved[i]))
            return XB_E_OVERLAP;
    if (!f->fdt_valid || f->dt.start % p->dt_align ||
        !covered(f->ram, f->ram_count, f->dt))
        return XB_E_DT;
    /* These are required live allocations. The kernel itself must be usable RAM;
     * memreserve is deliberately not required for the kernel Image. */
    const struct xb_range required[] = {f->dt, f->loader, f->initrd, f->protected_memory};
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++)
        if (!covered(f->ram, f->ram_count, required[i]) ||
            !covered(f->reserved, f->reserved_count, required[i]))
            return XB_E_RESERVED;
    const struct xb_range distinct[] = {f->image, f->dt, f->loader, f->initrd};
    for (size_t i = 0; i < sizeof(distinct) / sizeof(distinct[0]); i++)
        for (size_t j = 0; j < i; j++)
            if (xb_overlaps(distinct[i], distinct[j]))
                return XB_E_OVERLAP;
    /* CTRR-protected pages may contain m1n1, but never the Linux Image or initrd. */
    if (xb_overlaps(f->protected_memory, f->image) ||
        xb_overlaps(f->protected_memory, f->initrd))
        return XB_E_OVERLAP;
    if (f->cpu_count != p->cpus)
        return XB_E_CPU;
    unsigned boot_count = 0;
    for (size_t i = 0; i < f->cpu_count; i++) {
        const struct xb_cpu *cpu = &f->cpu[i];
        if (!cpu->alive)
            return XB_E_CPU;
        if (cpu->boot) {
            boot_count++;
        } else {
            struct xb_range release = {cpu->release, 8};
            if (!cpu->alive || !cpu->release_matches || (cpu->release & 7) ||
                !covered(f->ram, f->ram_count, cpu->stack) ||
                !covered(f->reserved, f->reserved_count, cpu->stack) ||
                !xb_contains(f->loader, release))
                return XB_E_CPU;
            if (xb_overlaps(cpu->stack, f->image) || xb_overlaps(cpu->stack, f->dt) ||
                xb_overlaps(cpu->stack, f->initrd) || xb_overlaps(cpu->stack, f->loader) ||
                xb_overlaps(cpu->stack, f->protected_memory))
                return XB_E_OVERLAP;
        }
        for (size_t j = 0; j < i; j++) {
            if (cpu->mpidr == f->cpu[j].mpidr)
                return XB_E_CPU;
            if (!cpu->boot && !f->cpu[j].boot &&
                (cpu->release == f->cpu[j].release ||
                 xb_overlaps(cpu->stack, f->cpu[j].stack)))
                return XB_E_CPU;
        }
    }
    if (boot_count != 1)
        return XB_E_CPU;
    if (f->mtp_dapf_owner != p->mtp_dapf_owner || f->dart_owner != p->dart_owner)
        return XB_E_OWNER;
    return XB_OK;
}

enum xb_error xb_check(struct xb_session *s, const struct xb_profile *p,
                       const struct xb_facts *f)
{
    if (s->phase != XB_DT_PREPARED && s->phase != XB_CHECKED)
        return xb_fail(s, XB_E_PHASE);
    enum xb_error error = xb_validate(p, f);
    if (error != XB_OK)
        return xb_fail(s, error);
    if (s->phase == XB_DT_PREPARED)
        return step(s, XB_DT_PREPARED, XB_CHECKED);
    return XB_OK;
}

enum xb_error xb_arm(struct xb_session *s)
{
    return step(s, XB_CHECKED, XB_ARMED);
}

enum xb_error xb_consume(struct xb_session *s)
{
    return step(s, XB_ARMED, XB_CONSUMED);
}
