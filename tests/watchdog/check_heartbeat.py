#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check a heartbeat log (heartbeat.c) and say what it shows, and what it does not.

    check_heartbeat.py LOG [--armed CNT] [--stopped] [--opened-by COMM] [--expect-feed] [--strict]
                       [--wdt-hz HZ] [--wdt-tol FRACTION] [--cur-tol-ms MS] [--gap-s S]
                       [--next-boot-realtime UNIX_S] [--json OUT]

Every finding is one of: an observation (what the log holds), an inference (computed from it, with its
error bound), or a PREDICTION (what would happen if nothing changed; never an observation). Problems
are FAIL (the log contradicts itself: exit 1) or WARN (something unexpected that the log does show, such
as a gap or a jump; exit 0 unless --strict).

The log's own clocks:
- the counter (CNTVCT_EL0): its rate is CNTFRQ_EL0 as the header logs it; the lines must show the same
  rate within 0.1 %. A log from before the header carried cntfrq (the J713's 9D-a run) has only the
  measured rate, and its boottime0_cnt was computed for 24 MHz: it is recomputed here and not trusted.
- CLOCK_BOOTTIME: where its 0 falls on the counter, from every line; they must agree within 1 ms (FAIL).
- the counter and CLOCK_BOOTTIME only go up (FAIL).
- heartbeat gaps: two heartbeats further apart than --gap-s (1.5 s) of CLOCK_BOOTTIME (WARN).
- CLOCK_REALTIME (the wall clock): REALTIME - BOOTTIME should stay put; every step over 50 ms is a WARN
  and the wall clock is tied to the counter only within the segments between steps.
- the kernel log's clock (the marks): where its 0 falls on the counter; marks must agree within 1 ms.
  A record's timestamp is when the kernel logged it, not when the thing it describes happened.
- who had the watchdog open (the w lines).

The watchdog's own registers (the wdt header and the d lines: CUR, BITE, CTRL of WD1), when present:
- every read: its two counter reads must be in order (FAIL); a read slower than 1 ms is a WARN (a bus
  that stalls is what the J713's earlier hard hangs looked like from the inside); every heartbeat line
  must be followed by its read; a log that ends between the two is a WARN (the read may not have
  returned).
- between two reads, CUR is classified against what the watchdog clock (--wdt-hz) would have added:
  count (as expected), zeroed (went down, and low enough to have been set to 0 between the reads: a
  feed or any other write of 0), wrap (past 2^32 back to near 0, as a 32-bit counter would), stalled
  (did not move), slow (went up, but less than its clock adds: stopped for part of the time, or written
  0 when it was already low), fast (moved more than its clock can), lowered (went down, but not as a
  write of 0 could explain). The last four are WARN. A zeroing is put on the counter (the read after it minus CUR at the measured
  rate) with an error bound. What wrote the 0 is not in the log: the kernel's ping, set_timeout, start
  and restart all write 0 to CUR (apple_wdt.c); the BITE and CTRL changes in the same interval and the
  spacing of the zeroings are given next to it, as evidence, not as proof.
- BITE and CTRL changes are their own events. CUR >= BITE with RESET_EN on while the log goes on is a
  WARN (the watchdog should have reset the machine).
- the watchdog clock against the counter: a straight-line fit of CUR on the counter within every stretch
  without a zeroing, wrap or change; the rate and its spread, and the largest residual, are the error
  bound of every time computed from CUR.
- the PREDICTION from the last read: when CUR would reach BITE if nothing wrote it again, with its bound.
  The reset itself is not in this log. With --next-boot-realtime (the wall clock of the next boot's first
  record, from that boot), the prediction is compared with the window the two boots leave for the reset:
  after the last heartbeat, before the next boot. That is a separate, coarse observation.
- with --expect-feed (the host test): at least one zeroing.
At the end, the chain 6boot armed -> Linux took the watchdog over -> fed -> feeds stopped -> heartbeat
stops -> (reset) -> (fallback), with what in this log speaks for each link.
Also a check of the arithmetic on fixed values (24 MHz and 1 GHz).
"""
import argparse
import json
import re
import statistics
import sys

U32 = 1 << 32


def boottime0(cnt, boot_ns, frq):
    return cnt - boot_ns * frq // 1_000_000_000


def kernel0(us, c1, c2, frq):
    """the counter value at the kernel log's 0, from one mark"""
    return (c1 + c2) // 2 - us * frq // 1_000_000


def fed_at(c1, c2, cur, frq, hz):
    """the counter value when CUR was last set to 0, from one read (c1, c2 bracket the register reads)"""
    return (c1 + c2) // 2 - cur * frq // hz


def reset_at(c1, c2, cur, bite, frq, hz):
    """the counter value at which CUR would reach BITE if nothing wrote it after this read"""
    return (c1 + c2) // 2 + (bite - cur) * frq // hz


def classify(x, y, frq, hz, tol_ticks, rate_tol):
    """What CUR did between reads x and y, each (c1, c2, cur, bite, ctrl). Returns (kind, detail)."""
    mid_x, mid_y = (x[0] + x[1]) / 2, (y[0] + y[1]) / 2
    e = (mid_y - mid_x) / frq * hz                          # what the watchdog clock adds between the reads
    tol = tol_ticks + abs(e) * rate_tol
    inc = y[2] - x[2]
    if abs(inc - e) <= tol:
        return "count", None
    if abs(inc) <= tol_ticks and e > 2 * tol:
        return "stalled", f"CUR stayed at {y[2]} while its clock would have added {e:.0f}"
    since = (y[1] - x[0]) / frq * hz                        # the most CUR can show if it was 0 after read x
    if inc < 0:                                             # it went down
        if x[2] + e >= U32 - tol and abs(x[2] + e - U32 - y[2]) <= tol:
            return "wrap", None
        if y[2] <= since + tol:
            return "zeroed", None
        return "lowered", f"CUR {x[2]} -> {y[2]}: lower, but more than a write of 0 after the previous read allows"
    if inc < e - tol:                                       # it went up, but less than its clock adds
        alt = "; a write of 0 between the reads would also explain it" if y[2] <= since + tol else ""
        return "slow", f"CUR {x[2]} -> {y[2]}: {e - inc:.0f} less than its clock adds (stopped for part of the time{alt})"
    return "fast", f"CUR {x[2]} -> {y[2]}: {inc - e:.0f} more than its clock adds in that time"


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
    g, h = 1_000_000_000, 24_000_000
    r = lambda c, cur, bite=720_000_000, ctrl=4: (c, c + 1000, cur, bite, ctrl)
    assert classify(r(0, 0), r(g // 2, h // 2), g, h, h // 1000, 1e-3)[0] == "count"
    assert classify(r(0, 240_000_000), r(g // 2, h // 4), g, h, h // 1000, 1e-3)[0] == "zeroed"
    assert classify(r(0, U32 - h // 4, ctrl=0), r(g // 2, h // 4), g, h, h // 1000, 1e-3)[0] == "wrap"
    assert classify(r(0, 5_000_000), r(g // 2, 5_000_000), g, h, h // 1000, 1e-3)[0] == "stalled"
    assert classify(r(0, 240_000_000), r(g // 2, 200_000_000), g, h, h // 1000, 1e-3)[0] == "lowered"
    assert classify(r(0, 240_000_000), r(g // 2, 245_000_000), g, h, h // 1000, 1e-3)[0] == "slow"
    assert classify(r(0, 0), r(g // 2, h), g, h, h // 1000, 1e-3)[0] == "fast"


class Report:
    def __init__(self):
        self.flags, self.events, self.data = [], [], {}

    def fail(self, code, text):
        self.flags.append({"level": "FAIL", "code": code, "text": text})

    def warn(self, code, text):
        self.flags.append({"level": "WARN", "code": code, "text": text})


def parse(lines):
    p = {"t": [], "d": [], "marks": [], "mark_failed": 0, "k": [], "w": [], "order": []}
    for i, l in enumerate(lines):
        if l.startswith("t "):
            f = l.split()
            rt = f[3] if len(f) > 3 else None
            p["t"].append((int(f[1]), int(f[2]), rt))
            p["order"].append(("t", len(p["t"]) - 1, i))
        elif re.match(r"d \d+ \d+ \d+ \d+ [0-9a-f]+$", l):
            f = l.split()
            p["d"].append((int(f[1]), int(f[2]), int(f[3]), int(f[4]), int(f[5], 16)))
            p["order"].append(("d", len(p["d"]) - 1, i))
        elif re.match(r"m \d+ \d+ \d+$", l):
            p["marks"].append(tuple(int(x) for x in l.split()[1:4]))
        elif l.startswith("m - "):
            p["mark_failed"] += 1
        elif l.startswith("k "):
            p["k"].append(l.split(None, 3))
        elif l.startswith("w "):
            p["w"].append(l.split(None, 3))
    p["clock"] = dict(re.findall(r"(\w+)=(-?\d+)", next((l for l in lines if l.startswith("clock ")), "")))
    p["wdt"] = next((l for l in lines if l.startswith("wdt ")), None)
    p["kerr"] = next((l for l in lines if l.startswith("kmsg: ")), None)
    p["kfirst"] = next((l.split(None, 3) for l in lines if l.startswith("kfirst ")), None)
    p["klost"] = sum(l == "klost" for l in lines)
    p["stopped"] = any(l.startswith("stopped") for l in lines)
    p["chosen_wdt"] = next((l.split("=", 1)[1].strip() for l in lines if l.startswith("chosen xboot,watchdog")), None)
    p["wdog_hdr"] = next((l for l in lines if l.startswith("watchdog dev:")), None)
    return p


def realtime_ns(s):
    sec, _, frac = s.partition(".")
    return int(sec) * 1_000_000_000 + int((frac + "000000000")[:9])


def check_time(p, rep, gap_s):
    ts = p["t"]
    (c0, b0, _), (c1, b1, _) = ts[0], ts[-1]
    measured = (c1 - c0) * 1e9 / (b1 - b0)
    print(f"heartbeats: {len(ts)} over {(b1 - b0) / 1e9:.3f} s of CLOCK_BOOTTIME")
    print(f"counter rate measured from the lines: {measured:.2f} Hz")
    hdr = p["clock"]
    if "cntfrq" in hdr:
        frq = int(hdr["cntfrq"])
        print(f"CNTFRQ_EL0 in the header: {frq} Hz")
        if abs(measured - frq) > frq * 1e-3:
            rep.fail("cntfrq", "measured rate and CNTFRQ_EL0 disagree")
        logged0 = int(hdr["boottime0_cnt"])
    else:
        frq = round(measured)
        logged0 = None
        print("header has no cntfrq (written before it was logged): rate taken from the lines, header boottime0_cnt"
              " not used (it assumed 24 MHz)")
    zero = [boottime0(c, b, frq) for c, b, _ in ts]
    z0 = min(zero)
    spread = (max(zero) - z0) / frq
    print(f"CLOCK_BOOTTIME 0 on the counter: {z0} ({z0 / frq:.3f} s), spread over the lines {spread * 1e3:.3f} ms")
    if spread > 1e-3:
        rep.fail("boottime", "CLOCK_BOOTTIME 0 moves on the counter by more than 1 ms")
    if logged0 is not None and abs(logged0 - z0) > frq // 1000:
        rep.fail("boottime0_cnt", "the header's boottime0_cnt disagrees with the lines")
    if any(x[0] <= y[0] or x[1] <= y[1] for y, x in zip(ts, ts[1:])):
        rep.fail("monotonic", "counter or CLOCK_BOOTTIME does not only go up")
    gaps = [(y[1] - x[1]) / 1e9 for x, y in zip(ts, ts[1:])]
    med = statistics.median(gaps) if gaps else 0
    for i, g in enumerate(gaps):
        if g > gap_s:
            rep.warn("gap", f"heartbeat gap of {g:.3f} s after boottime {ts[i][1] / 1e9:.3f} s"
                            f" (about {round(g / med) - 1 if med else '?'} heartbeats missing)")
    print(f"heartbeat interval: median {med * 1e3:.3f} ms, largest {max(gaps) * 1e3 if gaps else 0:.3f} ms")
    # the wall clock against CLOCK_BOOTTIME: steps, and the segments between them
    rts = [(c, b, realtime_ns(r)) for c, b, r in ts if r]
    segs, start = [], 0
    for i in range(1, len(rts)):
        step = (rts[i][2] - rts[i][1]) - (rts[i - 1][2] - rts[i - 1][1])
        if abs(step) > 50_000_000:
            rep.warn("realtime_step", f"wall clock stepped {step / 1e9:+.3f} s between boottime"
                                      f" {rts[i - 1][1] / 1e9:.3f} and {rts[i][1] / 1e9:.3f} s")
            segs.append((start, i))
            start = i
    segs.append((start, len(rts)))
    wall = []
    for a, b in segs:
        off = [r - c * 1_000_000_000 // frq for c, _, r in rts[a:b]]    # wall ns minus counter ns
        wall.append({"from_line": a, "to_line": b - 1, "offset_ns": min(off), "spread_ns": max(off) - min(off)})
    if rts:
        print(f"wall clock: {len(segs)} segment(s) between steps; within each, wall - counter spreads at most"
              f" {max(w['spread_ns'] for w in wall) / 1e6:.3f} ms")
    rep.data.update({"cntfrq": frq, "boottime0_cnt": z0, "wall_segments": wall})
    return frq, z0, c1


def check_kernel(p, rep, frq, z0, armed, c_last):
    marks, failed = p["marks"], p["mark_failed"]
    k0 = None
    if p["kerr"]:
        print(f"kernel log: not read ({p['kerr'][6:]})")
    elif marks:
        z = [kernel0(us, x, y, frq) for us, x, y in marks]
        k0 = min(z)
        kspread = (max(z) - min(z)) / frq
        width = max(y - x for _, x, y in marks) / frq
        print(f"kernel log clock 0 on the counter: {k0} ({k0 / frq:.3f} s), spread over {len(marks)} marks"
              f" {kspread * 1e3:.3f} ms, widest mark {width * 1e6:.1f} us")
        print(f"kernel log clock 0 - CLOCK_BOOTTIME 0: {(k0 - z0) / frq * 1e3:.3f} ms")
        if kspread > 1e-3:
            rep.fail("kmarks", "the kernel log's clock does not keep to the counter (marks disagree by more than 1 ms)")
    elif p["kfirst"] or p["k"] or failed:
        rep.fail("kmarks", "kernel log read but no mark read back")
    if failed:
        print(f"marks not read back: {failed}")
    if p["kfirst"]:
        f = p["kfirst"]
        print(f"kernel log: oldest record still there when read: seq {f[1]}"
              f" ({'nothing lost' if f[1] == '0' else 'the records before it were lost'}): {f[3] if len(f) > 3 else ''}")
    if p["klost"]:
        print(f"kernel log: records overwritten before they were read: {p['klost']} times")
    if p["k"]:
        print(f"kernel log records about a watchdog: {len(p['k'])}")
    probe = None
    for r in p["k"]:
        us, msg = int(r[2]), r[3] if len(r) > 3 else ""
        at = f"armed +{(k0 + us * frq // 1_000_000 - armed) / frq:8.3f} s" if k0 is not None and armed is not None \
            else f"kernel {us / 1e6:10.6f} s"
        print(f"  {at}  {msg}")
        m = re.search(r"probe of (\S+) returned (-?\d+) after (\d+) usecs", msg)
        if m and "watchdog" in m.group(1) and k0 is not None:
            probe = k0 + us * frq // 1_000_000
            print(f"  {'':16s}  -> last heartbeat: {(c_last - probe) / frq:.3f} s after this record")
    rep.data["driver_probe_cnt"] = probe
    return probe


def check_openers(p, rep, opened_by, n_ts):
    w = p["w"]
    if not w:
        print("watchdog open checks: none in this log (heartbeat before they were logged)")
        if opened_by:
            rep.fail("openers", "no watchdog open checks")
        return None
    held = [x for x in w if len(x) > 3 and x[3] != "none"]
    if not held:
        print(f"watchdog open in user space: nobody, at every check ({n_ts} checks)")
    for x in w:
        print(f"  w {int(x[2]) / 1e9:9.3f} s boottime: {x[3] if len(x) > 3 else ''}")
    if opened_by:
        by = [i for i, x in enumerate(w) if len(x) > 3 and f"comm={opened_by} " in x[3] + " "]
        if not by or not any(len(x) > 3 and x[3] == "none" for x in w[by[0] + 1:]):
            rep.fail("openers", f"{opened_by} not seen holding the watchdog and then letting go")
    return not held


def fit(points):
    """least squares y = a + b x over (x, y); (b, largest residual)"""
    n = len(points)
    mx = sum(x for x, _ in points) / n
    my = sum(y for _, y in points) / n
    sxx = sum((x - mx) ** 2 for x, _ in points)
    b = sum((x - mx) * (y - my) for x, y in points) / sxx
    a = my - b * mx
    return b, max(abs(y - a - b * x) for x, y in points)


def check_wdt(p, rep, frq, z0, armed, a):
    d, hdr = p["d"], p["wdt"]
    hz = a.wdt_hz
    out = {"header": hdr, "reads": len(d), "intervals": {}, "zeroings": [], "changes": []}
    rep.data["wdt"] = out
    if hdr is None:
        print("watchdog registers: not in this log (heartbeat before they were read)")
        if a.expect_feed:
            rep.fail("wdt_missing", "no wdt header")
        return None
    print(f"watchdog registers: {hdr[4:]}")
    if not hdr.startswith("wdt node="):
        if d:
            rep.fail("wdt_lines", "register reads in a log whose header says they are off")
        if a.expect_feed:
            rep.fail("wdt_missing", "watchdog registers not read")
        return None
    on = (lambda c: f"armed +{(c - armed) / frq:9.3f} s") if armed is not None else \
         (lambda c: f"boottime {(c - z0) / frq:9.3f} s")
    # every heartbeat followed by its read, reads in order, brackets in order
    order = p["order"]
    for i, (kind, j, _) in enumerate(order):
        if kind == "t":
            nxt = order[i + 1][0] if i + 1 < len(order) else None
            if nxt != "d":
                if i + 1 == len(order):
                    rep.warn("ends_before_read", f"the log ends between a heartbeat (counter {p['t'][j][0]}) and its"
                                                 " register read: the read may not have returned")
                else:
                    rep.fail("read_missing", f"heartbeat at counter {p['t'][j][0]} has no register read after it")
        elif kind == "d":
            prev = order[i - 1] if i else None
            if not prev or prev[0] != "t" or d[j][0] < p["t"][prev[1]][0]:
                rep.fail("read_order", f"register read {j} does not follow its heartbeat")
    if any(x[0] > x[1] for x in d):
        rep.fail("bracket", "a read's two counter values are out of order")
    if any(y[0] <= x[1] for x, y in zip(d, d[1:])):
        rep.fail("read_monotonic", "register reads do not follow each other on the counter")
    if len(d) < 2:
        rep.fail("wdt_few", "watchdog registers mapped but fewer than 2 reads")
        return None
    widths = [(x[1] - x[0]) / frq for x in d]
    print(f"  reads: {len(d)}, read width median {statistics.median(widths) * 1e6:.1f} us,"
          f" widest {max(widths) * 1e6:.1f} us")
    for x, w in zip(d, widths):
        if w > 1e-3:
            rep.warn("slow_read", f"a register read took {w * 1e3:.3f} ms at {on(x[0])}")
    first = d[0]
    print(f"  first read ({on(first[0])}): CUR {first[2]} ({first[2] / hz:.3f} s), BITE {first[3]}"
          f" ({first[3] / hz:.3f} s), CTRL 0x{first[4]:x} ({ctrl_text(first[4])})")
    # classify every interval with the nominal clock; then fit the clock on the stretches that only counted
    tol_ticks = round(a.cur_tol_ms * 1e-3 * hz)
    kinds = [classify(x, y, frq, hz, tol_ticks, a.wdt_tol) for x, y in zip(d, d[1:])]
    stretches, cur = [], [d[0]]
    for (k, _), x, y in zip(kinds, d, d[1:]):
        if k == "count" and (y[3], y[4]) == (x[3], x[4]):
            cur.append(y)
        else:
            stretches.append(cur)
            cur = [y]
    stretches.append(cur)
    slopes, resid = [], []
    for s in stretches:
        if len(s) >= 3:
            b, r = fit([((x[0] + x[1]) / 2 / frq, x[2]) for x in s])
            slopes.append(b)
            resid.append(r)
    rate, rate_err, res = hz, None, None
    if slopes:
        rate = statistics.median(slopes)
        rate_err = (max(slopes) - min(slopes)) / 2 if len(slopes) > 1 else 0.0
        res = max(resid)
        print(f"  watchdog clock against the counter: {rate:.1f} Hz (expected {hz}), from {len(slopes)} stretch(es)"
              f" without a zeroing; slopes spread +-{rate_err:.1f} Hz, largest residual {res:.0f} ticks"
              f" ({res / hz * 1e6:.1f} us)")
        if abs(rate - hz) > hz * a.wdt_tol:
            rep.fail("wdt_rate", f"watchdog clock {rate:.1f} Hz is not {hz} Hz within {a.wdt_tol:.2%}")
    else:
        print("  watchdog clock against the counter: no stretch of 3 reads without a zeroing; nominal rate used")
    out.update({"rate_hz": rate, "rate_spread_hz": rate_err, "fit_residual_ticks": res})
    rel = (abs(rate - hz) / hz if rate_err is None else (rate_err / rate)) + 1e-6

    def bound(x, cur_ticks):
        """error of a time computed from CUR at read x: half the read, the rate's error, the fit residual"""
        return (x[1] - x[0]) / 2 / frq + cur_ticks / rate * rel + (res or tol_ticks) / rate

    counts = {}
    prev_zero = None
    for (k, why), x, y in zip(kinds, d, d[1:]):
        counts[k] = counts.get(k, 0) + 1
        if (y[3], y[4]) != (x[3], x[4]):
            if y[3] != x[3]:
                ev = {"kind": "bite", "cnt": y[0], "from": x[3], "to": y[3]}
                out["changes"].append(ev)
                rep.events.append(ev)
                print(f"  BITE change at {on(y[0])}: {x[3]} ({x[3] / hz:.3f} s) -> {y[3]} ({y[3] / hz:.3f} s)")
            if y[4] != x[4]:
                ev = {"kind": "ctrl", "cnt": y[0], "from": x[4], "to": y[4]}
                out["changes"].append(ev)
                rep.events.append(ev)
                print(f"  CTRL change at {on(y[0])}: 0x{x[4]:x} ({ctrl_text(x[4])}) -> 0x{y[4]:x} ({ctrl_text(y[4])})")
        if k == "zeroed":
            at = fed_at(y[0], y[1], y[2], frq, rate)
            b = bound(y, y[2])
            with_ = []
            if y[3] != x[3]:
                with_.append("BITE changed too (as set_timeout writes CUR 0, then BITE)")
            if not (x[4] & 4) and y[4] & 4:
                with_.append("RESET_EN came on too (as start writes CUR 0, then CTRL)")
            if y[3] == x[3] and y[4] == x[4]:
                with_.append("BITE and CTRL unchanged (as a ping writes CUR 0 and nothing else)")
            gap = (at - prev_zero) / frq if prev_zero is not None else None
            ev = {"kind": "zeroed", "cnt": at, "bound_s": b, "since_previous_s": gap, "with": with_}
            out["zeroings"].append(ev)
            rep.events.append(ev)
            prev_zero = at
        elif k in ("stalled", "slow", "fast", "lowered"):
            rep.warn(f"cur_{k}", f"between {on(x[0])} and {on(y[0])}: {why}")
            rep.events.append({"kind": k, "cnt": y[0]})
        elif k == "wrap":
            print(f"  CUR wrapped past 2^32 between {on(x[0])} and {on(y[0])}"
                  f" (RESET_EN {'on' if y[4] & 4 else 'off'})")
            rep.events.append({"kind": "wrap", "cnt": y[0]})
            if y[4] & 4 and y[3] and x[3] == y[3]:
                rep.warn("wrap_past_bite", "CUR wrapped with RESET_EN on: it went past BITE without a reset")
    for x in d:
        if x[4] & 4 and x[3] and x[2] >= x[3]:
            rep.warn("past_bite", f"CUR {x[2]} >= BITE {x[3]} with RESET_EN on at {on(x[0])}, and the log goes on")
            break
    out["intervals"] = counts
    print(f"  intervals: {', '.join(f'{k} {v}' for k, v in sorted(counts.items()))}")
    zs = out["zeroings"]
    print(f"  zeroings of CUR (it went down to what a write of 0 between two reads explains; a write of another"
          f" small value would look the same; the log does not say who wrote it): {len(zs)}")
    for i, z in enumerate(zs):
        gap = f"  (+{z['since_previous_s']:.3f} s)" if z["since_previous_s"] is not None else ""
        print(f"    {i + 1:2d} at {on(z['cnt'])} +- {z['bound_s'] * 1e3:.3f} ms{gap}; {'; '.join(z['with'])}")
    last = d[-1]
    if last[4] & 4:
        r = reset_at(last[0], last[1], last[2], last[3], frq, rate)
        b = bound(last, max(last[3] - last[2], 0))
        out["prediction"] = {"cnt": r, "bound_s": b}
        print(f"  PREDICTION (not an observation): from the last read ({on(last[0])}: CUR {last[2] / hz:.3f} s of"
              f" BITE {last[3] / hz:.3f} s), if nothing wrote CUR again the watchdog would reset the machine at"
              f" {on(r)} +- {b * 1e3:.3f} ms")
    else:
        out["prediction"] = None
        print(f"  PREDICTION: none; RESET_EN is off at the last read (CTRL 0x{last[4]:x}), WD1 would not reset")
    if a.expect_feed and not zs:
        rep.fail("no_feed", "no feed seen")
    return out


def ctrl_text(c):
    bits = [n for b, n in ((4, "RESET_EN"), (1, "IRQ_EN"), (2, "IRQ_STATUS")) if c & b]
    other = c & ~7
    return " ".join(bits + ([f"other 0x{other:x}"] if other else [])) or "none"


def chain(p, rep, frq, armed, probe, nobody, wdt, c_last, a):
    print("chain (what this log holds for each link; the reset and the fallback are not in it):")
    cw = p["chosen_wdt"]
    print(f"  1 6boot armed the watchdog:        {'6boot says ' + cw if cw and cw != '(none)' else 'no xboot,watchdog in /chosen'}")
    took = []
    if probe is not None:
        took.append("the kernel log has the watchdog's probe")
    if p["wdog_hdr"]:
        took.append(p["wdog_hdr"].split(", ", 1)[-1])
    if wdt and wdt.get("reads"):
        b0 = p["d"][0][3]
        took.append(f"first read BITE = {b0 / a.wdt_hz:.3f} s ({'the driver writes 30 s' if b0 == 30 * a.wdt_hz else 'not the driver default 30 s'})")
    print(f"  2 Linux took it over:              {'; '.join(took) if took else 'nothing in this log'}")
    print(f"  3 nobody in user space opened it:  {'nobody at any check' if nobody else ('someone did' if nobody is False else 'not checked')}")
    zs = (wdt or {}).get("zeroings", [])
    gaps = [z["since_previous_s"] for z in zs if z["since_previous_s"] is not None]
    feeds = f"{len(zs)} zeroings of CUR" + (f", spacing {min(gaps):.3f}-{max(gaps):.3f} s" if gaps else "")
    print(f"  4 the kernel fed it:               {feeds if wdt else 'registers not read'}")
    last_z = zs[-1]["cnt"] if zs else None
    on = (lambda c: f"armed +{(c - armed) / frq:.3f} s") if armed is not None else (lambda c: f"counter {c}")
    print(f"  5 feeds stopped:                   {'last zeroing at ' + on(last_z) if last_z else 'no zeroing seen'}"
          + (f", reads went on {(p['d'][-1][0] - last_z) / frq:.3f} s after it" if last_z and p["d"] else ""))
    print(f"  6 the heartbeat stopped:           last heartbeat on disk at {on(c_last)}"
          f"{' (clean stop)' if p['stopped'] else ''}: the reset, if any, came after it")
    pr = (wdt or {}).get("prediction")
    print(f"  7 PREDICTED reset:                 {on(pr['cnt']) + ' +- %.3f ms' % (pr['bound_s'] * 1e3) if pr else 'none'}")
    if a.next_boot_realtime is not None:
        segs = rep.data.get("wall_segments") or []
        if segs and pr:
            off = segs[-1]["offset_ns"]
            wall_last = (c_last * 1_000_000_000 // frq + off) / 1e9
            wall_pred = (pr["cnt"] * 1_000_000_000 // frq + off) / 1e9
            nb = a.next_boot_realtime
            ok = wall_last <= wall_pred <= nb
            print(f"  8 next boot (separate observation): last heartbeat wall {wall_last:.3f}, predicted reset wall"
                  f" {wall_pred:.3f}, next boot's first record {nb:.3f}: the prediction "
                  f"{'falls' if ok else 'does NOT fall'} in the window the two boots leave")
            if not ok:
                rep.warn("next_boot", "the predicted reset is outside the window between the last heartbeat and the next boot")
        else:
            print("  8 next boot: no wall clock or no prediction to compare")
    else:
        print("  8 next boot (reset happened, 6boot fell back): not in this log; 6boot's next-boot report and the"
              " next boot's own records")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--armed", type=int)
    ap.add_argument("--stopped", action="store_true")
    ap.add_argument("--opened-by")
    ap.add_argument("--expect-feed", action="store_true")
    ap.add_argument("--strict", action="store_true", help="WARN fails too")
    ap.add_argument("--wdt-hz", type=int, default=24_000_000)
    ap.add_argument("--wdt-tol", type=float, default=1e-3)
    ap.add_argument("--cur-tol-ms", type=float, default=1.0, help="slack on CUR between reads, in ms of its clock")
    ap.add_argument("--gap-s", type=float, default=1.5)
    ap.add_argument("--next-boot-realtime", type=float)
    ap.add_argument("--json")
    a = ap.parse_args()
    arithmetic()
    lines = open(a.log, errors="replace").read().splitlines()
    p = parse(lines)
    rep = Report()
    if len(p["t"]) < 2:
        print(f"heartbeats: {len(p['t'])}, too few")
        return 1
    frq, z0, c_last = check_time(p, rep, a.gap_s)
    probe = check_kernel(p, rep, frq, z0, a.armed, c_last)
    nobody = check_openers(p, rep, a.opened_by, len(p["t"]))
    wdt = check_wdt(p, rep, frq, z0, a.armed, a)
    if a.armed is not None:
        print(f"watchdog armed -> CLOCK_BOOTTIME 0: {(z0 - a.armed) / frq:.3f} s")
        print(f"watchdog armed -> first heartbeat: {(p['t'][0][0] - a.armed) / frq:.3f} s")
        print(f"watchdog armed -> last heartbeat:  {(c_last - a.armed) / frq:.3f} s (the reset came after this)")
    print(f"ends with stopped: {'yes' if p['stopped'] else 'no'}")
    if a.stopped and not p["stopped"]:
        rep.fail("stopped", "no stopped line")
    chain(p, rep, frq, a.armed, probe, nobody, wdt, c_last, a)
    for f in rep.flags:
        print(f"{f['level']}: {f['text']}")
    failed = any(f["level"] == "FAIL" for f in rep.flags) or (a.strict and rep.flags)
    print("heartbeat check:", "FAILED" if failed else "passed",
          f"({sum(f['level'] == 'WARN' for f in rep.flags)} warnings)")
    if a.json:
        with open(a.json, "w") as f:
            json.dump({"flags": rep.flags, "events": rep.events, "data": rep.data, "failed": bool(failed)}, f, indent=1)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
