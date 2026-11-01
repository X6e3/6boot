# E0 two-world microguest experiment

2026-10-30. Diskless, single-PE QEMU prototype. **Not a J713 boot image.**
Native preflight/BRK/HVC, their wire formats, and the production Makefile are
unchanged. No Linux/XNU guest, resident monitor, watchdog or DMA service.

## Entry and lifetime

Build in a fresh directory with `make WORLDS=1 BUILD=/absolute/scratch all`.
The same `sixboot_e0_run(mode)` gate, saved host block, shared vectors and
fail-closed restore are used. Without `WORLDS=1`, modes 5–7 remain refused.

| Mode | Execution |
| --- | --- |
| 5 PREPARE | Build/audit both domains; seed BRK; no guest entry, no test VTTBR/VTCR programming, no VM=1 |
| 6 SINGLE | A yield → A restore → RO-write fault → foreign-address fault → DONE → host |
| 7 DUAL | A yield → B yield/self-restore → B faults/DONE → A restore → A faults/DONE → host |

QEMU's harness explicitly exercises 5,6,7,6,7. This sequence is **not** a
physical activation policy. A J713 loader does not exist for these modes;
there is no native Gate D activation, regardless of emulator success.

Two fixed slots replace a dynamic allocator. `ew_build()` creates each root;
`ew_validate()` checks every descriptor, including invalid entries;
`ew_pair()` rejects shared VMIDs and physical pages. `select_world()` enters
or switches; exception dispatch saves/yields/faults/terminates a world.
`ew_restore_host()` retires both table slots only after VM=0 and the final
test-context TLBI. Private stack/data/RO pages are cleared before reuse.
Saved contexts and bounded trace remain available until the next invocation.

## Owned arena and mappings

Existing arena: 256 KiB at QEMU PA/EL2 VA `0x40800000`, alignment 64 KiB.
No memory allocation or device access occurs in the handler.

| Offset | Owner/use | Guest IPA |
| --- | --- | --- |
| 00000 | EL2 code/vectors | unmapped |
| 04000 | A code/vector page | 10000000, RO executable |
| 08000 | EL2 state/trace | unmapped |
| 0c000 | EL2 stack, 16 KiB | unmapped |
| 10000,14000 | A L2/L3 | unmapped |
| 18000 | A stack | 10014000, RW XN; adjacent IPA pages unmapped |
| 1c000 | A data | 10008000, RW XN |
| 20000 | A RO data | 1000c000, RO XN |
| 24000,28000 | B L2/L3 | unmapped |
| 2c000 | B stack | 10014000, RW XN; adjacent IPA pages unmapped |
| 30000 | B data | 10008000, RW XN |
| 34000 | B RO data | 1000c000, RO XN |
| 38000 | B code/vector copy | 10000000, RO executable |
| 3c000 | final reserve/guard | unmapped |

EL1 stacks are each 16 KiB, SP aligned 16 bytes. EL2 has its existing separate
16 KiB stack. P3's separate native caller stack is not part of this ELF.
Linker assertions bound code, vectors, trace/state and stacks.
This QEMU ownership proof does **not** reserve any J713 RAM.

Granule 16 KiB, T0SZ=32, SL0=1 (L2), DS=0, VS=0. One L3 covers the fixed
32 MiB window beginning at IPA 0x10000000; other windows are rejected by
the leaf mapper. A 128-entry effective root is stored in a zeroed 16 KiB page.
Low IPA/high PA mappings are host-tested. PA width is separately checked
against MMFR0.PARange; DS=0 caps output at 48 bits. No live table update,
block descriptor, LPA2, HA/HD, permission overlay or FWB encoding is enabled.

VMIDs 0x7e/0x7f belong exclusively to this single-PE emulator. The current
inherited VMID must differ. This is not global VMID reservation on J713.
Each selection clears VM/TGE, flushes a previously active test VMID, writes
VTCR/VTTBR with ISB/readback, flushes the selected VMID with DSB/TLBI/DSB/ISB,
loads its EL1 bank, then enables VM with ISB/readback. E2H stays set. Final
assembly restore disables VM and flushes the final test VMID. The C helper
loads/checks the host EL1 bank, then retires tables. Assembly subsequently
checks all original control-register readbacks.

Stage1 M/C/I are off. HCR.DC/FWB/NV are refused; the handler verifies the
actual HCR and SCTLR before interpreting a guest fault. Stage2 Normal-WB
descriptors alone do not imply effective cached guest data. Preparation
CIVACs owned RAM and publishes code; the observer invalidates guest-written
aliases before reading them. QEMU does not prove physical M4 coherency.

## CPU and exception contract

All x0–x30, exception PC/PSTATE and these EL1 bank values are world-specific:
SP_EL0/SP_EL1, SCTLR, TCR, TTBR0/TTBR1, MAIR/AMAIR, VBAR, ELR/SPSR,
ESR/FAR, CONTEXTIDR, TPIDR_EL1/EL0/RO_EL0. Under VHE the banked EL1 controls
use their `_EL12` accessors. The payload checks all 31 GPR patterns after
yield, then its private signature, stack and RO canary. DONE additionally
compares the actual bank to the saved bank. The outer observer compares 28
EL1/EL2 system values plus callee-saved GPRs and SP to the original caller.

These integer-only, interrupt-masked programs do not touch SIMD/SVE/SME,
debug, PMU, PAC, EL0 execution or timers. Such state is **not** a complete
general-purpose guest context. There is no claim of side-channel isolation.

HVC ELR already identifies the following instruction; it is not incremented.
Fault recovery accepts only the exact STR PC, address, EC/IL/DFSC/write
syndrome and phase, then selects its named recovery label. Ordinary RO-write
permission faults do not provide valid HPFAR: FAR=IPA is used only under
the verified Stage1-off contract. The numeric other-world PA-as-IPA probe
must instead fault at L2 and validates HPFAR FIPA[47:12]. The fixed probe
requires a sub-4-GiB QEMU arena; a high-PA native port needs another probe,
not an automatic increase in IPA width. FnV, S1PTW, wrong PC/address/class,
missing faults and unexpected HVCs cannot pass the policy.

Unexpected guest events exit through full restoration, status 2. Failed
restore readback records status 3; restore/switch exceptions or lost context
record 4 and park at `e0_terminal_park`. No ordinary C return is allowed
after those fatal conditions. Masked DAIF, owned EL2 mappings and stack,
and delivery through EL2 remain prerequisites. Destroyed EL2 mappings,
higher-level guarded traps, and faults after final inherited VBAR commit
are outside the recoverability claim. Terminal trace export on J713 does
not yet exist. A terminal stop would require external/manual recovery.

## E0W1 wire format

Separate from E2RS/E2EX/E2NA. Fixed 1856 bytes; all scalars unsigned LE64.

| Offset | Content |
| --- | --- |
| 0 | 16-word header: magic 0x31573045, version1, size1856, source build ID, mode, status, host_restored, selection-TLBI count, event count, refusal, MMFR0, MMFR1, MMFR2, PFR0, incoming HCR, outer observer_ok |
| 128 | A domain, 12 words |
| 224 | B domain, 12 words |
| 320 | 12 event slots, 128 bytes each; unused slots zero |

Domain words: VMID, root PA, VTCR, code PA, stack PA, data PA, RO PA,
signature, completion, context_ok, observed data, observed RO canary.

Event words: world, kind, VMID, full VTTBR readback, HCR, VTCR, ESR, ELR,
SPSR, FAR, raw HPFAR, derived IPA, address-validity flags, expected,
context_ok, selection-TLBI count. Kinds: yield1, permission2, foreign3,
done4, unexpected5. Address-validity bit0=FAR, bit1=HPFAR, bit2=derived IPA.
Permission uses flags5, translation uses7; HVC/unexpected use0 and IPA0.
Selection-TLBI counts exclude the final assembly cleanup TLBI. They record
issued operations, not a physical measurement of a TLB's contents.

Refusals: 1 capability/layout/mode; 2 emergency exception/overflow;
3 invalid domain/pair; 4 control readback; 5 bank readback;
6 inherited VMID collision; 7 unsupported numeric foreign-address probe.
Restore status3/4 is recorded even when a normal result cannot be exported.

`worlds_decode.py` independently validates exact event sequence, PC/syndrome,
domains, validity, memory observations and outer restoration. Expected PCs
and build ID come from the separately hashed ELF/build header, not the trace.
`worlds_trace_test.py` mutates actual emulator records to test false-PASS
rejection. `worlds_run.py` saves binary records, JSON, command and serial log.

## Physical gates

Gate A: host sanitizers, static/ELF audit, mutations, emulator positive and
negative tests. These are software evidence only.

Gate B: **BLOCKED / PENDING PHYSICAL TEST**. First obtain the raw c625 E2RS
v2 register values. Prove a native 256 KiB reservation/exclusion set,
RX/RW mappings, cache contract, single-PE quiescence, VMID ownership and
guarded EL2 control authority. Implement/review a separate relocation and
native caller/trace adapter. The existing native 128 KiB arena/mapper is
insufficient and is deliberately unchanged. Do not load this QEMU ELF at
its link address on J713. No safe physical activation command exists yet.

Gate C: blocked by Gate B/native preparation plus physical VM=0 PASS.
Requires an independently selected single-world candidate, successful
allowed access, permission fault and full restoration, not merely Linux boot.

Gate D: blocked by Gate C **physical** PASS, then a separately reviewed
dual-world opt-in/receipt and native VMID/cache lifecycle. Emulator mode7
does not authorize native activation. A kernel panic or non-cooperative
hang is not detected by this bounded HVC-driven prototype.

Until these prerequisites exist, rollback is to leave the verified REL3
BOOT.BIN and ESP untouched. This package has no installer/activation script
and does not invent disk-writing commands for an unimplemented native port.

References: Arm HPFAR register description
https://documentation-service.arm.com/static/5e7e1405b471823cb9de57ae ;
Arm memory attribute combinations
https://documentation-service.arm.com/static/63a43e333f28e5456434e18b ;
Arm-authored generic register references (mirror):
https://df.lth.se/~getz/ARM/SysReg/AArch64-vtcr_el2.html ,
https://df.lth.se/~getz/ARM/SysReg/AArch64-id_aa64mmfr1_el1.html ,
https://df.lth.se/~getz/ARM/SysReg/AArch64-hcr_el2.html .
