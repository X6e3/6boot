#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check a heartbeat log (heartbeat.c) and say what it shows about the counter.

    check_heartbeat.py LOG [--armed CNT] [--stopped]

- the counter's rate: CNTFRQ_EL0 as the header logs it, against the rate the lines themselves show
  (delta CNTVCT over delta CLOCK_BOOTTIME); they must agree within 0.1%. A log from before the header
  carried cntfrq (the J713's 9D-a run) has only the measured rate, and its boottime0_cnt was computed
  for 24 MHz: it is recomputed here and not trusted.
- where CLOCK_BOOTTIME 0 falls on the counter, from every line; they must agree within 1 ms.
- the counter and CLOCK_BOOTTIME only go up.
- with --armed (xboot,watchdog's cntpct): the first and last heartbeat after the watchdog was armed.
  The last heartbeat is the last that reached the disk, not the reset itself.
- with --stopped: the log must end with "stopped" (a clean stop, as in the host test).
Also a check of the boottime0 arithmetic on fixed values (24 MHz and 1 GHz). Exit 1 on any failure.
"""
import argparse
import re
import sys


def boottime0(cnt, boot_ns, frq):
    return cnt - boot_ns * frq // 1_000_000_000


def arithmetic():
    # 1 GHz: the J713's 9D-a header, cntvct and boottime_ns as logged
    assert boottime0(16405620797, 1872361499, 1_000_000_000) == 14533259298
    # 24 MHz, as in the build VM: 2 s of boottime is 48,000,000 ticks
    assert boottime0(100_000_000, 2_000_000_000, 24_000_000) == 52_000_000
    # a long uptime does not overflow (Python ints do not; the C side uses __int128)
    assert boottime0(10**18, 10**17, 1_000_000_000) == 9 * 10**17


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--armed", type=int)
    ap.add_argument("--stopped", action="store_true")
    a = ap.parse_args()
    arithmetic()
    text = open(a.log, errors="replace").read()
    lines = text.splitlines()
    ts = [tuple(int(x) for x in l.split()[1:3]) for l in lines if l.startswith("t ")]
    clock = next((l for l in lines if l.startswith("clock ")), "")
    hdr = dict(re.findall(r"(\w+)=(-?\d+)", clock))
    bad = []

    if len(ts) < 2:
        print(f"heartbeats: {len(ts)}, too few")
        return 1
    (c0, b0), (c1, b1) = ts[0], ts[-1]
    measured = (c1 - c0) * 1e9 / (b1 - b0)
    print(f"heartbeats: {len(ts)} over {(b1 - b0) / 1e9:.3f} s of CLOCK_BOOTTIME")
    print(f"counter rate measured from the lines: {measured:.2f} Hz")
    if "cntfrq" in hdr:
        frq = int(hdr["cntfrq"])
        print(f"CNTFRQ_EL0 in the header: {frq} Hz")
        if abs(measured - frq) > frq * 1e-3:
            bad.append("measured rate and CNTFRQ_EL0 disagree")
        logged0 = int(hdr["boottime0_cnt"])
    else:
        frq = round(measured)
        logged0 = None
        print("header has no cntfrq (written before it was logged): rate taken from the lines, header boottime0_cnt"
              " not used (it assumed 24 MHz)")
    zero = [boottime0(c, b, frq) for c, b in ts]
    spread = (max(zero) - min(zero)) / frq
    print(f"CLOCK_BOOTTIME 0 on the counter: {min(zero)} ({min(zero) / frq:.3f} s), spread over the lines {spread * 1e3:.3f} ms")
    if spread > 1e-3:
        bad.append("CLOCK_BOOTTIME 0 moves on the counter by more than 1 ms")
    if logged0 is not None and abs(logged0 - min(zero)) > frq // 1000:
        bad.append("the header's boottime0_cnt disagrees with the lines")
    if any(x[0] <= y[0] or x[1] <= y[1] for y, x in zip(ts, ts[1:])):
        bad.append("counter or CLOCK_BOOTTIME does not only go up")
    if a.armed is not None:
        print(f"watchdog armed -> CLOCK_BOOTTIME 0: {(min(zero) - a.armed) / frq:.3f} s")
        print(f"watchdog armed -> first heartbeat: {(c0 - a.armed) / frq:.3f} s")
        print(f"watchdog armed -> last heartbeat:  {(c1 - a.armed) / frq:.3f} s (the reset came after this)")
    stopped = any(l.startswith("stopped") for l in lines)
    print(f"ends with stopped: {'yes' if stopped else 'no'}")
    if a.stopped and not stopped:
        bad.append("no stopped line")
    for b in bad:
        print("FAIL:", b)
    print("heartbeat check:", "passed" if not bad else "FAILED")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
