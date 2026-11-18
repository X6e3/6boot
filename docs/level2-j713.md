# Trial boots on the J713: what ran on the machine

Installer level 2 on the J713 (MacBook Air 13-inch, M4): 6boot boots a trial image once and, if nothing marks it
as good, goes back to its own image on the next boot by itself. This file records what has run on the machine
and what has not. The design is in `src/xboot_bootstate.h`, `src/xboot_trial.h` and `src/xboot_level2.c`; the
host tests are in `tests/bootstate/`.

Every run below was on the J713's internal NVMe, its ESP (`nvme0n1p5`, FAT32 with 4096-byte sectors and
clusters, partition UUID `f2a5b0ec-5cb9-4736-9fa8-135aca4f0234`, at LBA 58111494), with kernel 7.1.13-x6e3-r5.
The reports are kept outside the repository with their SHA-256 (listed at the end).

## 16 November 2026: log only

Stage 2 `6boot-p32-user-l2-j713` (b3e988b, `SIXBOOT_LEVEL2`): it brings up the NVMe, reads the GPT and the
FAT, looks for the record, decides, and writes nothing.

- `/chosen`: `xboot,path = known`, `xboot,trial = no-record`,
  `xboot,trial-report = would=known write=0 failed_at=none err=0 block=0 us=41182`.
- Linux found its root on the NVMe after stage 2 had brought the controller up and shut it down again.

## 17 November 2026: one trial, then back by itself

Stage 2 `6boot-p32-user-l2t-j713` (f9c6b31 with the die of 175eac0, `SIXBOOT_LEVEL2_TRIAL`; stage 2 sha256
`c6381960`), in `M1N1/BOOT.BIN` `9d9076e5` over the R5 payload. On the ESP:

- `M1N1/BOOT-TRIAL.BIN` `614722bf`, 21193824 bytes: the R5 payload alone (the same device tree, kernel and
  initramfs) with one boot argument changed, `x6e3.v=6boot-r5-trial-9c` instead of `6boot-r5-tm-root2`, so the
  booted image can be told from Linux;
- `j713/bootstate.bin` `302846ec`: armed, 1 try of 1, sequence 0, the trial's size and SHA-256;
- `M1N1/BOOT-PREV.BIN` and `M1N1/BOOT-RESCUE.BIN`: R5 `3c9c2ef2`, whose stage 2 does not read the record.

Before rebooting, `esp-check` (`tests/bootstate/esp_test.c`, built for Linux with its writing command removed)
ran the same check on `/dev/nvme0n1` and said what the next boot would do: `boot=trial reason=trial write=1`,
record block 58178214, the trial's hash matching.

**Boot 1** (02:07 +03): 6boot chose the trial, spent its try on the disk and booted it.

- `/proc/cmdline`: `x6e3.v=6boot-r5-trial-9c`.
- `/chosen`: `xboot,path = trial`, `xboot,trial = trial`,
  `xboot,trial-report = would=trial write=1 wrote=1 wstep=none failed_at=none err=0 block=58178214 us=937137`.
- After it, from the raw disk: `esp-check` gave `boot=own reason=trial-failed write=0 parse=0` (the record parses,
  is armed and has no try left). Read by hand through `/boot` (not in the report): 0 tries of 1, sequence 1, armed.

Nothing marked the trial as good (no unit was installed), and the machine was rebooted.

**Boot 2** (02:09 +03): 6boot found the try spent and booted its own image, writing nothing.

- `/proc/cmdline`: `x6e3.v=6boot-r5-tm-root2`.
- `/chosen`: `xboot,path = known`, `xboot,trial = trial-failed`,
  `xboot,trial-report = would=known write=0 wrote=0 wstep=none failed_at=none err=0 block=0 us=41247`.
- After it: `esp-check` gave the same `trial-failed` for the same record; read by hand through `/boot`, still 0 tries
  of 1, sequence 1, armed.

So on the machine: the trial was chosen from the record, the one-block write went through the NVMe and read back
the same, the trial's own boot arguments were the ones Linux got, and the boot after an unmarked trial went back to
the known image without anyone choosing it. Root on the NVMe worked after both boots.

The check took 0.94 s on boot 1 (the 21 MB trial read in 4096-byte commands and hashed, then the write) against
41 ms without a trial; the die stands still during it.

## 17 November 2026: the watchdog resets a trial that Linux never takes over (9D-a)

Stage 2 `6boot-p32-user-l2w-j713` (4cdfb38, `SIXBOOT_LEVEL2_WATCHDOG`; stage 2 sha256 `8bf6c6b0`), in
`M1N1/BOOT.BIN` `4271f78c` over the R5 payload; `BOOT-PREV.BIN` and `BOOT-RESCUE.BIN` R5 `3c9c2ef2`. Once the
trial is loaded it arms the primary watchdog for 60 s of its 24 MHz clock. The trial `b4e1560b` is the R5 payload
with `x6e3.v=6boot-r5-trial-9da`, `x6e3.hb=1` (the heartbeat, `tests/watchdog/heartbeat.c`) and
`initcall_blacklist=apple_wdt_driver_init`, so Linux's watchdog driver never registers and nothing feeds the
watchdog. The blacklist works on this kernel: `module_platform_driver()` names the initcall
`apple_wdt_driver_init`, `do_one_initcall()` checks it by its kallsyms name, and the R5 build has `CONFIG_KALLSYMS=y`,
no LTO and `apple_wdt_driver_init` in its `System.map`; its `Image.gz` is byte for byte the kernel in R5.

**Boot 1**: 6boot chose the trial, spent its try (`wrote=1 err=0`, block 58205527), booted it and armed the
watchdog: `xboot,watchdog = armed=60 s at cntpct=13058109843`. Linux: `x6e3.v=6boot-r5-trial-9da`, no
`/dev/watchdog0`, the `apple-watchdog` driver not registered. Nobody touched the machine. It reset by itself.

**Boot 2**: `x6e3.v=6boot-r5-tm-root2`, `xboot,path = known`, `xboot,trial = trial-failed`, `write=0 wrote=0 wdt=0`,
`xboot,watchdog = off`; the record 0 tries of 1, sequence 3; `esp-check` from the raw disk: `trial-failed`.

So on the machine: 6boot's own watchdog reset a trial that never handed it to Linux, without the power button,
and the boot after it went back to the known image.

What boot 1 left behind:

- The heartbeat log: 112 lines over 55.553 s of `CLOCK_BOOTTIME`, no `stopped` line (a shutdown writes one); the
  journal of boot 1 has no shutdown line either. Its last entry is at 24.14 s (`Startup finished`), so the journal
  does not show when the boot ended.
- The counter: the lines give 1,000,000,000.74 Hz (delta `CNTVCT_EL0` over delta `CLOCK_BOOTTIME`), the M4's
  `CNTFRQ_EL0` of 1 GHz, not the watchdog's 24 MHz. On that counter, measured from the arming: `CLOCK_BOOTTIME`
  0 at 1.475 s, the first heartbeat at 3.348 s, the last at 58.901 s. The reset came after the last heartbeat that
  reached the disk; its exact time was not measured.
- These times assume that Linux's `CNTVCT_EL0` and the `CNTPCT_EL0` 6boot read are the same count. Linux at EL2
  clears `CNTVOFF_EL2` (`arch/arm64/include/asm/el2_setup.h`), and `CLOCK_BOOTTIME` 0 falling 1.475 s after the
  arming fits it; it has not been checked on the machine on its own.
- Not explained: over the same lines `CLOCK_REALTIME` moved 73.498 s while `CLOCK_BOOTTIME` moved 55.553 s.

The first check report of this run divided the counter by 24 MHz and gave 139.48 s and 2454.19 s; the heartbeat's
own header (`boottime0_cnt`) had the same mistake. The check now takes the rate from the lines, and the heartbeat
logs `CNTFRQ_EL0` and computes `boottime0_cnt` from it (`tests/watchdog/check_heartbeat.py` checks both). The stage
2 that ran is unaffected: it arms the watchdog in its own 24 MHz ticks, and a 1 GHz reading would have reset the
trial 1.44 s after the arming, before Linux.

## 9D-b telemetry: prepared, not run (18 November 2026)

Nothing below ran on the J713. It is the heartbeat and the check that a later 9D-b would use, tested in the build
VM only.

**What the heartbeat can add.** It can read the watchdog's own registers (WD1: CUR, BITE, CTRL) every half second
through `/dev/mem`, read only. The read is **off unless the trial's command line has `x6e3.hbwdt=1`**; the 9D-b
trial built on 17 November (3276f25e) does not have it. Turning the read on needs a new trial image and its own
decision.

**Why it is off by default.** Two of the J713's recorded hard hangs were MMIO reads: the 2 November CPU_CONTROL
read and the 3 November B4 read (docs/AB.md in j713-ane). A read is therefore not safe because it is a read. The
R5 sources (`src.tar.gz` ad0c99df) say this much about this one, and no more:
- `apple_wdt.c:37-52`: WD1 is CUR 0x10, BITE 0x14, CTRL 0x1c. CUR counts at the reference clock and "can also be
  overwritten to any value". The machine resets while CTRL has RESET_EN and CUR >= BITE. IRQ_STATUS is cleared by
  writing 1, not by reading.
- The driver reads these registers itself: CTRL at probe (`:186`), CUR and BITE in `get_timeleft` (`:115-116`), and
  CUR in `restart` (`:136`). It writes CUR = 0 in ping, start, set_timeout and restart (`:71, :90, :100, :128`).
- The driver maps the block non-posted (`nonposted-mmio` on `soc` in the trial DT, so `ioremap_np`,
  Device-nGnRnE). `/dev/mem` maps a non-RAM page `pgprot_noncached`, also Device-nGnRnE (`arch/arm64/mm/mmu.c:95`,
  `pgtable.h:751`). It is the same memory type at the same address.
- `/dev/mem` allows it: `CONFIG_STRICT_DEVMEM=y` blocks RAM and exclusive regions, `CONFIG_IO_STRICT_DEVMEM` is
  off, the driver's region is not exclusive (`kernel/resource.c:1884-1925`), and `SECURITY_LOCKDOWN_LSM` is off.
- The trial DT's watchdog node has no `power-domains`; the clock is the 24 MHz fixed `clkref`.
- An SError from a user-space read panics (`traps.c:979-983`), and `PANIC_TIMEOUT=0` leaves a panicked kernel
  sitting there. A read that hangs the fabric does neither: that is what the earlier hangs looked like.

What is **not** shown: that reading WD1 from user space at an arbitrary moment, possibly while the driver writes
CUR, is harmless on the J713; and that the watchdog still bites when the fabric is hung. 9D-a showed only that it
bites when nothing feeds it and the machine is otherwise well.

**How a hanging read would show.** The heartbeat line before each read reaches the disk first. A log that ends
between a `t` line and its `d` line is flagged (`ends_before_read`). A read slower than 1 ms is flagged
(`slow_read`).

**The chain, and the evidence for each link** (the check prints this for every log):

| link | independent evidence | what it does not show |
|---|---|---|
| 1 6boot armed WD1 | 6boot's own `xboot,watchdog` in `/chosen` (armed, at cntpct A) | that the arming took effect before Linux |
| 2 Linux took it over | initcall_debug's probe record of the watchdog (time on the counter via the marks); header: `/dev/watchdog0`, driver registered; with the read: the first BITE = 30 s x 24 MHz (the driver's set_timeout) instead of 6boot's 60 s | the exact moment R of the takeover (only the probe record's time) |
| 3 user space did not open it | the `w` lines (every 0.5 s); the journal's "did not stop" count (check script) | an open shorter than 0.5 s |
| 4 the kernel fed it | with the read: zeroings of CUR, each on the counter with an error bound, and their spacing (15 s in the source model) | who wrote the 0 (any write of 0, or of a small value, looks the same) |
| 5 feeds stopped | with the read: the last zeroing, then reads with CUR only counting up | — |
| 6 the heartbeat stopped | the last `t` line on disk: the reset, if any, came after it | the reset itself |
| 7 reset (PREDICTED) | from the last read: when CUR would reach BITE, with an error bound | anything: it is a prediction |
| 8 the reset happened, 6boot fell back | the next boot: 6boot's report (path=known, trial-failed, record seq) through `esp-check`; the next boot's wall clock, which `--next-boot-realtime` compares with the prediction | the reset time (the next boot only bounds it from above, coarsely) |

**Clocks.**
- The counter (CNTVCT, 1 GHz on the M4) is the reference.
- CLOCK_BOOTTIME must keep to it within 1 ms (FAIL otherwise).
- CLOCK_REALTIME is used only between its steps. The 9D-a log has one: +17.945 s between BOOTTIME 35.918 and
  36.419 s, cause unknown, now flagged as a WARN.
- The watchdog clock is fitted on the counter over stretches without a zeroing. Its rate, spread and largest
  residual bound every time computed from CUR.

**Tests** (build VM: Fedora aarch64, gcc 16.2.1):
- `make -C tests/watchdog test`, three times in a row and once as root: the 13 unit tests on written logs, the fake
  watchdog run and 10 fake scenarios:
  - feeds, takeover, ctrl, wrap, stall, lowered, fast, pastbite;
  - gap (SIGSTOP 0.8–2.6 s);
  - off (no `x6e3.hbwdt=1`).
- The 9D-a log (fixture, sha256 as below) checks as before: 1.475 / 3.348 / 58.901 s.
- `gcc -fanalyzer` is clean.
- Two things the tests taught:
  - Across a gap, a write of 0 cannot be told from CUR having stopped. The check says "slow" and names both
    explanations.
  - A write of a lower value shortly before a read can look like a write of 0. Only a drop larger than the read
    interval is "lowered".
- The first VM run, just after the VM booted, failed `cntfrq` and `boottime` in two scenarios. chronyd was setting
  the clock; the reruns passed. The check is meant to catch exactly that.

## Not yet run on the machine

- The watchdog with Linux's driver: a trial whose `apple_wdt` takes the watchdog over and whose user space never
  opens it within `watchdog.open_timeout` (9D-b), with or without the register read above.
- A write that fails, is lost, is torn or is not flushed: only on the host (`esp_fixture_test.py`).
- A trial that is marked good (promote) and the record set back to idle from Linux.
- Recovery from REL3.

## Reports

Kept with the session folders (not in this repository), SHA-256:

```
c3a0d8f4cc05b63c4020d73b4d16280cd54e553b3dcc0cfe030e2dc54fb8e060  rapor-9b-20261116T222520Z.txt
475b1fcc488bc10e9cc5e335867a0276071e8d66d06cee85d28b103157a872ef  rapor-9c-hazirla-20261116T230552Z.txt
ce36706afd2eb5db3aea433c147a30a848b98fee27ce1684d1d637b20f7c55fd  onucus.txt
92719ee04317e1cb8edfc8d2e1d7ec76b9369256c335355c3f9bb5edfc6fc667  rapor-9c-1-20261116T230830Z.txt
903cb3db5cb4e7adcc5523624d2172722ef99e4aa260372384f8268025180c7e  rapor-9c-2-20261116T231021Z.txt
```

9D-a (17 November):

```
2ab7c5a0ed97aa7a03c994b57feebd723e47eaab1fdf014155e6b0bcf20ea218  rapor-9da-hazirla-20261116T235012Z.txt
aea5ebc90706180629beb7fef4c93d96564d31e60be747ae0235557eaaec3dc4  onucus-9da.txt
014c7fe8e5fe7d618e098c3ce93954bce63f6ee1ebcb9e05e5231578e23b183d  heartbeat-9da.log
ead088c8d5460c41995c3c686bd3a0461097a286878caf287c4a1c5325c21876  rapor-9da-kontrol-20261116T235349Z.txt
7b68561fd1a3c435d4aa2b37cd898744e6b397c8687891e486e8d11a1b82d92c  rapor-9da-kontrol-20261116T235840Z.txt
```

The first 9D-a check report is the one with the 24 MHz mistake; the second is after the fix.

In the two after-boot reports of 9C the script's own read-only mount of the ESP failed, because the ESP was already
mounted at `/boot`; their file hashes are missing for that reason. The record was read through `/boot` instead,
and `esp-check` read it from the raw disk.
