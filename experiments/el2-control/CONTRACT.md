# 6boot experimental EL2 foundation — preserved contracts

Baseline inspected: main at fa608b63da0024813a01fdb8f3766f621408c8d0,
28 October 2026. The tree was clean before the foundation step; the vector
step preserves and extends that untracked module. This note uses
the live source tree, not the earlier exported review copies. No device or
X63-R access is part of this work.

## Ownership and evidence

- 6boot additions: entry-probe/arch/entry.S, entry-probe/core/probe.c,
  src/entry_probe.c, src/xboot_*, src/m1n1_adapter.c and boot-trace adapters.
- Inherited from the eb9d627 import of m1n1 950e6b7: startup, CPU/MMU setup,
  exception.c, exception_asm.S, hv.c, hv_asm.S, hv_vm.c, firmware/device
  drivers and the underlying boot path. The existing hypervisor is not a new
  6boot implementation. New work lives under experiments/el2-control/, plus
  the hooks in src/ that only a CFG=SIXBOOT_EL2_NATIVE build compiles.
- Commit 9f84907a2a92c79a123629ea08590c112174913a records a 19 October J713
  P3 execution and return at EL2. It is a historical device-test statement.
  The old entry-probe README
  predates that integration. Neither that statement nor the P3 result format
  contains a VBAR/HCR/SPSel snapshot; their historical numeric values remain
  unknown. P3's raw CurrentEL value for EL2 is 8, not 2.

## Existing P3 call and return

src/kboot.c calls entry_probe_run(dt) after dt_set_memory(), before the
handoff record and fdt_pack(). The adapter allocates 32 KiB aligned to 64 KiB,
copies the separate probe, prepares D/I cache coherency and supplies its own
context and 64-byte result. It saves DAIF, masks D/A/I/F, performs an ordinary
AAPCS64 function call, restores DAIF, copies results into /chosen and frees
the temporary image. This call does not change Exception level.

| State | What current source establishes |
| --- | --- |
| CurrentEL | Entry assembly reads it; the C validator accepts raw 4 or 8. The device-test commit reports EL2. |
| DAIF | The adapter forces mask 0x3c0 for the call and restores the saved value afterward. |
| HCR_EL2 | Normal inherited exception_initialize() sets API/APK/TEA/E2H/RW/TGE/AMO/IMO/FMO. P3 neither reads nor changes it. This is a source expectation, not a historical measurement. |
| VBAR_EL2 | Inherited setup writes VBAR_EL1 and guest forwarding through VBAR_EL12. With E2H, EL1 accessor names can alias host EL2 state. Read VBAR_EL2 explicitly; do not assume a literal vector address. |
| Active SP / SP_EL2 | P3 saves/restores its active SP and uses a private 16 KiB stack, without reading SPSel. Therefore its old record does not prove SP_EL2 selection. At EL2, do not use MRS SP_EL2; use MOV from SP when SPSel=1. |
| Return | Preserve x18–x30 and the original active SP. x0 is status; x0–x17 and NZCV are caller-clobbered. FP/SIMD is unused. P3 has no private exception table or exception-return contract. |

## This foundation's boundary

This is a separate, callable AArch64 image and a host-testable C core. Nothing
in the production Makefile, P3 adapter, installer or boot path calls or embeds
it. Building it is an explicit opt-in operation. The original callable entry
implements PREFLIGHT. A separate sixboot_el2_exception_entry now installs
private EL2 vectors and a bounded current-EL BRK experiment; see EXCEPTIONS.md.
READY means preflight passed. E2_EXPECTED means the separate exception path
completed its BRK, GPR checks and restore checks; neither denotes an EL1 guest.
The later sixboot_el2_guest_entry is separate: it requests the EL1/HVC stage
after the BRK checks. GUEST.md defines its save/restore and ABI v3 result.

The assembly refuses CurrentEL != 8, SPSel != 1, or unmasked DAIF before
changing stack. Such early refusals return ENVIRONMENT without writing the
result. Before the first stack write it also checks SP alignment and refuses
overflowing request/output spans or spans crossing the caller's 112-byte save
frame; those buffer errors return ARGUMENT without writing a result.
It reads VBAR_EL2/HCR_EL2/SP_EL0/MPIDR_EL1, assumes those reads are
permitted by the current platform. The original preflight path temporarily
changes the selected SP and restores it and x18–x30, leaving system registers
untouched. The BRK exception path saves/restores VBAR, DAIF, ELR/SPSR
and ESR/FAR explicitly; HCR and translation state remain unchanged. It uses
BRK and ERET, with no HVC/EL1, SMC, MMIO, firmware calls, cache maintenance
or TLB operations.

The caller must provide accessible, correctly mapped code/data/stack, prepare
instruction coherency, and call on the boot CPU only, with no recursion or
concurrency. C length/range validation cannot catch an inaccessible pointer or
an exception while reading a system register. The real loader adapter is
deliberately absent in this phase.

## Memory layout: offsets, never physical-address recommendations

Use a freshly allocator-owned, 64 KiB-aligned 128 KiB RAM arena. Exclude the
loader, caller stack, ADT, FDT, kernel, initramfs, live page tables, framebuffer
and every firmware/device reserved region. A range being inside RAM is not
enough: a future adapter must build the exclusion list from current live
ownership and check actual mappings. The PRIVATE_ARENA flag is a caller
assertion, not hardware attestation.

| Arena offset | Size | Use in this phase |
| --- | --- | --- |
| 0x00000 | 16 KiB | Separate module text/rodata; ELF RX segment |
| 0x04000 | 16 KiB | Entry snapshot and saved exception session; RW |
| 0x08000 | 16 KiB | Private EL2 stack; RW |
| 0x0c000 | 16 KiB | Reserved future guest payload; not executed |
| 0x10000 | 16 KiB | EL1 stack; current register-only payload does not access it |
| 0x14000 | 16 KiB | Fixed exception trace (1600 bytes); generic API remains separate |
| 0x18000 | 32 KiB | Reserved; not a protected guard page |

These are linker reservations, not installed page permissions or DMA isolation.
The complete arena must be mapped/owned and BSS initialized by a future loader.
The firmware entry verifies the request's arena base against its actual linked
load base. No address is selected from another boot or guessed from free RAM.
Request and result buffers must be outside the arena and not overlap each other.

## State for current vectors and the future EL1 phase

The vector path now saves the exact incoming VBAR_EL2,
HCR_EL2, DAIF, SPSel/active SP, SP_EL0/SP_EL1, ELR_EL2 and SPSR_EL2. A real
exception handler needs all x0–x30, not only AAPCS callee-saved registers.
Account separately for guest EL1 register banks under VHE (EL12 accessors),
including SCTLR/TCR/TTBR/MAIR/VBAR/CPACR/context state and timers. If changed,
also save/restore VTCR_EL2, VTTBR_EL2, CNTHCTL_EL2, CNTVOFF_EL2, CPTR_EL2,
MDCR_EL2 and any platform GXF/SPRR state. Guest-bank, translation and timer
changes beyond those explicitly implemented in GUEST.md remain future work. Do not clear
E2H or TGE casually: register aliasing changes.

Restore vectors, translation/trap state and the selected stack while exceptions
remain masked; restore interrupt state last, with architecture-required barriers.
Any future MMU change also needs an explicit cache/TLB and lifetime contract.
The BRK entry changes no guest MMU, translation or timer state. The guest
entry saves/restores SCTLR/VBAR/ELR/SPSR_EL12 and SP_EL1, changes HCR temporarily,
and preserves PAR_EL1 across AT checks. It does not enable Stage-2 or change timers.

## Future hook placement

Use a new, explicit development-only adapter near the existing P3/DT preparation
phase, before handoff authorization and before shutdown/MMU teardown. Allocate
and validate the arena after payload/reservation information exists, and before
final dt_set_memory()/fdt_pack() if allocation changes usable-memory accounting.
Run and free temporary state before that final accounting; copy a result record
out before freeing. If trace survives into Linux, reserve it explicitly and do
not publish a pointer into freed memory. Never add the experiment after
xboot_arm_handoff(), exception_shutdown() or mmu_shutdown().

No hook is added now. An experiment failure must remain distinct from a normal
boot success, and recovery into the inherited path requires a demonstrated
restore contract before any physical test is proposed.
