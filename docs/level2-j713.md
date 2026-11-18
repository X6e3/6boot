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

## 18 November 2026: Linux's driver takes the watchdog over, then lets it go (9D-b)

**Setup.**
- Trial 3276f25e: R5 with `watchdog.open_timeout=120`, `x6e3.hb=1`, `initcall_debug`, `log_buf_len=4M` and
  `printk.ignore_loglevel=0`; no `initcall_blacklist`.
- Stage 2 `6boot-p32-user-l2w-j713`; heartbeat v3, without the register read.
- Prepared on 17 November (session folder `oturum9db`, whose 17 files matched their SHA-256 on 18 November before
  the run).
- Run by Can on 18 November, about 08:18–08:26 +03.

**Boot 1:**
- 6boot chose the trial and spent its try (`wrote=1 err=0`, block 58210733, `us=908087`).
- It armed the watchdog for 60 s at cntpct A = 12072308434 (`xboot,watchdog`) and booted R5 with its driver.
- **`apple_wdt` probed:** "probe of 3882b0000.watchdog returned 0 after 59 usecs" at kernel 0.329576 s, which is
  A + 1.767 s on the counter (the heartbeat's kernel log marks: 12 marks, spread 0.608 ms).
- **The same record is in the trial's survey (`0319-...-cb08159d/dmesg.txt`)**, and its `iomem.txt` shows
  3882b0000-3882b3fff claimed by the watchdog. The heartbeat header: `/dev/watchdog0`, driver registered.
- **Nobody opened `/dev/watchdog`** in user space at any of the 233 checks, and the journal has no "did not stop".
- **Heartbeats:** 233 over 117.067 s of CLOCK_BOOTTIME; the first at A + 3.404 s, the last on disk at A + 120.471 s.
  There is no `stopped` line and no shutdown in the journal.

**Boot 2:**
- `x6e3.v=6boot-r5-tm-root2`, `xboot,path = known`, `xboot,trial = trial-failed`, `write=0 wrote=0 wdt=0`.
- The record: installed, try 0/1, seq 5.
- `esp-check`: `boot=own reason=trial-failed`.
- The survey folder 320 follows the trial's 319.

**What it shows:**
- **6boot alone would have reset the machine at A + 60.000 s. The heartbeat was alive 60.471 s after that.** Something
  stopped the 60 s bite after 6boot armed the watchdog. The only candidate in the records is Linux's `apple_wdt`,
  probed at A + 1.767 s. By the source (`KAYNAK-9DB.txt`), its set_timeout writes CUR 0 and BITE 30 s, and the
  watchdog core then feeds it while nobody has it open. The feeding itself was not observed: the register read was
  not part of this run.
- **By the same source the feeds stop at R + 90 s and the reset comes at R + 120 s:** about A + 121.77 s, with R taken
  as the probe record's time.
  - The last heartbeat on disk is 1.296 s before that, so the two heartbeats expected in between are not in the file.
  - That is consistent with the source and with the delayed NVMe flush losing the last fsync'd lines. Neither the
    moment of the reset nor the lost lines are measured.
- **9D-a against 9D-b:**

  | | the driver | the last heartbeat | the source model's reset |
  |---|---|---|---|
  | 9D-a | blacklisted | A + 58.901 s | A + 60 s |
  | 9D-b | took the watchdog over | A + 120.471 s | A + 121.77 s |

  The two last heartbeats are 61.57 s apart; the models are 61.77 s apart. Both logs end 1.1–1.3 s before their
  model's reset.
- **6boot found the try spent and booted its own image.**

**The wall clock, explained.**
- The heartbeat saw it step +18.920 s between boottime 36.368 and 36.872 s. The journal has systemd-timesyncd's
  "Initial clock synchronization" at kernel 36.497 s, inside that step.
- 9D-a's +17.945 s step came at the same point of the boot (35.918–36.419 s) and is very likely the same thing; its
  journal was not kept, so that is not shown.
- **The J713 has no RTC under R5:** `macsmc-rtc: Failed to locate of_node`, so the driver does not bind. Each boot's
  clock starts from timesyncd's saved clock file and runs behind until that boot's own sync. The next boot's survey
  files say 08:24:42 on that basis, while the trial's last heartbeat says 08:25:10 on the synced basis. Wall clocks
  from before a boot's sync cannot be compared across boots.

**Deviation from the runbook (told by Can on 18 November, after the run).** The runbook said to touch nothing
during boot 1. Out of habit, Can opened KRDC and connected as soon as the desktop came up (within about two
seconds, in his account), then opened a terminal.
In his account he typed no command and mounted nothing; this is not checked against the boot's journal or shell
history. What the records say about it:
- **Nobody opened `/dev/watchdog`.** The heartbeat checks every 0.5 s and logs a `w` line on every change; there is
  one line, "none", and it never changed. The journal has no "did not stop", which an open without magic close
  would have left.
  - An open and a magic close inside 0.5 s would have stopped the watchdog. The machine reset anyway, at the time
    the source model gives.
  - User space has no other way to feed it: `CONFIG_WATCHDOG_SYSFS` is off.
- **The heartbeat was not disturbed.** The interval median was 503.6 ms in every 10 s window from A + 10 s to the end,
  and the largest interval was 516 ms (in the first 10 s).
- **The timing rules out an early hang.** The kernel feeds stop at R + 90 s whatever user space does, so a hang after
  that point would not move the reset. The heartbeat lived to R + 118.7 s, so no hang came before it.

The main results stand for these reasons, but the run was not untouched. A repeat with nothing touched would remove
the question.

**Not shown:**
- the feeding;
- the moment of the reset;
- what reset the machine (a watchdog bite is the only cause consistent with every record, but no record says so);
- what the disk lost;
- that KRDC and the terminal did nothing else (Can's account only).

### The same trial again, nothing touched (18 November, about 08:56–09:00 +03)

Can ran the same package again so that the deviation above would not be the only record.
- **Boot 1:**
  - 6boot spent the try (`wrote=1 err=0`, block 58215909) and armed for 60 s at A = 12128020514.
  - `apple_wdt` probed at A + 1.829 s. The record is in the heartbeat's kernel log copy and in survey
    `0322-...-3c4e6b13`.
  - Nobody opened `/dev/watchdog` (234 checks), and there is no "did not stop".
  - The last heartbeat on disk is at **A + 121.731 s = R + 119.902 s**.
  - The journal has the automatic login (sddm autologin, Plasma) and no KRDC. KRDC may not write to the journal,
    so this is consistent with Can's account and does not prove it.
- **Boot 2:** `path = known`, `trial = trial-failed`, `write=0`; the record shows try 0/1, seq 5; `esp-check`
  says `reason=trial-failed`; survey 323 follows the trial's 322.
- **Against the source model's reset at R + 120 s:** the last line is 0.098 s before it, against 1.296 s in the
  first run. Both are within the delayed-flush window, and neither is a measurement of the reset.

| run | the driver probed | the last heartbeat | before R + 120 s |
|---|---|---|---|
| first (KRDC, terminal) | A + 1.767 s | A + 120.471 s | 1.296 s |
| second (nothing touched) | A + 1.829 s | A + 121.731 s | 0.098 s |

**The kit's check failed the second run, and the check was wrong.** `check_heartbeat.py` in `oturum9db`
(17 November) failed it with "CLOCK_BOOTTIME 0 moves on the counter by more than 1 ms" and "the header's
boottime0_cnt disagrees with the lines". It reported 999968098 Hz as the counter's measured frequency.
- **What happened:** CLOCK_BOOTTIME kept to the counter within about 1 µs until boottime about 68 s, then ran ahead
  of it. The rate started near 230 ppm and decayed, and it had gained 3.776 ms by the end. That is the kernel's NTP
  loop slewing the clock (adjtimex). It started about 32 s after timesyncd's first synchronization at 36.04 s,
  where its next poll would be; timesyncd does not log that poll.
- **The counter did not move.** The kernel log's clock (sched_clock, which NTP does not slew) kept to the counter
  within 3.9 µs over all 12 marks.
- **The kit's check had two errors:** it assumed CLOCK_BOOTTIME runs with the counter, and its "measured frequency"
  was counter ticks per CLOCK_BOOTTIME second.
- **The fix:** the check in this repository now treats a move of CLOCK_BOOTTIME as an NTP slew. It is a WARN with
  its size, start and largest rate, and only a rate over 1000 ppm is a FAIL. It uses the counter for every time, and
  takes CLOCK_BOOTTIME 0 from the first line.
- **Nothing on the machine changed between the two runs.** The 17 files of `oturum9db` matched their sums before
  and after. The kit's copy of the check is left as it was, as a record of what ran.
- With the fix, both runs pass. The first has one WARN (the wall clock step); the second has two (the slew and the
  step).

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

**Measuring the read.** The two counter reads around the register loads are ordered against them (the
architecture lets a counter read move past loads): `isb; mrs` and a load that depends on the value then `dmb ld`
(Linux's `arch_counter_enforce_ordering`, `barrier.h:116`) before, `dsb ld; isb; mrs` after. The disassembly of
the binary shows exactly that around the three loads. This orders the measurement. It does not make the read safe.

**How a hanging read would show.** The heartbeat line before each read is written and fsynced first. A log that
ends between a `t` line and its `d` line is flagged (`ends_before_read`), and so is a read slower than 1 ms
(`slow_read`).

**What a line on disk means.** put() writes a line and fsyncs it, so a line found in the file was written when its
counter says. The converse does not hold. In R5 the Apple NVMe driver completes a flush that comes within 1 s of
the previous one without sending it to the drive (`drivers/nvme/host/apple.c:781-798, 837-840`,
`flush_interval = 1000` ms), so fsync can return while the line is still in the drive's cache. A reset can lose
about the last second of lines that fsync reported done, and at worst leave a torn tail.
- The last line is when the heartbeat was alive, not when it stopped.
- The check compares it with the prediction that applies: a gap up to 1 s plus two intervals is "consistent with
  the lost lines, not shown"; a longer one is a WARN (`stopped_early`).
- In 9D-a the log ends 1.099 s before 6boot's 60 s, so the two heartbeats at 59.40 and 59.90 s are missing. That
  is consistent with this. It is not proven by it.
- Failed writes and fsyncs are now counted and logged (`e` lines). No fsync was added, and nothing else in the I/O
  changed.
- `nvme_apple.flush_interval=0` on a trial's command line would make every fsync reach the drive. That needs a new
  trial image, so it is not done here.

**The telemetry against the command line.**
- Without `x6e3.hbwdt=1`, "wdt off" or no wdt header at all (a heartbeat from before the read, as 9D-a) is expected.
- With it, "wdt none" (asked for, not available) is a FAIL.
- A header that contradicts the command line, or reads under a header that says there are none, is a FAIL.
- `--expect-feed` is a separate claim.

**What CUR did between two reads.** The classes are:
- count (as its clock adds);
- zeroed (it went down, to what a write of 0 between the reads explains);
- possible_zeroing (it went up, but less than its clock adds, and a write of 0 explains it as well as a stop
  does). This is kept apart from zeroings and is a WARN;
- wrap (past 2^32);
- stalled;
- slow;
- fast;
- lowered.

Who wrote a 0 is not in the log.

**Uncertainty.** Every time computed from CUR comes with an *estimated* uncertainty and its basis:
- from the clock fit (the rate's spread, the largest residual, half the read), assuming the watchdog clock runs
  linearly against the counter over the whole log;
- or, with too few reads to fit, "NOT measured": the nominal 24 MHz within `--wdt-tol`.

It is not a guarantee. 6boot's own prediction (armed + N s) has no uncertainty estimate: it assumes exactly
24 MHz. If the reads do not advance, nothing is fitted or predicted, and the result is a clean FAIL. An error
inside the checker is a FAIL too.

**The chain, and the evidence for each link** (the check prints this for every log):

| link | independent evidence | what it does not show |
|---|---|---|
| 1 6boot armed WD1 | 6boot's own `xboot,watchdog` in `/chosen` (armed, at cntpct A) | that the arming took effect before Linux |
| 2 Linux took it over | the probe record of the watchdog (initcall_debug) in the heartbeat's kernel log copy and in the trial's survey `dmesg.txt`; the survey's `iomem.txt` (the region claimed); header: driver registered; with the read: the first BITE = 30 s x 24 MHz (set_timeout) instead of 6boot's 60 s | the exact moment of the takeover (only the probe record's time) |
| 3 user space did not open it | the `w` lines (every 0.5 s); the journal's "did not stop" count (check script) | an open shorter than 0.5 s |
| 4 the kernel fed it | with the read: zeroings of CUR, each on the counter with an estimated uncertainty, their spacing (15 s in the source model); possible zeroings apart | who wrote the 0 |
| 5 feeds stopped | with the read: the last zeroing, then reads with CUR only counting up | — |
| 6 the heartbeat stopped | the last line on disk: alive then; later lines may have been lost (delayed flush) | when it stopped; the reset |
| 7 reset (PREDICTED) | from the last read (or, with no driver, from 6boot's arming) | anything: it is a prediction |
| 8 the reset happened, 6boot fell back | 6boot's next-boot report (path=known, trial-failed, record seq) through `esp-check`; the next survey folder (a different boot, its seq after the trial's; its RTC line) | the reset time |

**Across boots.** x6e3-survey (in the R5 initramfs, `/x6e3/x6e3-survey`) writes
`/var/lib/x6e3/survey/<k>/<seq>-<tag>-<boot id>/` about 20 s into every boot: `cmdline.txt`, `dmesg.txt`,
`iomem.txt`, `findmnt`.
- `--trial-survey` must be the log's boot with the log's command line.
- Its files' times say when the survey finished: a sign of life, not the boot's start.
- `--next-survey` would give the next kernel's start on the wall clock from an RTC line ("setting system clock to
  ... (EPOCH)", `drivers/rtc/class.c:433-436`; whole seconds, so ±0.5 s). **On the J713 under R5 there is none:**
  the RTC driver does not bind (9D-b), and a boot's clock is behind until its timesyncd sync. A value for
  `--next-boot-realtime` must therefore come from after the next boot's sync, converted back to its kernel's
  start.
- The window between the last heartbeat and the next kernel is checked against the prediction with an uncertainty
  of every wall clock step in the trial plus 1 s. The two boots' wall clocks need not share a basis: 9D-a's clock
  jumped 17.945 s, and R5 writes the system time back to the RTC (`RTC_SYSTOHC`).
- The R5 kernel (Image.gz 5ba9b133) is unchanged; nothing was added to `apple_wdt`.

**Tests** (build VM: Fedora aarch64, gcc 16.2.1; host: macOS Python 3):
- `make -C tests/watchdog test`, three times in a row and once as root, after the VM's clock had settled:
  - 25 unit tests on written logs:
    - the 9D-a fixture;
    - a 9D-b-like log;
    - the telemetry/command-line matrix;
    - possible zeroing;
    - reads that do not advance;
    - a malformed line against a torn tail;
    - failed writes;
    - the checker failing;
    - the survey folders and the reset window;
  - the fake watchdog run;
  - 11 fake scenarios: feeds, takeover, ctrl, wrap, stall, lowered, fast, pastbite, gap, off, and unavailable,
    where the read is asked for with no node and must FAIL.
- `gcc -fanalyzer` is clean.
- Things the tests taught:
  - Across a gap, a write of 0 cannot be told from CUR having stopped.
  - A write of a lower value shortly before a read can look like a write of 0.
  - The first VM run right after boot failed `cntfrq` and `boottime` while chronyd set the clock.

**Not shown, and assumed nowhere:**
1. that a user-space read of WD1 is harmless on the J713;
2. that the watchdog bites when the fabric is hung;
3. whether CUR wraps or stops at 2^32 (the check tells the two apart);
4. who wrote each 0;
5. CNTVOFF = 0 (source only);
6. that the watchdog clock is exactly 24 MHz (DT; measured only if the read is on);
7. the cause of 9D-a's wall clock step, and whether the next boot's RTC shares the trial's basis;
8. that the Apple ARM cores follow the architecture's ordering for these barriers;
9. how much the drive loses at a reset (the delayed flush makes up to about 1 s possible, not certain);
10. the fake watchdog models the registers as the driver's source describes them, not as the hardware behaves.

## Not yet run on the machine

- 9D-b with the register read above (`x6e3.hbwdt=1`): the feeds and the stop observed rather than inferred.
- A user space that opens `/dev/watchdog`, and a kernel that hangs or panics after taking the watchdog over.
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

9D-b (18 November), session folder `oturum9db`; the evidence copied by Can into `kanit-9db/` with its own sums
(`SHA256SUMS-9DB.txt`, 35 files, all matching on the Mac):

The first run:

```
1926952802abb436acc50564bdff2f95b99292adb2a5c0e265d9b163587af8ea  rapor-9db-hazirla-20261118T051802Z.txt
cd94a9c619b49ca506efc1b11de41ddf8b48e5efd12e9b8057e4b85823e9e74b  onucus-9db.txt
2635c7ed8295927869f29fd8d4b89b698f31e39e457392c0471b9139ec4b1be0  rapor-9db-kontrol-20261118T052623Z.txt
98afb6c8c615a393a8e1418e612aaf77e9b34cd73dbc5928a39f8bdf3f80f244  kanit-9db/SHA256SUMS-9DB.txt
04b2ca3e30f7450a35e4e5f17463c1b225ec1c0239b3d9fd50a154ba997cdc4e  kanit-9db/cb08159d-611c-48da-93a2-34d827545eec.log
38c2873cba647aeff206ead8780b5c3ff16b61452491fd66fd85009c7ea46f94  kanit-9db/journal-trial-k.txt
4828b3ef0fbc30a100e53e0ac31521a853bd8fe9ef925a8cb4605cc20b6a427e  kanit-9db/journal-known-k.txt
d9ec0259b44d97eb3ebcafe2a51e831b399423845757a502591c7be233d6bc5d  esp-before-9db.img
```

The second run (`kanit-9db-2/`, `SHA256SUMS-9DB-2.txt`, 37 files, all matching on the Mac). `onucus-9db.txt` and
`esp-before-9db.img` in the session folder were rewritten by its preparation:

```
741602719d2f8b5cf7c5bbd3e6f17f3d4cb8ae7a0873bb62116b5948f6b5a767  rapor-9db-hazirla-20261118T055655Z.txt
fa0b5820c44850e91119754687cc86ffc557b3d7dd2d5fcd103d7d6933956f9e  rapor-9db-kontrol-20261118T060048Z.txt
0eec5ba227bf42cac7a6b230732cfe27dfffdc426a3b0271929d4d0add9516e4  onucus-9db.txt
8e8ac92d1a9b3678dc517674c9892ce292b7b92d63fb59b7c563588be90e89b5  kanit-9db-2/SHA256SUMS-9DB-2.txt
e8affee8be0a547bb02cbd2d9c5188bf2e73da6b100ec100d2930b402a92fbdb  kanit-9db-2/3c4e6b13-df05-442b-9bca-0aa8ae2abf48.log
c836555d827dc327cd271d5e6d572fdd8440a737b15b8c7213feee54a7c0f85f  kanit-9db-2/journal-trial.txt
ea66b555f36e4eeae7c59a8f54abbe5a4b9db9c5908303564d95f872ef0b48fb  kanit-9db-2/journal-known.txt
```

In the two after-boot reports of 9C the script's own read-only mount of the ESP failed, because the ESP was already
mounted at `/boot`; their file hashes are missing for that reason. The record was read through `/boot` instead,
and `esp-check` read it from the raw disk.
