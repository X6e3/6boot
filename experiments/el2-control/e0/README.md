# E0: controlled Stage-2 CPU access experiment

Base82b1826. Separate freestanding QEMU ELF; no production/native loader changes.
This runs actual AArch64 ERET, Stage-2 walks, exceptions and returns on QEMU TCG.
J713: **PENDING PHYSICAL TEST**. This is not persistent EL2, Linux guest boot,
DMA isolation, panic detection, a watchdog or guest-only restart.

The existing ../arch/vectors.S and its frame/stack ABI are linked unchanged.
The existing EL2/HVC module and its wire protocol remain unchanged. The E0 ELF
selects its own C dispatcher and a separate bounded session; there is no USB
host/proxy dependency in the experiment. QEMU semihosting provides test output
and exit only; it is not a device implementation for physical6boot.

## Experiment and evidence

Every run writes/reads0x1234 at allowed IPA0x10008000 and issues HVC#0x6e33.
It then executes one STR at a named PC, targeting IPA0x1000c000:

- ALLOW: that IPA maps a dedicated guard page. STR completes and guard becomes0xbad.
- DENY: its L3 descriptor is invalid. The same STR takes a lower-A64 data abort;
  guard stays0x1357. Require EC0x24, IL1, DFSC7, WnR1, S1PTW0, FnV0,
  exact FAR, reconstructed IPA from valid HPFAR and exact fault PC. Only then
  resume at the explicitly named instruction after STR. HVC returns are not
  incremented: their ELR already points after HVC.
- Wrong-address negative: STR targets denied IPA+8. Architectural HPFAR is valid,
  but this is not the expected access; return failure to the monitor.
- Missing-denial negative: deliberately map the guard while expecting a fault.
  The store succeeds, but completion is rejected because no fault was observed.

The QEMU harness repeats ALLOW/DENY/ALLOW/DENY using the same VMID, then the two
negative cases. Every normal/unexpected-guest return compares18 saved system
registers, caller x18–x29 and SP. The guest checks x19/x20 and its stack. Existing
BRK/HVC regressions separately check the original complete GPR protocol.

ESR/FAR/HPFAR/ELR/SPSR and GPRs are retained per exception. HPFAR validity is
recorded independently of whether the fault was expected. Raw FAR/HPFAR shown
for HVC or BRK are stale diagnostic values, not valid fault addresses. Abort
syndrome fields are decoded only for abort classes. One emergency record is
retained for nested/restore exceptions. Per-run records are reset before the
next test, after the harness exports them; no persistent crash journal exists.

## Geometry and ownership

Only QEMU virt owns/provides this RAM. Fixed link addresses describe this ELF,
not the physical J713 memory map. Entry also requires EL2h, masked DAIF, E2H=1,
VM=0, DC=0 and FWB=0; all arena pages must translate identically as Normal-WB.
These checks do not establish a native allocation or other-PE exclusivity.

Arena base0x40800000, size0x40000:

| Offset | Exclusive use | EL1 mapping |
|---|---|---|
|0x00000|EL2 code/vectors|None|
|0x04000|EL1 code/vectors|IPA0x10000000, RO executable|
|0x08000|EL2 state/trace|None|
|0x0c000|EL2 stack,16KiB|None|
|0x10000|Stage-2 L2 table,16KiB|None|
|0x14000|Stage-2 L3 table,16KiB|None|
|0x18000|EL1 stack,16KiB|IPA0x10004000, RW/XN|
|0x1c000|Allowed data,16KiB|IPA0x10008000, RW/XN|
|0x20000|Guard data,16KiB|Only ALLOW/missing-denial control|
|0x24000–0x40000|Reserved|None|

16KiB granule,32-bit IPA, start level2, no concatenated root tables. MMFR0
TGran16_2/fallback and PARange are checked; at most48-bit output format (DS=0).
VTCR specifies WB/inner-shareable walks, software AF, no HA/HD or FWB. Default
entries are invalid, so no device/MMIO or remaining host RAM is accessible.

VMID0x7f with8-bit VMID encoding belongs solely to this single-PE QEMU test.
It is not a safe globally reserved VMID on J713. Tables/code/data are published
before use, with exclusive data aliases cleaned/invalidated. VTTBR selects the
current test VMID; HCR.TGE is cleared before guest TLBI. DSB/TLBI VMALLS12E1IS/
DSB/ISB precede entry. Exit disables VM with TGE still0, flushes that context,
then restores VTCR/VTTBR and the saved EL1/EL2 register set. HCR and inherited
DAIF and finally VBAR are restored last. Readback mismatch records
RESTORE_FAILED and parks; it never returns a partial context. TLB contents
are invalidated, not snapshotted. Other-PE and silicon-specific speculative-walk
requirements are outside this single-PE prototype.

A trapped restore operation records CONTEXT_LOST without retrying writes.
Both fatal statuses park with masked DAIF at `e0_terminal_park`: no C call,
RET, ERET or firmware reset. The guest-return flag is cleared, and trace/saved
context remain in private RAM. This deliberately stops the experiment, not
recovers its caller. It assumes the owned EL2 mappings remain accessible.

The pre-post-review code returned to its caller after partial restoration;
the old closeout's error-code test did not establish a safe exit. That source,
test and raw failure evidence remain in the prior/incoming snapshots. The new
mutation test uses only a local QEMU GDB stub to observe terminal PC, status,
DAIF/SP, HCR/VBAR/VTCR/VTTBR, saved registers and emergency trace. It covers
skipped writes, false readback and restore exceptions, including the old DAIF
trap window. DAIF is now restored/checked while our vectors are installed.
An observed restore failure does not enter Linux or execute a loader C return.
The final VBAR commit transfers exception routing to the inherited vectors;
this does not promise interception of arbitrary instruction corruption after
that commit, loss of owned EL2 mappings, or Apple GXF/GL2 traps.

## Build/test

Requires Clang AArch64 target, aarch64-elf binutils and an existing QEMU runtime.
No network, disk, USB, firmware download or physical boot is performed:

```sh
make -C experiments/el2-control/e0 BUILD=/absolute/new/build \
  QEMU=/absolute/qemu-system-aarch64 QEMU_LIB_DIR=/absolute/runtime/lib qemu
```

RUN defaults to BUILD/qemu and must not already exist; choose a fresh RUN for a
repeat to retain previous failures. Host tests use ASan/UBSan. The ELF audit checks
layout, segment flags and exact VHE/TLBI sequences, and rejects seven mutations, including unsafe terminal/restore changes.
This structural check is not proof of a correct hardware TLB implementation.
The independent kernel KVM VHE reference is
arch/arm64/kvm/hyp/vhe/tlb.c:enter_vmid_context(), which clears TGE for guest TLB
maintenance. The first E0 draft did not do that; QEMU accepted it, so source
review corrected the order and a static guard was added.

The initial assembly build rejected oversized immediate operands; the next link
exposed aggregate-zeroing's libc memset dependency. Both logs are retained.
Explicit instruction forms and freestanding byte clears fix those build issues.
The final clean build and tests are separate from these failed attempts.

## Native prerequisites, not implemented

A physical E0 adapter needs a separately allocated/excluded arena large enough
for this layout; the old128KiB arena cannot be reused unchanged. Prove code,
tables, stacks, trace and firmware/DT/kernel/DMA disjointness, identity/PA width,
cache policy, quiesced other PEs and VMID ownership. Check M4 guarded permissions
on all new system-register writes and extend the outer native observer to include
VTCR/VTTBR/HPFAR and EL1 fault banks. Treat every partial restore as a handoff stop.
Build a separately reviewed candidate with independent REL3 recovery access.
No P3 connection, J713 boot image, ESP write or activation command is supplied.

Physical progression: allowed access first; then mapped-target control; then one
unmapped target; then return-context and ordinary Linux continuation. Preserve
raw logs with source/ELF/module/boot hashes. A QEMU PASS cannot promote these steps.

## 2026-10-30 review continuation

The original ALLOW/DENY/ALLOW/DENY, wrong-address and missing-denial controls
remain. A seventh mode (STATE_DENY=4) writes to the identity-looking IPA of
`e0_state_canary` in the actual EL2 state page. No mode maps that page.
Its root entry is invalid, so the expected data-abort DFSC is 6 (L2), not the
guard control's 7 (L3). Only the exact write address/PC/syndrome resumes at the
known next instruction. The monitor canary must remain unchanged. This proves
a CPU access denial in QEMU, not DMA isolation or a native J713 memory map.

The two pre-existing negative controls still deliberately report UNEXPECTED;
the restore-UDF injection still reports CONTEXT_LOST and is not boot-safe.

## Post-review address tests

The HPFAR/FAR formula is unchanged: for the ordinary STR, HPFAR[39:4] carries
IPA[47:12], including valid IPA[13:12] in HPFAR[5:4]. Host tests cover offsets
0, 0xfff, 0x1000, 0x2000 and 0x3fff, including inconsistent FAR/FIPA, FnV,
address-size, permission, S1PTW and PC negatives. MOPS is outside this contract;
the ELF audit fixes the expected fault site to the ordinary STR instruction.

Additional disposable QEMU copies use STRB solely to remove unaligned-wide-
access/page-crossing ambiguity. The debugger observes all five raw fault
addresses, then requires E0 to reject those unexpected targets and restore the
caller. The production STR, original STATE_DENY canary and original matrix
are unchanged. QEMU GDB observation does not validate Apple cache behavior.

T0SZ=32 describes IPA, not the host PA. Host tests map low IPAs to PAs above
4 GiB with the same geometry and reject insufficient PARange. A future native
STATE_DENY cannot blindly treat a high monitor PA as a 32-bit IPA: that would
cause an address-size fault. Choose/review its guest address separately; no
native loader, arena allocation or geometry expansion is implemented here.
