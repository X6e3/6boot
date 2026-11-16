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

## Not yet run on the machine

- The watchdog: no stage 2 arms it yet. A trial that hangs before Linux would need the power button; the boot
  after that would still go back, because the try is spent before the trial starts, but that has not been run.
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

In the two after-boot reports the script's own read-only mount of the ESP failed, because the ESP was already
mounted at `/boot`; their file hashes are missing for that reason. The record was read through `/boot` instead,
and `esp-check` read it from the raw disk.
