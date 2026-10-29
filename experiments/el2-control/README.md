# Experimental EL2 control module

This is 6boot's own experimental code, GPL-2.0-or-later. Read CONTRACT.md
first. The source baseline is fa608b6. Stage 2 calls the module only from a
separate development build with CFG=SIXBOOT_EL2_NATIVE; ordinary builds do
not link or call it. NATIVE.md describes that adapter, GUEST.md the EL1
guest, and J713.md the first runs on J713 (29 October 2026). Stage-2
translation is future work.

## Implemented now

- A separate AArch64 entry and private stack, entry-state capture and ordinary
  AAPCS64 return. EL2h and fully masked DAIF are checked before changing SP.
- A freestanding preflight core: bounded LE ABI, actual image-base comparison,
  RAM coverage, complete-arena geometry and exclusion/alias checks.
- An in-memory bounded trace API, with no clock or device accesses.
- A 128 KiB relative linker layout, ARM64 ELF/raw-blob build and structural /
  instruction audit. EL1 code is in .guest_text in the RX code page; the
  separate guest-data slot remains reserved.
- ASan/UBSan host tests. Their synthetic state does not prove hardware behavior.
- Private 2 KiB vector table, all-GPR exception frame, bounded leaf C dispatcher,
  fixed exception trace and checked restoration of the previous EL2 environment.
- The explicit exception entry seeds/checks all 31 GPRs around BRK/ERET. This
  machine-code self-check ran on J713 on 29 October 2026 (J713.md).

Only the separate sixboot_el2_exception_entry installs vectors and temporarily
writes documented system registers; sixboot_el2_entry remains preflight.
The separate sixboot_el2_guest_entry runs a register-only EL1 payload after
the BRK stage. The generic trace API is not called by these entries.
The raw binary is
an internal callable module, not an Apple boot object or an installable image.

## Build on macOS

Requires clang with AArch64 ELF support, GNU AArch64 binutils and Python 3.
On macOS, Apple clang and Homebrew's aarch64-elf-binutils are enough. Run
from this directory:

    make BUILD=/absolute/path/to/work/el2-control all

For Linux toolchains, override CROSS_COMPILE, FW_CC/FW_TARGET or FW_LD,
OBJDUMP and OBJCOPY. HOST_CC is separate from the cross compiler. For example,
with aarch64-linux-gnu binutils and clang:

    make BUILD=/absolute/path/to/work/el2-control CROSS_COMPILE=aarch64-linux-gnu- all

Outputs are three host-test executables and el2-foundation.elf/.bin/.dis. There is
intentionally no install, flash, boot or clean target. No build target executes
the ARM64 image. The root Makefile includes native objects only with the
explicit SIXBOOT_EL2_NATIVE configuration. The ELF audit checks segment boundaries and privileges, undefined
symbols/relocations, vector layout, GPR frames and the exact permitted register
writes. Twenty-eight audit tests (one baseline and twenty-seven instruction mutations) check
critical rejection guards. The
dispatcher must remain a leaf within a 1024-byte stack budget (this compiler
currently reports 32 bytes). These checks alone are not an execution proof.

## Optional QEMU TCG execution

QEMU is never downloaded or run by `all`. With QEMU already available:

    make BUILD=/absolute/path/to/work/el2-qemu QEMU=qemu-system-aarch64 all qemu-matrix

This runs the real ARM blob on a diskless QEMU virt machine with 16 KiB host
stage-1 mappings. Seven modes cover normal execution and six deliberate
fault/order/stack failures in temporary copies. The unchanged blob is audited
first. The harness observes the final outer RET, x18..x29, caller SP and
16 system/PSTATE values, including PAR and PAN. It is test code, not a J713
adapter. No m1n1 or Linux binary is linked into it. HLT semihosting exists
only in the harness and is forbidden in the experimental module itself.

`qemu-native-matrix` additionally runs the production native caller gate and
L3-only page protector, using four-level/48-bit tables. It exports actual wire
records for the independent collector decoder tests. Full m1n1/FDT integration
and Linux collector I/O remain outside the emulator coverage.

If QEMU is not on PATH, point at it and its libraries:

    make BUILD=/absolute/path/to/work/el2-qemu \
      QEMU=/path/to/qemu-system-aarch64 \
      QEMU_LIB_DIR=/path/to/qemu/lib \
      all qemu-matrix

On Linux, the same source also accepts e.g. `FW_CC=aarch64-linux-gnu-gcc`,
`FW_TARGET=` and `CROSS_COMPILE=aarch64-linux-gnu-`; the host compiler remains
separate. The GNU/Lima build has not been tried yet.

## Request ABI v1

Integers are little-endian; buffers may be unaligned. Header is 80 bytes:

| Offset | Field |
| --- | --- |
| 0 | u32 magic E2_REQUEST_MAGIC |
| 4 / 6 | u16 version 1 / u16 header size 80 |
| 8 / 12 | u32 total size / u32 mode E2_PREFLIGHT (1) |
| 16 / 20 | u32 chip 0x8132 / four bytes J713 |
| 24 / 28 | u32 flags PRIVATE_ARENA + EXCLUSIONS_COMPLETE / reserved zero |
| 32 / 40 | u64 arena base / u64 arena size 0x20000 |
| 48 / 52 | u32 RAM range count (1..16) / reserved zero |
| 56 / 60 | u32 exclusion count (1..16) / reserved zero |
| 64 / 72 | u64 expected boot MPIDR / reserved zero |
| 80+ | RAM ranges, then exclusions: each u64 base + u64 size |

Total length must match both counts exactly. RAM ranges cannot overlap; one
RAM range must contain the complete arena. Exclusions may overlap each other
and may be outside RAM. None may intersect the arena. The caller must include
all live owned/reserved ranges; these declarations cannot attest ownership.
Address fields are not dereferenced. The arena base must equal the actual
module load base supplied by the assembly entry, with 64 KiB alignment.
Request/result buffers must be outside the arena and must not alias each other
or the result with the snapshot. Accessible spans/mappings remain caller duties.
Both spans are also refused if they intersect the caller's 112-byte save frame;
assembly checks this before the first stack write. This does not validate the
whole caller stack mapping or prevent overlap with every live caller object.

The new entry accepts only EL2h, fully masked DAIF, HCR E2H/TGE/RW set and VM
clear, and the expected boot MPIDR. This deliberately narrows P3's EL1-or-EL2
ABI. A different environment is refused, not silently reconfigured.

## Result ABI v1

128 LE bytes. Offset 0 is magic E2_RESULT_MAGIC; 4/6 are version/size; 8 is
status; 12 is flags (PREFLIGHT_ONLY always, LAYOUT_VALID on READY). Eight u64
fields at 16..79 are CurrentEL, SPSel, original active SP (SP_EL2 with SPSel=1),
SP_EL0, VBAR_EL2, HCR_EL2, DAIF and MPIDR_EL1. At 80/88 are the arena base/size
only when READY. Bytes 96..127 are zero. No success flag denotes guest execution.

ARGUMENT errors write no output, including buffers inside the actual image
arena even when the request claims a different arena. Other C-core errors produce a result. Early
assembly environment or buffer/frame refusal also writes no output, so a future caller must
initialize a sentinel and check the return before accepting a record. Caller
x18–x30 and SP are restored; x0–x17/NZCV may change. No promise is made for an
inaccessible address or disallowed system-register read. There is no exception
handler on this preflight entry. No safe-return claim covers future guest faults yet.

## Separate exception and guest entries

The separate exception entry uses the same request and a 2048-byte result;
its success status is E2_EXPECTED (13), not READY. It also requires an exclusion
range covering the caller frame in a stack of at least 16 KiB. Full result
layout, error statuses and restoration limits are in EXCEPTIONS.md and GUEST.md.
The exception result is now ABI v2 (2048 bytes); request and preflight result
remain v1. A native adapter must reject an unexpected result version.

The native adapter is described in NATIVE.md and its first J713 runs in
J713.md. The guest path uses EL12 guest-bank accessors under inherited VHE;
do not transplant inherited el1_call() or clear HCR bits without the complete
save/restore design. Stage-2 mappings and device/MMIO experiments are later
steps. Nothing here is enabled in the default boot.
