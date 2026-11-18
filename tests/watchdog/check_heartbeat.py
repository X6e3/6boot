#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check a heartbeat log (heartbeat.c) and say what it shows about the counter.

    check_heartbeat.py LOG [--armed CNT] [--stopped] [--opened-by COMM] [--expect-feed]
                       [--wdt-hz HZ] [--wdt-tol FRACTION]

- the counter's rate: CNTFRQ_EL0 as the header logs it, against the rate the lines themselves show
  (delta CNTVCT over delta CLOCK_BOOTTIME); they must agree within 0.1%. A log from before the header
  carried cntfrq (the J713's 9D-a run) has only the measured rate, and its boottime0_cnt was computed
  for 24 MHz: it is recomputed here and not trusted.
- where CLOCK_BOOTTIME 0 falls on the counter, from every line; they must agree within 1 ms.
- the counter and CLOCK_BOOTTIME only go up.
- the kernel log's clock (the marks): where its 0 falls on the counter, from every mark; they must agree
  within 1 ms, which also says the kernel log's clock keeps to the counter. The kernel's watchdog records
  are then put on the counter too. A mark ties the two clocks to within its own width (the two counter
  reads around the write), and a record's timestamp is when the kernel logged it, not when the thing it
  describes happened.
- who had the watchdog open (the w lines), over every check.
- with --armed (xboot,watchdog's cntpct): the first and last heartbeat, and the kernel's watchdog records,
  after the watchdog was armed. The last heartbeat is the last that reached the disk, not the reset itself.
- with --stopped: the log must end with "stopped" (a clean stop, as in the host test).
- with --opened-by COMM (the host test): COMM must be seen holding the watchdog, and then nobody.
- the watchdog's own registers (the wdt header and the d lines), when the log has them:
  - the watchdog clock's rate, from CUR against the counter between reads with no feed in between; it
    must be --wdt-hz (24 MHz) within --wdt-tol (0.1 %);
  - every feed (CUR lower than it would have counted to): when it happened, on the counter, as the read
    after it minus CUR at the watchdog's rate; and the intervals between feeds;
  - every change of BITE or CTRL (RESET_EN is bit 2);
  - the time left at the last read, (BITE - CUR) at the watchdog's rate, and so when the watchdog would
    reset if nothing fed it again. With --armed all of these are on the arming's scale too.
  - with --expect-feed (the host test): at least one feed.
Also a check of the arithmetic on fixed values (24 MHz and 1 GHz). Exit 1 on any failure.
"""
import argparse
import re
import sys


def boottime0(cnt, boot_ns, frq):
    return cnt - boot_ns * frq // 1_000_000_000


def kernel0(us, c1, c2, frq):
    """the counter value at the kernel log's 0, from one mark"""
    return (c1 + c2) // 2 - us * frq // 1_000_000


def fed_at(c1, c2, cur, frq, hz):
    """the counter value when CUR was last set to 0, from one read (c1, c2 bracket the register reads)"""
    return (c1 + c2) // 2 - cur * frq // hz


def reset_at(c1, c2, cur, bite, frq, hz):
    """the counter value at which CUR would reach BITE if nothing fed the watchdog after this read"""
    return (c1 + c2) // 2 + (bite - cur) * frq // hz


def wdt_report(lines, frq, zero0, armed, hz, tol, expect_feed, last_hb, bad):
    hdr = next((l for l in lines if l.startswith("wdt ")), None)
    d = [tuple(int(v, 16) if i == 4 else int(v) for i, v in enumerate(l.split()[1:6]))
         for l in lines if re.match(r"d \d+ \d+ \d+ \d+ [0-9a-f]+$", l)]
    if hdr is None:
        print("watchdog registers: not in this log (heartbeat before they were read)")
        if expect_feed:
            bad.append("no wdt header")
        return
    print(f"watchdog registers: {hdr[4:]}")
    if not hdr.startswith("wdt node="):
        if expect_feed:
            bad.append("watchdog registers not read")
        return
    if len(d) < 2:
        bad.append("watchdog registers mapped but fewer than 2 reads")
        return
    on = (lambda c: f"armed +{(c - armed) / frq:9.3f} s") if armed is not None else \
         (lambda c: f"boottime {(c - zero0) / frq:9.3f} s")
    width = max(c2 - c1 for c1, c2, *_ in d) / frq
    print(f"  reads: {len(d)}, widest {width * 1e6:.1f} us")
    rates = [(y[2] - x[2]) * frq / ((y[0] + y[1] - x[0] - x[1]) / 2) for x, y in zip(d, d[1:])
             if y[2] >= x[2] and y[3] == x[3]]
    if rates:
        rate = sorted(rates)[len(rates) // 2]
        print(f"  watchdog clock measured from CUR: {rate:.1f} Hz over {len(rates)} intervals (expected {hz})")
        if abs(rate - hz) > hz * tol:
            bad.append(f"watchdog clock {rate:.1f} Hz is not {hz} Hz within {tol:.2%}")
    first = d[0]
    print(f"  first read ({on((first[0] + first[1]) // 2)}): CUR {first[2]} ({first[2] / hz:.3f} s), BITE {first[3]}"
          f" ({first[3] / hz:.3f} s), CTRL 0x{first[4]:x} (RESET_EN {'on' if first[4] & 4 else 'off'})")
    feeds = []
    for x, y in zip(d, d[1:]):
        expected = x[2] + (y[0] + y[1] - x[0] - x[1]) // 2 * hz // frq
        if y[2] < expected - hz // 1000:
            feeds.append(fed_at(y[0], y[1], y[2], frq, hz))
        if (y[3], y[4]) != (x[3], x[4]):
            print(f"  change at {on((y[0] + y[1]) // 2)}: BITE {x[3]} -> {y[3]} ({y[3] / hz:.3f} s),"
                  f" CTRL 0x{x[4]:x} -> 0x{y[4]:x}")
    print(f"  feeds seen: {len(feeds)}")
    for i, f in enumerate(feeds):
        gap = f"  (+{(f - feeds[i - 1]) / frq:.3f} s)" if i else ""
        print(f"    feed {i + 1:2d} at {on(f)}{gap}")
    last = d[-1]
    r = reset_at(last[0], last[1], last[2], last[3], frq, hz)
    print(f"  last read ({on((last[0] + last[1]) // 2)}): CUR {last[2] / hz:.3f} s of BITE {last[3] / hz:.3f} s,"
          f" CTRL 0x{last[4]:x}: {(last[3] - last[2]) / hz:.3f} s left")
    print(f"  unfed after it, the watchdog resets at {on(r)}"
          + (f"; the last heartbeat was at {on(last_hb)}" if last_hb is not None else ""))
    if expect_feed and not feeds:
        bad.append("no feed seen")


def arithmetic():
    # 1 GHz: the J713's 9D-a header, cntvct and boottime_ns as logged
    assert boottime0(16405620797, 1872361499, 1_000_000_000) == 14533259298
    # 24 MHz, as in the build VM: 2 s of boottime is 48,000,000 ticks
    assert boottime0(100_000_000, 2_000_000_000, 24_000_000) == 52_000_000
    # a long uptime does not overflow (Python ints do not; the C side uses __int128)
    assert boottime0(10**18, 10**17, 1_000_000_000) == 9 * 10**17
    # a mark at kernel 3 s, written between counter 5e9 and 5e9+2000 at 1 GHz: kernel 0 at 2e9+1000
    assert kernel0(3_000_000, 5_000_000_000, 5_000_002_000, 1_000_000_000) == 2_000_001_000
    # 24 MHz: kernel 1 s at counter 30e6 puts kernel 0 at 6e6
    assert kernel0(1_000_000, 30_000_000, 30_000_000, 24_000_000) == 6_000_000
    # a read at 1 GHz counter 100e9 (+-1000) shows CUR 12e6 at 24 MHz: the feed was 0.5 s earlier
    assert fed_at(100_000_000_000 - 1000, 100_000_000_000 + 1000, 12_000_000, 1_000_000_000, 24_000_000) == 99_500_000_000
    # BITE 30 s at 24 MHz, CUR 6 s: 24 s left
    assert reset_at(50_000_000_000, 50_000_000_000, 144_000_000, 720_000_000, 1_000_000_000, 24_000_000) == 74_000_000_000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--armed", type=int)
    ap.add_argument("--stopped", action="store_true")
    ap.add_argument("--opened-by")
    ap.add_argument("--expect-feed", action="store_true")
    ap.add_argument("--wdt-hz", type=int, default=24_000_000)
    ap.add_argument("--wdt-tol", type=float, default=1e-3)
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

    # the kernel log: marks, then its watchdog records on the counter
    marks = [tuple(int(x) for x in l.split()[1:4]) for l in lines if re.match(r"m \d+ \d+ \d+$", l)]
    failed = [l for l in lines if l.startswith("m - ")]
    kerr = next((l for l in lines if l.startswith("kmsg: ")), None)
    k0 = None
    if kerr:
        print(f"kernel log: not read ({kerr[6:]})")
    elif marks:
        z = [kernel0(us, x, y, frq) for us, x, y in marks]
        k0 = min(z)
        kspread = (max(z) - min(z)) / frq
        width = max(y - x for _, x, y in marks) / frq
        print(f"kernel log clock 0 on the counter: {k0} ({k0 / frq:.3f} s), spread over {len(marks)} marks"
              f" {kspread * 1e3:.3f} ms, widest mark {width * 1e6:.1f} us")
        print(f"kernel log clock 0 - CLOCK_BOOTTIME 0: {(k0 - min(zero)) / frq * 1e3:.3f} ms")
        if kspread > 1e-3:
            bad.append("the kernel log's clock does not keep to the counter (marks disagree by more than 1 ms)")
    elif any(l.startswith(("kfirst ", "k ")) for l in lines) or failed:
        bad.append("kernel log read but no mark read back")
    if failed:
        print(f"marks not read back: {len(failed)}")
    first = next((l.split(None, 3) for l in lines if l.startswith("kfirst ")), None)
    if first:
        print(f"kernel log: oldest record still there when read: seq {first[1]}"
              f" ({'nothing lost' if first[1] == '0' else 'the records before it were lost'}): {first[3] if len(first) > 3 else ''}")
    lost = sum(l == "klost" for l in lines)
    if lost:
        print(f"kernel log: records overwritten before they were read: {lost} times")
    krec = [l.split(None, 3) for l in lines if l.startswith("k ")]
    if krec:
        print(f"kernel log records about a watchdog: {len(krec)}")
    for r in krec:
        us, msg = int(r[2]), r[3] if len(r) > 3 else ""
        if k0 is not None and a.armed is not None:
            at = f"armed +{(k0 + us * frq // 1_000_000 - a.armed) / frq:8.3f} s"
        else:
            at = f"kernel {us / 1e6:10.6f} s"
        print(f"  {at}  {msg}")
        m = re.search(r"probe of (\S+) returned (-?\d+) after (\d+) usecs", msg)
        if m and "watchdog" in m.group(1) and k0 is not None:
            p = k0 + us * frq // 1_000_000
            print(f"  {'':16s}  -> last heartbeat: {(c1 - p) / frq:.3f} s after this record")

    # who had the watchdog open
    w = [l.split(None, 3) for l in lines if l.startswith("w ")]
    if not w:
        print("watchdog open checks: none in this log (heartbeat before they were logged)")
    else:
        held = [x for x in w if len(x) > 3 and x[3] != "none"]
        if not held:
            print(f"watchdog open in user space: nobody, at every check ({len(ts)} checks)")
        for x in w:
            print(f"  w {int(x[2]) / 1e9:9.3f} s boottime: {x[3] if len(x) > 3 else ''}")
        if a.opened_by:
            by = [i for i, x in enumerate(w) if len(x) > 3 and f"comm={a.opened_by} " in x[3] + " "]
            if not by or not any(len(x) > 3 and x[3] == "none" for x in w[by[0] + 1:]):
                bad.append(f"{a.opened_by} not seen holding the watchdog and then letting go")
    if a.opened_by and not w:
        bad.append("no watchdog open checks")

    wdt_report(lines, frq, min(zero), a.armed, a.wdt_hz, a.wdt_tol, a.expect_feed, c1, bad)

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
