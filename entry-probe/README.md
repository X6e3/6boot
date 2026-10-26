# XBOOT P3 — a standalone callable entry prototype

The module in this directory does not link against m1n1 sources or objects.
P2 is the working m1n1-based control image; P3 is developed separately here.

## Scope and execution status

The ARM64 entry wrapper runs `xp_probe` on its own 16 KiB stack and returns to
the AAPCS64 caller. Apart from reading CurrentEL and MPIDR_EL1 it touches no
registers: no MMIO, no SEP/ANE command, no firmware load, no GPIO, DT or disk
write. This **callable entry experiment** is not yet an Apple boot_args parser,
a Mach-O package, a replacement for m1n1's stage 2 or a bootloader that starts
Linux. The ELF uses a relative layout with entry=0; it does not propose a J713
physical address. The firmware ELF has not been run on a device or an emulator.
The host test runs only the C core in Linux userland, with a synthetic context
and synthetic EL/MPIDR values.

## Context ABI v1

All fields are little-endian; no address field is dereferenced.

| Offset | Field |
|---|---|
| 0 | u32 magic 0x58504354 |
| 4/6 | u16 version=1 / u16 header_size=64 |
| 8/12 | u32 total size / u32 chip=0x8132 |
| 16 | four ASCII bytes J713 |
| 20 | u32 flags=0 |
| 24/32 | u64 loader address/size |
| 40/48 | u64 ADT address/size |
| 56/60 | u32 RAM range count (1..16) / u32 reserved=0 |
| 64+16*n | u64 RAM address/size |

The result is 64 bytes: magic/version/size/status, raw CurrentEL and MPIDR, a
metadata-only flag and, on success, the chip and the RAM range count. The
remaining bytes are zero. This result is not P2's handoff record and does not
verify initialization or hardware identity.

## Call preconditions

- Boot CPU only; no concurrent or recursive calls. Interrupts must already be
  masked by the caller; the code does not change the interrupt state.
- Trap settings must allow reading the EL1/EL2 state and MPIDR_EL1; this module
  has no exception handler. If these preconditions do not hold, a safe return
  is not guaranteed.
- All ELF load segments are mapped correctly, the 16 KiB stack is accessible,
  the relative layout is preserved and the load bias is at least 4 KiB aligned.
  MMU, cache and instruction coherency setup is still the loader's job.
- The caller's stack, the context and the output must be accessible for their
  stated lengths, and the output must not overlap the context. The module
  cannot read inaccessible addresses safely.
- x18..x30 and the caller's stack are restored; x0..x17 are caller-saved under
  AAPCS64. FP/SIMD is not used. The loader/ADT coverage checks only check the
  declared RAM ranges; they do not measure real physical access.

## Build and verification

ARM64 GCC/binutils:

```sh
make BUILD=/tmp/j713-xboot-p3-entry-20261012 all
```

The host test runs under ASan/UBSan and covers truncation, ABI/platform/EL,
overflow/overlap/RAM coverage, unaligned buffers and output bytes that must not
be written. The bare-metal ELF links only its own entry/probe objects;
undefined symbols and relocations are checked separately. Simulation is not
proof of success on hardware.

## Missing before a device experiment

No ESP package is made until the P2→P3 call adapter, the loaded segment and
stack reservation, real context generation, the interrupt/cache/EL entry
contract and a deterministic return to P2 are designed and have passed host and
assembly review. A direct Apple/iBoot entry is a later, separate stage. This
work was not installed on the active P2.

12 October: the first cold start froze at the Gravity logo, and P2 booted after
a power cycle. That problem is recorded separately; P3/SEP/ANE were not loaded.
The cold-start stage and the way back will be clarified before the next
hardware experiment.
