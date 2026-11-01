# E0 visual experiment

Two boxes and a status line, rendered by `src/fb.c`. No GPU/DCP protocol,
second framebuffer renderer, guest MMIO or graphics driver is added.

## Separate experiments

**Framebuffer-only native smoke:** a stage 2 built with
`CFG="FB_SILENT_MODE SIXBOOT_USER SIXBOOT_E0_VISUAL SIXBOOT_E0_FB_SMOKE"`
draws A/B/A from EL2 after a successful P3 call, about 750 ms per frame,
with `SMOKE / NO STAGE-2` on screen, publishes E0VS and E0XT under /chosen
and continues to REL3. The hook is selected in the source by those options;
`build_visual_smoke.py` builds from an unmodified copy of the tree. The
option cannot be combined with SIXBOOT_EL2_NATIVE. No EL1 microguest is
entered and HCR.VM stays zero. A smoke result is not an E0 isolation PASS
or a Gate B prepare result.

**Dual-world emulator:** `WORLDS=1 VISUAL=1` links the actual framebuffer/font
code and cache helper into the diskless harness. Its owned scanout RAM and
support code are outside both guest mappings. Frame A follows A's validated
first HVC, B follows B's validated first HVC, and restored A follows A's
first validated fault after its register/private-memory checks. These are
guest-execution observations, not notifications that a switch was requested.

PASS is drawn only after the 1856-byte E0W1 record passes `visual_check.c`,
an independent byte decoder, after outer host restoration. The Python
decoder checks the same captured record independently. A completed guest
or boot by itself cannot authorize PASS. Corrupting the completion record
must produce a red FAIL frame. Restorable failures can display FAIL; an
unrestorable EL2 context parks without calling framebuffer C code and can
leave the last RUNNING frame. Never weaken terminal restoration safety to
promise an on-screen message from inaccessible mappings.

The emulator observes a 250 ms pause at each of the three confirmed visual
events. `ew_visual_ticks` measures drawing plus those pauses only. There is
no context-switch latency claim or benchmark in this demo.

## Framebuffer and cache contract

`fb_e0_frame()` is compiled only with `SIXBOOT_E0_VISUAL`. It reuses existing
direct fill, font and shadow logic, preserving console activation/font state.
The native baseline still calls `fb_init()`, which maps the physical scanout
as Normal-NC (`src/fb.c`). Before drawing, the helper checks dimensions,
stride, depth30/32, initialized shadow consistency and complete cache-line
bounds. Sampled pages across the drawing band are translated with `AT S1E2W`,
and the level-3 leaf is read back from the mapper's own tables. The panel must
be an identity Normal non-cacheable mapping. PAR may report that class as
0x44, or as 0x40 on a core with FEAT_XS (ID_AA64ISAR1_EL1.XS=1): 0x40 is Normal
NC with XS=0, and it is refused when XS is absent. Write-back is accepted only
for emulator-owned pixel RAM; native write-back scanout, Device memory and any
other attribute or alias refuse. PAR and interrupt masks are restored.

The existing `dc_cvac_range()` advances 64 bytes. The helper reads CTR,
requires a cache line >=64 bytes, aligns the affected rows to full lines
inside the framebuffer, cleans to PoC, and finishes with DSB SY. It does
not change mappings, DART, DCP, or GPU state. The current boot mapping regime
uses the 64-bit PAR format; this is not a generic VMSAv9-128 implementation.
There is no vsync/atomic flip guarantee. CPU pixel publication does not prove
that physical DCP scanout displays it: J713 human observation is still required.

## Commands (host only)

From this directory, use fresh absolute output directories:

```sh
make WORLDS=1 VISUAL=1 SMOKE=1 DEPTH=32 BUILD=/absolute/new/smoke32 qemu QEMU=/absolute/qemu-system-aarch64 QEMU_LIB_DIR=/absolute/qemu-lib
make WORLDS=1 VISUAL=1 SMOKE=1 DEPTH=30 BUILD=/absolute/new/smoke30 qemu QEMU=/absolute/qemu-system-aarch64 QEMU_LIB_DIR=/absolute/qemu-lib
make WORLDS=1 VISUAL=1 DEPTH=32 BUILD=/absolute/new/dual qemu QEMU=/absolute/qemu-system-aarch64 QEMU_LIB_DIR=/absolute/qemu-lib
python3 -I -B build_visual_smoke.py --baseline-build /path/to/native-baseline-build --rel3-bundle /path/to/rel3-bundle --out /absolute/new/native-smoke
python3 -I -B verify_visual_smoke.py /absolute/new/native-smoke --rel3 /path/to/rel3-bundle
```

QEMU tests execute actual `fb.c` drawing on RAM, save PNGs, check both box
states, stride padding, surrounding canaries, ten refusals, PAR/mask
preservation, valid PASS and tampered-record FAIL. They are RAM pixel tests,
not physical display tests or a claim that QEMU models M4 cache coherency.

The source/framebuffer bridge uses the existing driver's warning policy;
the standalone table/trace code retains conversion warnings and sanitizers.
Build IDs include source and world/visual/smoke/depth configuration. Debug
paths are normalized for repeated standalone ELF builds.

## E0VS and E0XT records

E0VS v2 is the current smoke record: 256 bytes, little endian, defined in
`visual_smoke.h` (outcome, reason, stage, restore safety bits, PAR/DAIF before
and after, the EL2 translation registers and the policy decision). Its first
80 bytes keep the v1 layout below, so old captures still decode. Records whose
diagnostic flags include TRANSLATION_REQUIRED (256) must come with the E0XT
v1 companion, `6boot,e0-translation-result`: 168 bytes, magic `E0XT`, holding
the sampled VA, raw PAR, the four descriptors of the walk, the resolved MAIR,
ID_AA64ISAR1/MMFR3/PFR0, the sample count, page shift, leaf level, walk and
attribute status and the publication result (`src/fb_e0.h`). A missing or
inconsistent companion makes the decoder return BLOCKED_EVIDENCE.

E0VS v1, 80 bytes; separate from E2RS/E2EX/E2NA and E0W1.

| Offset | Field |
| --- | --- |
| 0 | u32 magic0x53563045; u16 version1; u16 size80 |
| 8 | u32 frame count; u32 error (0 success,1 mode/context,2 renderer refusal,3 HCR changed) |
| 16 | u64 HCR before, HCR after, CTR, observation ticks, framebuffer address |
| 56 | u32 width, height, byte stride, depth/flags |
| 72 | u64 source build ID |

Native properties: `6boot,e0-visual-mode="smoke"` and
`6boot,e0-visual-result`. The decoder's success label is
`SMOKE_TRACE_VALID`, never Stage2 PASS. Capture the raw result and `/proc/cmdline`
from the same boot and match the package build ID. For example, on an already
booted candidate, copy the chosen properties into a new capture directory
and run `visual_smoke_decode.py RECORD --build BUILD_ID`. No mount/install/reboot
operation is part of the collector or this package.

## Physical activation boundaries

A freshly built smoke package always records `physical_status: PENDING
PHYSICAL TEST` in its manifest: the manifest describes the build, not a later
boot. What happened when a build was booted is in that boot's capture and in
../J713.md.

The native smoke image is a separate candidate. Original REL3
kernel/initramfs bytes and all DT properties except the explicit smoke mode
must verify against the frozen input. Preserve the existing working BOOT.BIN
and fallback selection; do not overwrite either to test this image.

Real Stage2 A/B visual activation remains **BLOCKED / PENDING PHYSICAL TEST**:
native 256 KiB RAM/exclusion and framebuffer DMA ownership, actual J713
MMFR/granule/PA/VMID values, VMID/other-PE ownership, guarded control/cache
authority and a reviewed native E0 loader are prerequisites. Physical VM=0
prepare must precede single-world VM=1; a physical Gate C PASS must precede
Gate D. The smoke image and emulator A/B run do not open those gates.

Arm-authored references (mirrors):
https://df.lth.se/~getz/ARM/SysReg/AArch64-dc-cvac.html and
https://df.lth.se/~getz/ARM/SysReg/AArch64-par_el1.html .
