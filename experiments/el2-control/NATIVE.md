# Native adapter: stage 2 calls the EL2 module

The adapter exists only in a separate development build of stage 2, made
with `CFG="FB_SILENT_MODE SIXBOOT_USER SIXBOOT_EL2_NATIVE"`. An ordinary
build does not link the native objects; `tools/build_native.py --control`
builds one next to it to check this. None of these tools change the kernel,
the initramfs or any disk.

## Entry and ownership

`payload_run()` passes the address and size of the ARM64 Image to the
adapter. `dt_set_memory()` records the CTRR range it already reads; no new
Apple register access is added. `entry_probe_run()` runs the P3 probe as
before, copies its result into the DT and frees the probe memory.
`el2_native_run(dt)` is called only if the P3 return, magic and status are
good. The hook runs while m1n1's MMU and exception setup are still up,
before `xboot_prepare_record()` and the final kernel handoff checks.

The arena is not free-looking RAM picked at random. The loader's BSS holds
192 KiB of static storage, and at run time a 64 KiB-aligned 128 KiB arena is
chosen inside it. The whole loader is already kept from Linux by its FDT
memreserve entry. The 16 KiB **native caller stack** is a separate BSS
object. P3's old 16 KiB stack lives inside its old 32 KiB heap object and is
not used by the native call.

The arena's 16 KiB slots: RX code `0`, session `0x4000`, EL2 stack `0x8000`,
reserved guest data `0xc000`, EL1 stack `0x10000`, trace `0x14000`, two
reserved slots from `0x18000`. The guest code and vectors are in the first
RX slot too. The caller's request, result and outer observer stay outside
the arena. The real load address is not assumed at build time; it is written
to the DT and the console.

`en_fdt_memory()` checks RAM coverage, that the loader and the kernel do not
overlap, the DT and initrd spans, the FDT reservation list and the
`/reserved-memory` nodes. Disabled reservations are kept too. The full
loader reservation and the CTRR reservation recorded earlier are the only
exceptions it accepts, and only for BSS the loader owns. AP stacks are
compared separately. This is not DART/DMA isolation: it relies on the static
loader object and the existing allocator and reservation rules, and it does
not stop undeclared DMA from other firmware.

It requires chip T8132, FDT J713/T8132, the boot CPU, a 16 KiB granule, EL2h
with SPSel=1, HCR E2H/TGE/RW set and VM clear, PAN support and GXF off. There
is no path that enables GXF, enters GL2 or changes Apple control registers.

## Page preparation

All 32 4 KiB subpages are checked with `AT S1E2R/W`: VA must equal PA and
reads must be Normal-WB (attribute `0xff`). PAR and the masks are kept. The
code is copied and the caches are made coherent. `en_protect_arena()`
accepts only m1n1's existing 48-bit VA, 16 KiB, four-level L3 **page**
mappings. Splitting blocks or tables, new table allocations, the contiguous
hint, DBM, LPA2/DS and hardware AF/dirty management are not supported; each
is refused rather than adapted.

The eight L3 descriptors are checked first and all made invalid; after
DSB / TLBI ALLE2IS / DSB / ISB the new descriptors are written, followed by
DSB / ISB. Output address, memory attributes and shareability are kept; the
code page becomes RX and the other seven pages RW/NX. The existing
`mmu_add_mapping()`, which splits blocks, is not used here. The last AT
check also requires that a code write faults and a data write succeeds.

These permissions stay on this loader object after the module returns; it
is not made RWX again until m1n1's normal MMU shutdown. Equal TTBR/TCR values
in the outer observer do not mean that all page-table memory kept its old
bytes. Alias mappings are not turned into a general security boundary.
Because the EL1 guest's MMU and Stage-2 translation are off, the payload is
trusted test code.

## Call and return

`/chosen/6boot,el2-mode` must be a NUL-terminated `preflight`, `brk` or
`guest`. Entry offsets come from the built module ELF. A command-line tag
alone does not enable a mode; both the native build and the DT property are
needed.

`el2_native_invoke()` first saves the caller's x18..x30 and frame base. It
saves and masks DAIF and moves to the separate native stack. It fills
x18..x29 with `0x712..0x71d` and then calls the module. On return it compares
those 12 registers, SP and 26 context values: VBAR/HCR/ELR/SPSR/ESR/FAR_EL2,
SP_EL0/SP_EL1, SCTLR/VBAR/ELR/SPSR_EL12, PAR, DAIF, SPSel, PAN,
SCTLR/TCR/TTBR0/TTBR1/MAIR_EL2, MPIDR, CurrentEL and the private SP, followed by ESR_EL12 and FAR_EL12.
PAN remains element 15 (byte 120); the new banks are appended at 24/25.

This does not claim to observe every architectural register the module
leaves alone, or any microarchitectural state. No SIMD is used; these
objects are built with `-mgeneral-regs-only`. Restoring x30 and the original
caller frame is checked by the gate's normal RET, and again by the outer
AAPCS wrapper in QEMU.

EN flags: CONTEXT=1, GPRS=2, SP=4, RETURNED=8, DAIF=16; a full result is
`0x1f`. Status 19/20, a missing context/GPR/SP check or an unsafe record
gives `EN_FATAL`. PAN is put back to its old value for diagnosis; after a
critical failure the interrupt masks are not left open. A failed HCR/EL12
restore is not retried from the outer gate.

`EN_FATAL` closes the kernel handoff latch. Both `kboot_prepare_dt()` and
`kboot_boot()` / the final handoff check apply it, and `next_stage.entry` is
cleared. A refused experiment, or a semantic error with the outer context
and core restore fully checked, may continue to Linux so the records can be
collected; neither counts as a PASS. This is not a guarantee against every
failure: an invalid first stack, an EL3/GXF trap, a broken previous vector or
unreachable RAM can still stop the machine. After a fatal result the Linux
collector cannot run; use the serial console or a photo.

## Records

DT scalar fields are big-endian; the request, result and the wire records
below are little-endian.

| Record | Size | Content |
| --- | ---: | --- |
| `6boot,el2-request` | variable, at most 592 | E2CT v1; the RAM list and two exclusions covering every address outside the arena |
| `6boot,el2-result` | 160 / 2048 | preflight E2RS v2 or exception E2EX v3; copied bytes, not an arena pointer |
| `6boot,el2-native-state` | 624 | E2NA v2; mode/status/called/flags/ret, 26 u64 before and after, 12 GPRs, stack/DAIF fields |
| `6boot,el2-ownership` | 176 | E2OW v1; loader/BSS/storage/arena/caller stack/request/result/observer/kernel/CTRR spans |
| `6boot,el2-mappings` | 1056 | E2MP v1; read/write PAR before and after preparation for the 32 subpages |

In E2NA, `before` is at offset 64, `after` at 272 and the GPRs at 480; the
tail starts at 576: observed SP, expected SP, ret, the original **saved
frame base**, flags and the original DAIF. The saved frame base is 112 bytes
below the SP before the call.

Scalar fields: adapter-status (0 PASS, 1 FAILED, 2 REFUSED, 3 FATAL),
refusal-reason, called, return, safe, load-address and ticks, plus the
build-id and module-sha256 strings. Mapping flags: pre-attempt=1,
pre-pass=2, post-attempt=4, post-pass=8; 15 when complete. After a refusal
the counters show where the first failing subpage is. If no call was made,
the gate snapshot is empty and must not be read as a register measurement.

Main refusal reasons: 1 mode, 2 platform/CPU/EL/granule, 3 host HCR/PAN/GXF,
4 arena ownership/code size, 5 AP stack, 6 first AT, 7 L3 protection or last
AT, 8 request DT record. `100+n` is `en_fdt_memory()` error n: 101
incompatible input, 102 memory reg, 103 bad/overlapping RAM, 104 coverage,
105 DT overlap, 106 initrd, 107 reservation format, 108 reservation overlaps
the arena, 109 reserved-memory parent, 110 child node format, 111 reserved
span, 112 DT traversal error. A failure to publish to the DT blocks the
kernel handoff.

## Rebuilding

GNU make, AArch64 ELF binutils, clang targeting AArch64, Rust/Cargo and
`aarch64-unknown-none-softfloat` are needed. The tools work in a new scratch
directory and do not change the source tree; there is no install or boot
command.

```sh
python3 -I tools/build_native.py --work /absolute/new/scratch --make gmake --control
python3 -I tools/audit_native.py --build /absolute/new/scratch
```

Run these from `experiments/el2-control/`. `--cc`, `--ld`, `--objcopy` and
`--nm` select a Linux cross toolchain. The build that ran on J713 was made on
macOS; a Lima build has not been tried yet. `assemble_native.py` builds the
three images only from REL3 inputs copied beforehand and the host libfdt,
and refuses to produce a new image if it cannot first rebuild the original
byte for byte.

The Arm break-before-make order and the descriptor reading follow the
existing `src/memory.c`, `src/arm_cpu_regs.h` and [Arm AArch64 memory management, TLBs and Changing translations](https://documentation-service.arm.com/static/68e680a2587fd957f5efd742?token=).
That reference describes the architecture; it is not a J713 verification.

Legacy E2NA v1/592 (24 values), E2RS v1/128 and E2EX v2/2048 remain
readable; old captures do not gain measurements they never recorded.
The adapter preserves framebuffer activation, including FB_SILENT_MODE.
