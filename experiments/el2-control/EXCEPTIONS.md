# EL2 vectors and controlled exception session

This increment extends the existing local foundation. It does not integrate
with src/entry_probe.c, root Makefile, the installer or the kernel boot path.
All code here is 6boot-owned, GPL-2.0-or-later; the inherited m1n1 handler is
neither called nor copied into the new exception path.
This document describes the BRK entry and shared vector machinery. The
separate EL1 entry and its current QEMU evidence are documented in GUEST.md.

## Architectural basis

The vector table has 16 slots of 128 bytes at a 2 KiB-aligned base. Four groups
distinguish current EL/SP0, current EL/SPx, lower AArch64 and lower AArch32;
each group orders synchronous, IRQ, FIQ and SError entries.
[Arm DAI 0527A, section 5.1.1](https://documentation-service.arm.com/static/5ed11732ca06a95ce53f8f2c?token=).

Exception entry selects the target EL's stack and saves PSTATE/return PC in
SPSR/ELR; software saves GPRs. ERET restores PSTATE and PC from those registers.
For BRK, the saved PC identifies the BRK itself; only the verified BRK resumes
at PC+4. Other exceptions use an explicit abort target.
[Arm 102412, sections 5.1–5.2, pages 31–35](https://documentation-service.arm.com/static/67ac57fb091bfc3e0a9479cc?token=).

The breakpoint check uses EC=0x3c, IL=1 and imm16=0x6e32. The BRK-only entry
does not enable HVC/EL1 handling; the separate guest entry does.
[Arm A64 instruction definitions, BRK](https://documentation-service.arm.com/static/685eb35c3cb5b729562ba389).
Stack frames remain 16-byte aligned; the outer callable interface preserves
its AAPCS/platform callee state, while exception entry preserves all x0–x30.
[Arm AAPCS64, sections 6.1 and 6.4.5](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst).

## Two distinct callable entries

- sixboot_el2_entry: unchanged preflight semantics, 128-byte result.
- sixboot_el2_exception_entry: same PREFLIGHT request, but requires a complete
  2048-byte result span before installing any vectors. Preflight failures
  return the original error semantics; check return status first.

Both require single boot CPU, no recursion/concurrency, identity-mapped owned
RAM, permitted EL2 system-register accesses, EL2h, fully masked DAIF and the
inherited VHE host state (E2H/TGE/RW set, VM clear).
The assembly's early environment or buffer/frame refusal does not write a result.
It rejects unaligned SP before the first stack access and checks request and
complete output spans against the caller save frame before writing that frame.

The new entry additionally requires a declared exclusion of at least 16 KiB
covering the caller's saved 112-byte frame. The arena cannot intersect that
exclusion. The original active SP/frame must also be outside the actual module
arena. This validates declarations and the current frame; actual mapping and
the complete P3 stack allocation must still be checked by a future loader.

## Memory and stack bounds

The 128 KiB layout remains unchanged. Snapshot and 336-byte session are in
the state page; the exception trace is at offset 0x14000. The experiment's
stack is its own 16 KiB range [base+0x8000, base+0xc000), not P3's 16 KiB stack.
No physical address is selected here, and no SP_EL2 MRS is attempted.

Every vector saves x0/x1 in a 16-byte landing, then checks room for the complete
384-byte frame plus the dispatcher's 1024-byte budget before the remaining
stores or any C call. The current clang build reports 32 bytes of C stack use for the
dispatcher; the audit forbids calls from it. Its only loop copies 384 bytes.
ELF/linker bounds and C offset assertions bind assembly to the same layout.

The first 16-byte landing must already be mapped: no software can guarantee
handling an exception with an inaccessible target stack. A bad full-frame
bound takes the assembly-only BAD_STACK path without calling C. It does not
claim to have captured a complete GPR frame. Mapped private state, readable
system registers and a valid saved caller frame are also entry preconditions.
On a nested stack failure it uses the retained original host SPSR. If nesting
started before a valid host SPSR was captured, it returns CONTEXT_LOST via
direct cleanup rather than replaying the handler's SPSR as the original host.

## Session and return

1. Preflight validates the full output span, arena, exclusions, CPU and caller
   frame. No vectors are installed on a preflight error.
2. Zero fixed session/trace storage, then save old VBAR_EL2, DAIF, ELR_EL2,
   SPSR_EL2, ESR_EL2, FAR_EL2, HCR_EL2, SP_EL0 and SP_EL1, plus the private
   call-frame SP.
3. With exceptions masked, write VBAR_EL2
   using the EL2 accessor and issue ISB. No HCR/VHE, MMU or timer change.
4. Seed x0–x30 with distinct values and execute the single labeled BRK.
5. The slot saves all GPRs and raw diagnostics. A valid first current-EL
   SPSR is retained as the host return context before optional diagnostics.
6. The bounded leaf dispatcher classifies/records the exception and selects
   resume or abort. Assembly restores all GPRs and executes ERET.
7. Expected resume checks all 31 register values; any mismatch is
   RESTORE_FAILED. Unexpected resume goes to cleanup, not the faulting PC.
8. Reset SP to the saved private call frame, restore old VBAR with ISB,
   restore/read back old ELR/SPSR/ESR/FAR, compare unchanged HCR/SP_EL0/
   SP_EL1/SPSel, then restore/read back DAIF last.
9. Serialize the fixed result only after inherited vectors are back.
   The wrapper restores its original caller SP/x18–x30 and returns.

Why save syndrome registers? ERET does not preserve the pre-session syndrome
and return state; ESR can become UNKNOWN on exception return. The session
restores the sampled old values rather than leaving its diagnostics installed.
[Arm ESR_EL2 definition](https://documentation-service.arm.com/static/60d32e78677cf7536a55bad7).

No printf, heap, device access or inherited exception helper appears on the
handler path. The only BL from the main handler is e2_exception_dispatch.
The generic trace API is separate and is not called from this handler.

## Expected and unexpected cases

E2_EXPECTED (13) requires the exact slot 4, BRK syndrome, labeled PC, masked
EL2h SPSR, first event and expected HCR/VBAR. Its resume SPSR is the captured
host PSTATE without inventing a new bit pattern.

| Status | Meaning |
| --- | --- |
| E2_UNEXPECTED (14) | Wrong source/syndrome/PC or other non-matching event; recorded and aborted |
| E2_NESTED (15) | Nested handler entry; abandon the interrupted handler |
| E2_BAD_STACK (16) | Frame/C-budget bounds rejected |
| E2_TRACE_FULL (17) | Fixed storage exhausted; saturating drop count, abort |
| E2_NOT_TAKEN (18) | Expected exception was not observed |
| E2_RESTORE_FAILED (19) | GPR check or restore/readback contract failed |
| E2_CONTEXT_LOST (20) | No valid host PSTATE/targets; never fabricate successful restoration |

Unexpected faults do not advance an arbitrary ELR. With a valid host context,
ERET targets the fixed cleanup label using the retained host SPSR. Nesting is
recorded and abandons the interrupted stack. If a host PSTATE cannot be
established, the fallback performs bounded register cleanup and returns
CONTEXT_LOST without claiming PSTATE restoration. No custom panic loop or
reboot is used. A fatal inaccessible stack, unreadable registers or higher-EL
trap is outside the controlled-entry guarantee; no universal recovery claim.

On the BRK-only entry no EL1 code runs, so a lower-EL first event violates
the BRK entry contract. The guest entry adds EL12 preservation and HCR/TGE
policy separately; see GUEST.md. It has TCG coverage, not J713 validation.

## Structured records

Native frame: 384 bytes. All offsets are asserted in el2_exception.h.
The first 248 bytes hold x0–x30; then EL2 landing SP (not an inferred guest SP),
ESR, ELR, SPSR, FAR, DAIF, HCR, VBAR, MPIDR, vector index, depth, status,
resume ELR/SPSR, SP_EL0, validity flags and SP_EL1.

ESR-valid is set for synchronous/SError slots. FAR-valid is limited to
instruction/data abort classes with FnV clear. A raw FAR in a BRK/IRQ record
is not presented as that exception's fault address.

Trace storage is a 64-byte header plus four 384-byte frames, 1600 bytes total
inside the reserved 16 KiB page. No overwrite on overflow, dynamic allocation
or unbounded search. Incomplete stack failures use status/last-vector in the
session rather than fabricating a complete frame.

Exception result ABI v2 is 2048 LE bytes (preflight/request ABI is still v1):

| Offset | Field |
| --- | --- |
| 0 / 4 / 6 | u32 E2_EXCEPTION_MAGIC / u16 version / u16 size |
| 8 / 12 | u32 final status / flags INSTALLED=1, RESTORED=2, EXPECTED=4 |
| 16 / 20 / 24 | u32 frame count / u32 frame size / u64 dropped |
| 32 | u64 restore mask |
| 40..104 | u64 old VBAR, installed VBAR, old DAIF/HCR/ELR/SPSR/ESR/FAR/SP_EL0 |
| 112..176 | u64 private call SP, host SPSR, expected/resume/abort PCs, last vector, active, depth, old SP_EL1 |
| 184..247 | guest request/status/verdict/count/flags/HCR/pre-BRK SP/old SCTLR; see GUEST.md |
| 248 | u64 signature_seen |
| 256..1791 | up to four LE frames |
| 1792..1855 | optional E2OF v1 overflow summary, otherwise zero |
| remainder | zero |

Restore mask: VBAR=1, DAIF=2, ELR=4, SPSR=8, ESR=16, FAR=32,
unchanged HCR/SP_EL0/SP_EL1/SPSel=64, ERET-return flow=128, private call
stack bound=256, guest-bank/PAR cleanup=512. RESTORED requires all 1023 bits plus completed cleanup.
It does not prove the caller's final RET executed; the caller must observe
the return. AAPCS caller-clobbered x0–x17/NZCV are not promised across the
outer function call; the interrupted frame itself is restored in full.

## Verification boundary

Host tests execute the same C dispatch/encoding logic on synthetic frames.
Static checks bind vector offsets, frame stores/loads, BRK/resume labels,
GPR seed/check sequences, permitted MSRs, restore-source offsets and barriers
to the built ELF. Mutation tests remove critical instructions to demonstrate
those checks reject broken artifacts. None executes ERET on a real CPU.

The shared machinery and guest have executed on QEMU TCG, and on J713 on
29 October 2026 (J713.md).
The ARM64 artifact is an internal callable module, not an installable release.
TCG does not establish physical register restoration or arbitrary-fault recovery.
