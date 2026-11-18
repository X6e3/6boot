#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check a heartbeat log (heartbeat.c) and say what it shows, and what it does not.

    check_heartbeat.py LOG [--armed CNT] [--stopped] [--opened-by COMM] [--expect-feed] [--strict]
                       [--wdt-hz HZ] [--wdt-tol FRACTION] [--cur-tol-ms MS] [--gap-s S] [--flush-window-s S]
                       [--trial-survey DIR] [--next-survey DIR] [--next-boot-realtime UNIX_S] [--json OUT]

Every finding is an observation (what the log holds), an inference (computed from it, with an estimated
uncertainty and the assumptions behind it) or a PREDICTION (what would happen if nothing changed; never an
observation). Problems are FAIL (the log or the telemetry it asked for is broken: exit 1) or WARN (something
unexpected that the log does show; exit 0 unless --strict). The checker never passes because it gave up:
an error inside it is a FAIL.

The log itself: every line must be one the heartbeat writes. Unreadable lines at the very end are a WARN
(a reset can leave a torn tail: see "lines on disk" below); anywhere else they are a FAIL.

The log's own clocks:
- the counter (CNTVCT_EL0): its rate is CNTFRQ_EL0 as the header logs it; the lines must show the same
  rate within 0.1 %. A log from before the header carried cntfrq (9D-a) has only the measured rate.
- CLOCK_BOOTTIME: where its 0 falls on the counter, from every line. NTP slews CLOCK_BOOTTIME (adjtimex) and not
  the counter, so a move of more than 1 ms is a WARN with its size, start and largest rate; a rate over 1000 ppm,
  more than a slew, is a FAIL. With kernel log marks, the kernel's own clock (not slewed) shows which one moved.
- the counter and CLOCK_BOOTTIME only go up (FAIL); heartbeat gaps over --gap-s (1.5 s) are a WARN.
- CLOCK_REALTIME: steps over 50 ms are a WARN; the wall clock is tied to the counter only between steps.
- the kernel log's clock (the marks); who had the watchdog open (the w lines); failed writes (e lines).

Lines on disk: a line in the file was written (and fsync returned) when its counter says. Lines after the
last one may have been written and lost: R5's Apple NVMe driver completes a flush that comes within 1 s of
the previous one without sending it to the drive (apple_nvme_delayed_flush, flush_interval 1000 ms). The
last line is when the heartbeat was alive; it is compared with the predicted reset, and a gap up to
--flush-window-s (1 s) plus two heartbeat intervals is called consistent with that loss, not shown by it.

The watchdog's own registers (the wdt header and the d lines: CUR, BITE, CTRL of WD1). The command line in
the log decides what is expected:
- no x6e3.hbwdt=1: "wdt off", or no wdt header at all (a heartbeat from before the read, as 9D-a): fine;
- x6e3.hbwdt=1: "wdt node=..." and reads; "wdt none: ..." (asked for, not available) is a FAIL;
- the header contradicting the command line, or reads under a header that says there are none: FAIL.
--expect-feed is a separate claim (the host test's): at least one zeroing, which needs usable reads.
When the reads are usable:
- every read's two counter values in order, reads in order, every heartbeat followed by its read (FAIL
  otherwise; then nothing below is computed from them); a read slower than 1 ms, and a log that ends
  between a heartbeat and its read (the read may not have returned), are WARNs;
- between two reads CUR is: count (as its clock adds), zeroed (it went down, to what a write of 0 between
  the reads explains), possible_zeroing (it went up, but less than its clock adds, and a write of 0
  between the reads would explain it as well as a stop would), wrap (past 2^32), stalled (did not move),
  slow (went up less than its clock adds; no write of 0 explains it), fast (more than its clock adds),
  lowered (went down, more than a write of 0 explains). All but count, zeroed and wrap are WARNs. Who
  wrote a 0 is not in the log: ping, start, set_timeout and restart all write 0 to CUR (apple_wdt.c); the
  BITE and CTRL changes in the same interval and the spacing are given with it, as evidence.
- BITE and CTRL changes are their own events; CUR >= BITE with RESET_EN on while the log goes on: WARN.
- the watchdog clock is fitted against the counter over stretches that only counted; with fewer than
  three reads in every stretch, or reads that do not advance, nothing is fitted and the nominal rate is
  used, said as such.
- times computed from CUR come with an ESTIMATED uncertainty (half the read, the rate's spread or the
  assumed --wdt-tol, the fit residual or the assumed --cur-tol-ms) and its basis. It is not a guarantee.
- the PREDICTION from the last read: when CUR would reach BITE if nothing wrote it again. Also, from
  6boot's /chosen (armed=N s at cntpct=A), the PREDICTION if nobody but 6boot ever touched WD1.

Across boots (--trial-survey, --next-survey: x6e3-survey folders, /var/lib/x6e3/survey/<k>/<seq>-<tag>-<id>):
- the trial's folder must be this boot (boot id) with this command line; its dmesg can hold the
  watchdog's probe record and the RTC line "setting system clock to ... (EPOCH)", which anchors the
  trial's kernel log to the RTC; its iomem shows who claimed the watchdog. Its files' times are when the
  survey finished (about 20 s into the boot): a sign of life, not the boot's start.
- the next folder: a different boot; whether its seq follows (a boot that died before its survey leaves
  none); its RTC line gives the next kernel's start on the wall clock, the earliest reliable record of
  that boot. --next-boot-realtime overrides it with a value from elsewhere.
- the window the two boots leave for the reset (after the last heartbeat, before the next kernel) is
  compared with the prediction, with an uncertainty of every wall clock step seen in the trial plus 1 s
  (two whole-second RTC reads): 9D-a's wall clock jumped 17.945 s, and R5 writes the system time back to
  the RTC (RTC_SYSTOHC), so the two boots' wall clocks need not share a basis. This is a coarse check.
At the end, the chain 6boot -> Linux -> feeds -> stop -> heartbeat -> reset -> fallback, with what this log
(and the survey folders) hold for each link. Also a check of the arithmetic on fixed values.
"""
import argparse
import json
import os
import re
import statistics
import sys
import traceback

U32 = 1 << 32
RX = {
    "t": re.compile(r"t (\d+) (\d+)(?: (\d+\.\d{9}))?$"),
    "d": re.compile(r"d (\d+) (\d+) (\d+) (\d+) ([0-9a-f]+)$"),
    "m": re.compile(r"m (\d+) (\d+) (\d+)$"),
    "m-": re.compile(r"m - \d+ \d+ .*$"),
    "e": re.compile(r"e (\d+) write_errors=(\d+) fsync_errors=(\d+) errno=(-?\d+)$"),
    "w": re.compile(r"w \d+ -?\d+ .*$"),
    "k": re.compile(r"k \d+ \d+ .*$"),
    "kfirst": re.compile(r"kfirst \d+ \d+ .*$"),
}
HEADER = ("boot ", "cmdline ", "chosen ", "watchdog dev:", "clock ", "watchdog path prefix:", "wdt ", "kmsg: ",
          "stopped", "klost")


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
        if y[2] <= since + tol:
            return "possible_zeroing", (f"CUR {x[2]} -> {y[2]}: {e - inc:.0f} less than its clock adds; a write of 0"
                                        " between the reads and a stop for part of the time explain it equally")
        return "slow", f"CUR {x[2]} -> {y[2]}: {e - inc:.0f} less than its clock adds (stopped for part of the time)"
    return "fast", f"CUR {x[2]} -> {y[2]}: {inc - e:.0f} more than its clock adds in that time"


def fit(points):
    """least squares y = a + b x over (x, y): (b, largest residual), or None when x does not vary"""
    n = len(points)
    if n < 3:
        return None
    mx = sum(x for x, _ in points) / n
    my = sum(y for _, y in points) / n
    sxx = sum((x - mx) ** 2 for x, _ in points)
    if not sxx > 0:
        return None
    b = sum((x - mx) * (y - my) for x, y in points) / sxx
    a = my - b * mx
    return b, max(abs(y - a - b * x) for x, y in points)


def arithmetic():
    assert boottime0(16405620797, 1872361499, 1_000_000_000) == 14533259298     # 9D-a's header, 1 GHz
    assert boottime0(100_000_000, 2_000_000_000, 24_000_000) == 52_000_000     # 24 MHz, the build VM
    assert boottime0(10**18, 10**17, 1_000_000_000) == 9 * 10**17
    assert kernel0(3_000_000, 5_000_000_000, 5_000_002_000, 1_000_000_000) == 2_000_001_000
    assert kernel0(1_000_000, 30_000_000, 30_000_000, 24_000_000) == 6_000_000
    assert fed_at(100_000_000_000 - 1000, 100_000_000_000 + 1000, 12_000_000, 1_000_000_000, 24_000_000) == 99_500_000_000
    assert reset_at(50_000_000_000, 50_000_000_000, 144_000_000, 720_000_000, 1_000_000_000, 24_000_000) == 74_000_000_000
    g, h = 1_000_000_000, 24_000_000
    r = lambda c, cur, bite=720_000_000, ctrl=4: (c, c + 1000, cur, bite, ctrl)
    k = lambda x, y: classify(x, y, g, h, h // 1000, 1e-3)[0]
    assert k(r(0, 0), r(g // 2, h // 2)) == "count"
    assert k(r(0, 240_000_000), r(g // 2, h // 4)) == "zeroed"
    assert k(r(0, U32 - h // 4, ctrl=0), r(g // 2, h // 4)) == "wrap"
    assert k(r(0, 5_000_000), r(g // 2, 5_000_000)) == "stalled"
    assert k(r(0, 240_000_000), r(g // 2, 200_000_000)) == "lowered"
    assert k(r(0, 240_000_000), r(g // 2, 245_000_000)) == "slow"
    assert k(r(0, 1_000_000), r(g // 2, 5_000_000)) == "possible_zeroing"
    assert k(r(0, 0), r(g // 2, h)) == "fast"
    assert fit([(1.0, 2.0), (1.0, 3.0), (1.0, 4.0)]) is None and fit([(0, 0), (1, 1)]) is None
    b, res = fit([(0.0, 0.0), (1.0, 24.0), (2.0, 48.0)])
    assert abs(b - 24) < 1e-9 and res < 1e-9


class Report:
    def __init__(self):
        self.flags, self.events, self.data = [], [], {}

    def fail(self, code, text):
        self.flags.append({"level": "FAIL", "code": code, "text": text})

    def warn(self, code, text):
        self.flags.append({"level": "WARN", "code": code, "text": text})


def parse(lines, rep):
    p = {"t": [], "d": [], "marks": [], "mark_failed": 0, "k": [], "w": [], "e": [], "order": []}
    bad = []
    for i, l in enumerate(lines):
        if not l:
            continue
        kind = next((k for k in ("t", "d", "m", "m-", "e", "w", "kfirst", "k") if RX[k].match(l)), None)
        if kind is None and not (l.startswith(HEADER) and "\x00" not in l):
            bad.append(i)
            continue
        if kind == "t":
            m = RX["t"].match(l)
            p["t"].append((int(m[1]), int(m[2]), m[3]))
            p["order"].append(("t", len(p["t"]) - 1, i))
        elif kind == "d":
            m = RX["d"].match(l)
            p["d"].append((int(m[1]), int(m[2]), int(m[3]), int(m[4]), int(m[5], 16)))
            p["order"].append(("d", len(p["d"]) - 1, i))
        elif kind == "m":
            p["marks"].append(tuple(int(x) for x in RX["m"].match(l).groups()))
        elif kind == "m-":
            p["mark_failed"] += 1
        elif kind == "e":
            p["e"].append(tuple(int(x) for x in RX["e"].match(l).groups()))
        elif kind == "k":
            p["k"].append(l.split(None, 3))
        elif kind == "w":
            p["w"].append(l.split(None, 3))
    if bad:
        last_good = max((i for i, l in enumerate(lines) if l and i not in set(bad)), default=-1)
        if all(i > last_good for i in bad):
            rep.warn("torn_tail", f"{len(bad)} unreadable line(s) at the end of the log"
                                  f"{' (NUL bytes)' if any(chr(0) in lines[i] for i in bad) else ''}: a write that a reset cut off")
        else:
            rep.fail("malformed", f"{len(bad)} line(s) the heartbeat does not write, first at line {bad[0] + 1}:"
                                  f" {lines[bad[0]][:60]!r}")
    first = lambda pre: next((l for l in lines if l.startswith(pre) and "\x00" not in l), None)
    p["clock"] = dict(re.findall(r"(\w+)=(-?\d+)", first("clock ") or ""))
    p["wdt"] = first("wdt ")
    p["kerr"] = first("kmsg: ")
    p["kfirst"] = next((l.split(None, 3) for l in lines if RX["kfirst"].match(l)), None)
    p["klost"] = sum(l == "klost" for l in lines)
    p["stopped"] = any(l.startswith("stopped") for l in lines)
    cw = first("chosen xboot,watchdog")
    p["chosen_wdt"] = cw.split("=", 1)[1].strip() if cw else None
    m = re.search(r"armed=(\d+) s at cntpct=(\d+)", p["chosen_wdt"] or "")
    p["armed_6boot"] = (int(m[2]), int(m[1])) if m else None
    p["wdog_hdr"] = first("watchdog dev:")
    b = first("boot ")
    p["boot_id"] = b.split(None, 1)[1].strip() if b else None
    c = first("cmdline ")
    p["cmdline"] = c[len("cmdline "):].strip() if c else None
    return p


def realtime_ns(s):
    sec, _, frac = s.partition(".")
    return int(sec) * 1_000_000_000 + int((frac + "000000000")[:9])


def check_time(p, rep, a):
    ts = p["t"]
    (c0, b0, _), (c1, b1, _) = ts[0], ts[-1]
    if b1 <= b0 or c1 <= c0:
        rep.fail("monotonic", "counter or CLOCK_BOOTTIME does not only go up")
        return None
    measured = (c1 - c0) * 1e9 / (b1 - b0)
    print(f"heartbeats: {len(ts)} over {(b1 - b0) / 1e9:.3f} s of CLOCK_BOOTTIME")
    print(f"counter ticks per second of CLOCK_BOOTTIME: {measured:.2f} (CLOCK_BOOTTIME is NTP-disciplined, so this is"
          " the counter against it, not the counter alone)")
    hdr = p["clock"]
    if "cntfrq" in hdr:
        frq = int(hdr["cntfrq"])
        print(f"CNTFRQ_EL0 in the header: {frq} Hz")
        if abs(measured - frq) > frq * 1e-3:
            rep.fail("cntfrq", "measured rate and CNTFRQ_EL0 disagree")
        logged0 = int(hdr.get("boottime0_cnt", 0))
    else:
        frq = round(measured)
        logged0 = None
        print("header has no cntfrq (written before it was logged): rate taken from the lines, header boottime0_cnt"
              " not used (it assumed 24 MHz)")
    zero = [boottime0(c, b, frq) for c, b, _ in ts]
    z0 = zero[0]                                    # from the first line, before any NTP slew of CLOCK_BOOTTIME
    spread = (max(zero) - min(zero)) / frq
    print(f"CLOCK_BOOTTIME 0 on the counter: {z0} ({z0 / frq:.3f} s) from the first line; over the lines it moves"
          f" {spread * 1e3:.3f} ms")
    if spread > 1e-3:
        # CLOCK_BOOTTIME (like MONOTONIC and REALTIME) is slewed by NTP through adjtimex; the counter is not.
        gained = [(z0 - z) / frq for z in zero]     # how far CLOCK_BOOTTIME ran ahead of the counter, s
        start = next(i for i, g in enumerate(gained) if abs(g) > 1e-4)
        rates = [abs(gained[i + 5] - gained[i]) / ((ts[i + 5][0] - ts[i][0]) / frq) * 1e6 for i in range(len(ts) - 5)]
        top = max(rates) if rates else float("inf")
        text = (f"CLOCK_BOOTTIME ran {gained[-1] * 1e3:+.3f} ms against the counter, from near boottime"
                f" {ts[start][1] / 1e9:.3f} s, at most {top:.0f} ppm. CLOCK_BOOTTIME is NTP-disciplined (adjtimex slews"
                f" it); every time in this check is on the counter")
        if top > 1000:
            rep.fail("boottime", text + "; faster than an NTP slew (over 1000 ppm)")
        else:
            rep.warn("boottime_slew", text)
        rep.data["boottime_slew"] = {"gained_s": gained[-1], "from_boottime_s": ts[start][1] / 1e9, "max_ppm": top}
    if logged0 is not None and abs(logged0 - z0) > frq // 1000:
        rep.fail("boottime0_cnt", "the header's boottime0_cnt disagrees with the first line")
    if any(x[0] <= y[0] or x[1] <= y[1] for y, x in zip(ts, ts[1:])):
        rep.fail("monotonic", "counter or CLOCK_BOOTTIME does not only go up")
    gaps = [(y[1] - x[1]) / 1e9 for x, y in zip(ts, ts[1:])]
    med = statistics.median(gaps) if gaps else 0
    for i, g in enumerate(gaps):
        if g > a.gap_s:
            rep.warn("gap", f"heartbeat gap of {g:.3f} s after boottime {ts[i][1] / 1e9:.3f} s"
                            f" (about {round(g / med) - 1 if med > 0 else '?'} heartbeats missing)")
    print(f"heartbeat interval: median {med * 1e3:.3f} ms, largest {max(gaps) * 1e3 if gaps else 0:.3f} ms")
    rts = [(c, b, realtime_ns(r)) for c, b, r in ts if r]
    segs, start, steps = [], 0, []
    for i in range(1, len(rts)):
        step = (rts[i][2] - rts[i][1]) - (rts[i - 1][2] - rts[i - 1][1])
        if abs(step) > 50_000_000:
            rep.warn("realtime_step", f"wall clock stepped {step / 1e9:+.3f} s between boottime"
                                      f" {rts[i - 1][1] / 1e9:.3f} and {rts[i][1] / 1e9:.3f} s")
            segs.append((start, i))
            steps.append(step)
            start = i
    segs.append((start, len(rts)))
    wall = []
    for s0, s1 in segs:
        if s1 <= s0:
            continue
        off = [r - c * 1_000_000_000 // frq for c, _, r in rts[s0:s1]]
        wall.append({"from_line": s0, "to_line": s1 - 1, "offset_ns": min(off), "spread_ns": max(off) - min(off)})
    if wall:
        print(f"wall clock: {len(wall)} segment(s) between steps; within each, wall - counter spreads at most"
              f" {max(w['spread_ns'] for w in wall) / 1e6:.3f} ms")
    for e in p["e"]:
        rep.warn("io_errors", f"heartbeat writes failing at counter {e[0]}: write errors {e[1]}, fsync errors {e[2]},"
                              f" errno {e[3]}")
    rep.data.update({"cntfrq": frq, "boottime0_cnt": z0, "wall_segments": wall, "wall_steps_ns": steps,
                     "interval_s": med})
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
    rep.data.update({"driver_probe_cnt": probe, "kernel0_cnt": k0})
    return probe, k0


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


def wdt_claim(p, rep):
    """Is the register telemetry what the log's command line asked for? Returns True when it is usable."""
    asked = p["cmdline"] is not None and "x6e3.hbwdt=1" in p["cmdline"].split()
    hdr, d = p["wdt"], p["d"]
    state = "usable"
    if hdr is None:
        if asked:
            rep.fail("wdt_missing", "the command line asks for the register read (x6e3.hbwdt=1) but the log has no"
                                    " wdt header")
        state = "absent"
    elif hdr.startswith("wdt off"):
        if asked:
            rep.fail("wdt_cmdline", "the command line asks for the register read but the header says it is off")
        state = "off"
    elif hdr.startswith("wdt none"):
        if asked:
            rep.fail("wdt_unavailable", f"register read asked for, not available: {hdr[len('wdt none'):].strip(': ')}")
        else:
            rep.fail("wdt_cmdline", "the register read was tried without x6e3.hbwdt=1 on the command line")
        state = "none"
    elif hdr.startswith("wdt node="):
        if not asked:
            rep.fail("wdt_cmdline", "the registers were read without x6e3.hbwdt=1 on the command line")
    else:
        rep.fail("wdt_header", f"unknown wdt header: {hdr[:60]!r}")
        state = "unknown"
    if state != "usable" and d:
        rep.fail("wdt_lines", "register reads in a log whose header says there are none")
    if state == "absent" and not asked:
        print("watchdog registers: not in this log (no x6e3.hbwdt=1, no wdt header: a heartbeat from before the read)")
    elif state == "off" and not asked:
        print("watchdog registers: off, as the command line says (no x6e3.hbwdt=1)")
    elif hdr:
        print(f"watchdog registers: {hdr[4:]}")
    rep.data["wdt_state"] = state
    return state == "usable"


def check_wdt(p, rep, frq, z0, armed, a):
    d = p["d"]
    hz = a.wdt_hz
    out = {"header": p["wdt"], "reads": len(d), "intervals": {}, "zeroings": [], "possible_zeroings": [],
           "changes": [], "prediction": None}
    rep.data["wdt"] = out
    on = (lambda c: f"armed +{(c - armed) / frq:9.3f} s") if armed is not None else \
         (lambda c: f"boottime {(c - z0) / frq:9.3f} s")
    n_fail = sum(f["level"] == "FAIL" for f in rep.flags)
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
        rep.fail("wdt_few", "register read asked for, but fewer than 2 reads")
    if sum(f["level"] == "FAIL" for f in rep.flags) > n_fail:
        print("  reads are not consistent: no classification, no clock fit, no prediction from them")
        return out
    widths = [(x[1] - x[0]) / frq for x in d]
    print(f"  reads: {len(d)}, read width median {statistics.median(widths) * 1e6:.1f} us,"
          f" widest {max(widths) * 1e6:.1f} us")
    for x, w in zip(d, widths):
        if w > 1e-3:
            rep.warn("slow_read", f"a register read took {w * 1e3:.3f} ms at {on(x[0])}")
    first = d[0]
    print(f"  first read ({on(first[0])}): CUR {first[2]} ({first[2] / hz:.3f} s), BITE {first[3]}"
          f" ({first[3] / hz:.3f} s), CTRL 0x{first[4]:x} ({ctrl_text(first[4])})")
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
    fits = [f for f in (fit([((x[0] + x[1]) / 2 / frq, x[2]) for x in s]) for s in stretches) if f]
    if fits:
        slopes = [f[0] for f in fits]
        rate = statistics.median(slopes)
        spread = (max(slopes) - min(slopes)) / 2
        res = max(f[1] for f in fits)
        rel = max(spread / rate, 1e-6) if rate > 0 else a.wdt_tol
        basis = (f"estimated from the clock fit ({len(fits)} stretch(es), rate spread +-{spread:.1f} Hz, largest"
                 f" residual {res:.0f} ticks) and half the read; assumes the watchdog clock runs linearly against the"
                 f" counter over the whole log; not a guarantee")
        print(f"  watchdog clock against the counter: {rate:.1f} Hz (expected {hz}), from {len(fits)} stretch(es)"
              f" that only counted; slopes spread +-{spread:.1f} Hz, largest residual {res:.0f} ticks"
              f" ({res / hz * 1e6:.1f} us)")
        if rate <= 0 or abs(rate - hz) > hz * a.wdt_tol:
            rep.fail("wdt_rate", f"watchdog clock {rate:.1f} Hz is not {hz} Hz within {a.wdt_tol:.2%}")
            rate = hz
    else:
        rate, rel, res = hz, a.wdt_tol, tol_ticks
        basis = (f"NOT measured (no stretch of 3 reads that only counted): assumes the nominal {hz} Hz within"
                 f" --wdt-tol {a.wdt_tol:.2%} and CUR within --cur-tol-ms {a.cur_tol_ms}; plus half the read")
        print("  watchdog clock against the counter: not fitted (no stretch of 3 reads that only counted);"
              " the nominal rate is used")
    out.update({"rate_hz": rate, "rate_rel_uncertainty": rel, "fit_residual_ticks": res, "uncertainty_basis": basis})

    def unc(x, ticks):
        return (x[1] - x[0]) / 2 / frq + abs(ticks) / rate * rel + res / rate

    counts, prev_zero = {}, None
    for (k, why), x, y in zip(kinds, d, d[1:]):
        counts[k] = counts.get(k, 0) + 1
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
        if k in ("zeroed", "possible_zeroing"):
            at = fed_at(y[0], y[1], y[2], frq, rate)
            with_ = []
            if y[3] != x[3]:
                with_.append("BITE changed too (as set_timeout writes CUR 0, then BITE)")
            if not (x[4] & 4) and y[4] & 4:
                with_.append("RESET_EN came on too (as start writes CUR 0, then CTRL)")
            if y[3] == x[3] and y[4] == x[4]:
                with_.append("BITE and CTRL unchanged (as a ping writes CUR 0 and nothing else)")
            ev = {"kind": k, "cnt": at, "uncertainty_s": unc(y, y[2]), "with": with_,
                  "since_previous_s": (at - prev_zero) / frq if prev_zero is not None and k == "zeroed" else None}
            if k == "zeroed":
                out["zeroings"].append(ev)
                prev_zero = at
            else:
                ev["explanations"] = ["a write of 0 at the time given", "CUR stopped for part of the interval"]
                out["possible_zeroings"].append(ev)
                rep.warn("possible_zeroing", f"between {on(x[0])} and {on(y[0])}: {why}")
            rep.events.append(ev)
        elif k in ("stalled", "slow", "fast", "lowered"):
            rep.warn(f"cur_{k}", f"between {on(x[0])} and {on(y[0])}: {why}")
            rep.events.append({"kind": k, "cnt": y[0]})
        elif k == "wrap":
            print(f"  CUR wrapped past 2^32 between {on(x[0])} and {on(y[0])} (RESET_EN {'on' if y[4] & 4 else 'off'})")
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
    print(f"  zeroings of CUR: {len(zs)} (it went down to what a write of 0 between two reads explains; a write of"
          f" another small value would look the same; the log does not say who wrote it)")
    for i, z in enumerate(zs):
        gap = f"  (+{z['since_previous_s']:.3f} s)" if z["since_previous_s"] is not None else ""
        print(f"    {i + 1:2d} at {on(z['cnt'])} +- {z['uncertainty_s'] * 1e3:.3f} ms (estimated){gap};"
              f" {'; '.join(z['with'])}")
    if out["possible_zeroings"]:
        print(f"  possible zeroings (a write of 0, or a stop; not counted as zeroings): {len(out['possible_zeroings'])}")
    last = d[-1]
    if last[4] & 4:
        r = reset_at(last[0], last[1], last[2], last[3], frq, rate)
        out["prediction"] = {"cnt": r, "uncertainty_s": unc(last, last[3] - last[2]), "basis": basis,
                             "source": "registers"}
        print(f"  PREDICTION (not an observation): from the last read ({on(last[0])}: CUR {last[2] / hz:.3f} s of"
              f" BITE {last[3] / hz:.3f} s), if nothing wrote CUR again the watchdog would reset the machine at"
              f" {on(r)} +- {out['prediction']['uncertainty_s'] * 1e3:.3f} ms (estimated uncertainty)")
        print(f"    uncertainty basis: {basis}")
    else:
        print(f"  PREDICTION: none from the registers; RESET_EN is off at the last read (CTRL 0x{last[4]:x})")
    if a.expect_feed and not zs:
        rep.fail("no_feed", "no feed seen")
    return out


def ctrl_text(c):
    bits = [n for b, n in ((4, "RESET_EN"), (1, "IRQ_EN"), (2, "IRQ_STATUS")) if c & b]
    other = c & ~7
    return " ".join(bits + ([f"other 0x{other:x}"] if other else [])) or "none"


def read_survey(path):
    """an x6e3-survey folder: <seq>-<tag>-<boot id 8>/ with cmdline.txt, dmesg.txt, iomem.txt"""
    name = os.path.basename(os.path.normpath(path))
    m = re.match(r"(\d+)-(.+)-([0-9a-f]{8})$", name)
    if not m:
        raise ValueError(f"not a survey folder name: {name}")
    s = {"path": path, "seq": int(m[1]), "tag": m[2], "boot8": m[3], "probes": [], "rtc": None, "iomem": None}
    with open(os.path.join(path, "cmdline.txt"), errors="replace") as f:
        s["cmdline"] = f.read().strip()
    dm = os.path.join(path, "dmesg.txt")
    with open(dm, errors="replace") as f:
        for l in f:
            mm = re.match(r"\[\s*(\d+\.\d+)\]\s*(.*)$", l.rstrip("\n"))
            if not mm:
                continue
            ts, text = float(mm[1]), mm[2]
            pr = re.search(r"probe of (\S*watchdog\S*) returned (-?\d+) after (\d+) usecs", text)
            if pr:
                s["probes"].append((ts, text))
            rt = re.search(r"setting system clock to (\S+) UTC \((\d+)\)", text)
            if rt and s["rtc"] is None:
                s["rtc"] = (ts, int(rt[2]), rt[1])
    io = os.path.join(path, "iomem.txt")
    if os.path.exists(io):
        with open(io, errors="replace") as f:
            s["iomem"] = next((l.strip() for l in f if "watchdog" in l), None)
    s["finished_wall"] = os.stat(dm).st_mtime
    # the RTC holds whole seconds, truncated: the kernel's start on the wall clock is EPOCH + 0.5 - ts, +- 0.5 s
    s["kernel0_wall"] = s["rtc"][1] + 0.5 - s["rtc"][0] if s["rtc"] else None
    return s


def surveys(p, rep, a, frq, k0):
    trial = nxt = None
    if a.trial_survey:
        trial = read_survey(a.trial_survey)
        print(f"trial survey: {trial['path']}")
        if not p["boot_id"] or not p["boot_id"].replace("-", "").startswith(trial["boot8"]):
            rep.fail("survey_boot", f"the trial survey is boot {trial['boot8']}, the log is boot {p['boot_id']}")
        if p["cmdline"] is not None and trial["cmdline"] != p["cmdline"]:
            rep.fail("survey_cmdline", "the trial survey's command line is not the log's")
        for ts, text in trial["probes"]:
            on = f", counter {k0 + round(ts * frq)}" if k0 is not None else ""
            print(f"  its dmesg: kernel {ts:.6f} s{on}: {text}")
        print(f"  its iomem: {trial['iomem'] or 'no watchdog region'}")
        if trial["rtc"]:
            print(f"  its RTC line: kernel {trial['rtc'][0]:.6f} s, {trial['rtc'][2]} UTC ({trial['rtc'][1]}):"
                  f" kernel 0 at wall {trial['kernel0_wall']:.1f} +- 0.5 s (RTC basis)")
        print(f"  the survey finished at wall {trial['finished_wall']:.3f} (file time): the trial was alive then;"
              " not the boot's start")
    if a.next_survey:
        nxt = read_survey(a.next_survey)
        print(f"next survey: {nxt['path']}")
        if trial:
            if nxt["boot8"] == trial["boot8"]:
                rep.fail("survey_next", "the next survey is the same boot as the trial")
            if nxt["seq"] != trial["seq"] + 1:
                rep.warn("survey_seq", f"next survey seq {nxt['seq']} does not follow the trial's {trial['seq']}")
            else:
                print(f"  seq {nxt['seq']} follows the trial's {trial['seq']}: no surveyed boot in between (a boot that"
                      " died before its survey would leave none)")
        print(f"  its tag: {nxt['tag']}; command line: {nxt['cmdline'][:120]}")
        if nxt["rtc"]:
            print(f"  its RTC line: kernel {nxt['rtc'][0]:.6f} s, {nxt['rtc'][2]} UTC: next kernel's start at wall"
                  f" {nxt['kernel0_wall']:.1f} +- 0.5 s (the earliest reliable record of that boot)")
        else:
            print("  its dmesg has no RTC line: no wall clock anchor for the next boot")
    return trial, nxt


def window(p, rep, a, frq, k0, trial, nxt, c_last, preds):
    """the wall clock window between the last heartbeat and the next kernel, against each prediction"""
    nb = a.next_boot_realtime
    src = "given (--next-boot-realtime)"
    if nb is None and nxt and nxt["kernel0_wall"] is not None:
        nb, src = nxt["kernel0_wall"], "the next survey's RTC line"
    if nb is None:
        return None
    if trial and trial["kernel0_wall"] is not None and k0 is not None:
        wall = lambda c: trial["kernel0_wall"] + (c - k0) / frq
        basis = "the trial's RTC line and kernel log marks"
    elif rep.data.get("wall_segments"):
        off = rep.data["wall_segments"][0]["offset_ns"]
        wall = lambda c: (c * 1_000_000_000 // frq + off) / 1e9
        basis = "the trial's wall clock before its first step"
    else:
        print("reset window: no wall clock for the trial")
        return None
    u = sum(abs(s) for s in rep.data.get("wall_steps_ns", [])) / 1e9 + 1.0
    lo, hi = wall(c_last), nb
    print(f"reset window (coarse): after the last heartbeat (wall {lo:.3f}, {basis}) and before the next kernel"
          f" (wall {hi:.3f}, {src}): {hi - lo:.3f} s, +- {u:.3f} s (every wall clock step in the trial + 1 s of RTC)")
    res = []
    for name, pr in preds:
        if not pr:
            continue
        w = wall(pr["cnt"])
        ok = lo - u <= w <= hi + u
        res.append({"prediction": name, "wall": w, "inside": ok})
        print(f"  {name} PREDICTION at wall {w:.3f}: {'inside' if ok else 'OUTSIDE'} the window")
        if not ok:
            rep.warn("next_boot", f"the {name} prediction is outside the window between the last heartbeat and the next"
                                  " kernel")
    rep.data["window"] = {"lo": lo, "hi": hi, "uncertainty_s": u, "basis": basis, "next_source": src, "checks": res}
    return res


def loss(p, rep, a, frq, c_last, pred, name):
    """how far before the prediction the log ends, against the delayed NVMe flush"""
    iv = rep.data.get("interval_s") or 0.5
    gap = (pred["cnt"] - c_last) / frq
    allowed = a.flush_window_s + 2 * iv
    print(f"the log ends {gap:.3f} s before the {name} PREDICTION: about {max(int(gap / iv), 0)} heartbeat(s) expected"
          f" in between are not in the file")
    if gap < 0:
        rep.warn("alive_after_prediction", f"the heartbeat ran {-gap:.3f} s past the {name} prediction")
    elif gap <= allowed:
        print(f"  that is within {allowed:.3f} s (the delayed NVMe flush, {a.flush_window_s:.1f} s, plus two intervals):"
              " consistent with fsync'd lines lost at the reset; not shown by the log")
    else:
        rep.warn("stopped_early", f"the log ends {gap:.3f} s before the {name} prediction, more than the delayed flush"
                                  f" explains ({allowed:.3f} s): the heartbeat or the machine stopped earlier, or the"
                                  " prediction is wrong")


def chain(p, rep, frq, armed, probe, nobody, wdt, c_last, trial, nxt, preds, main=None, inapplicable=()):
    print("chain (what the log and the survey folders hold for each link):")
    cw = p["chosen_wdt"]
    print(f"  1 6boot armed the watchdog:        {'6boot says ' + cw if cw and cw != '(none)' else 'no xboot,watchdog in /chosen'}")
    took = []
    if probe is not None:
        took.append("the heartbeat's kernel log copy has the watchdog's probe")
    if trial and trial["probes"]:
        took.append("the trial survey's dmesg has it")
    if trial and trial["iomem"]:
        took.append(f"iomem: {trial['iomem']}")
    if p["wdog_hdr"]:
        took.append(p["wdog_hdr"].split(", ", 1)[-1])
    if wdt and wdt.get("reads") and p["d"]:
        b0 = p["d"][0][3]
        took.append(f"first read BITE = {b0 / 24e6:.3f} s ({'the driver writes 30 s' if b0 == 720_000_000 else 'not the driver default 30 s'})")
    print(f"  2 Linux took it over:              {'; '.join(took) if took else 'nothing'}")
    print(f"  3 nobody in user space opened it:  {'nobody at any check' if nobody else ('someone did' if nobody is False else 'not checked')}")
    zs = (wdt or {}).get("zeroings", [])
    pz = (wdt or {}).get("possible_zeroings", [])
    gaps = [z["since_previous_s"] for z in zs if z["since_previous_s"] is not None]
    feeds = f"{len(zs)} zeroings of CUR" + (f", spacing {min(gaps):.3f}-{max(gaps):.3f} s" if gaps else "") + \
            (f", {len(pz)} possible" if pz else "")
    print(f"  4 the kernel fed it:               {feeds if wdt else 'registers not read'}")
    on = (lambda c: f"armed +{(c - armed) / frq:.3f} s") if armed is not None else (lambda c: f"counter {c}")
    last_z = zs[-1]["cnt"] if zs else None
    print(f"  5 feeds stopped:                   {'last zeroing at ' + on(last_z) if last_z else 'no zeroing seen'}")
    print(f"  6 the heartbeat stopped:           last line on disk at {on(c_last)}{' (clean stop)' if p['stopped'] else ''};"
          " it was alive then, later lines may have been lost")
    for name, pr in preds:
        if pr:
            u = (f"+- {pr['uncertainty_s'] * 1e3:.3f} ms (estimated)" if pr["uncertainty_s"] is not None
                 else "(no uncertainty estimate: " + pr["basis"] + ")")
            if (main is not None and pr is not main) or name in inapplicable:
                u += "; does not apply here (a driver registered and took WD1 over)"
            print(f"  7 PREDICTED reset ({name}):{' ' * max(1, 13 - len(name))}{on(pr['cnt'])} {u}")
    if not any(pr for _, pr in preds):
        print("  7 PREDICTED reset:                 none")
    if nxt:
        print(f"  8 the next boot:                   survey {nxt['seq']}-{nxt['tag']}-{nxt['boot8']}; that 6boot fell back is"
              " in 6boot's next-boot report (esp-check), not here")
    else:
        print("  8 the next boot (reset happened, 6boot fell back): not in this log; 6boot's next-boot report and the"
              " next boot's own records")


def run(a):
    arithmetic()
    rep = Report()
    with open(a.log, errors="replace") as f:
        lines = f.read().split("\n")
    p = parse(lines, rep)
    if len(p["t"]) < 2:
        print(f"heartbeats: {len(p['t'])}, too few")
        rep.fail("too_few", f"{len(p['t'])} heartbeats")
        return rep
    tm = check_time(p, rep, a)
    if tm is None:
        return rep
    frq, z0, c_last = tm
    armed = a.armed
    if armed is None and p["armed_6boot"]:
        armed = p["armed_6boot"][0]
        print(f"armed: taken from /chosen (cntpct {armed})")
    probe, k0 = check_kernel(p, rep, frq, z0, armed, c_last)
    if "boottime_slew" in rep.data and len(p["marks"]) > 1:
        z = [kernel0(us, x, y, frq) for us, x, y in p["marks"]]
        print(f"  the kernel log's clock, which NTP does not slew, kept to the counter within"
              f" {(max(z) - min(z)) / frq * 1e6:.1f} us over {len(z)} marks: CLOCK_BOOTTIME moved, not the counter")
    nobody = check_openers(p, rep, a.opened_by, len(p["t"]))
    wdt = None
    if wdt_claim(p, rep):
        wdt = check_wdt(p, rep, frq, z0, armed, a)
    elif a.expect_feed:
        rep.fail("no_feed", "no feed can be shown: the register telemetry is not usable")
    preds = []
    if wdt and wdt.get("prediction"):
        preds.append(("registers", wdt["prediction"]))
    if p["armed_6boot"]:
        A, secs = p["armed_6boot"]
        pr6 = {"cnt": A + secs * frq, "uncertainty_s": None, "source": "6boot",
               "basis": "6boot's arming alone; assumes nobody else touched WD1 and the watchdog clock is exactly 24 MHz"}
        preds.append(("6boot alone", pr6))
        print(f"PREDICTION if nobody but 6boot touched WD1: {secs} s after the arming, at counter {pr6['cnt']}")
    rep.data["predictions"] = {n: pr for n, pr in preds}
    if armed is not None:
        print(f"watchdog armed -> CLOCK_BOOTTIME 0: {(z0 - armed) / frq:.3f} s")
        print(f"watchdog armed -> first heartbeat: {(p['t'][0][0] - armed) / frq:.3f} s")
        print(f"watchdog armed -> last heartbeat:  {(c_last - armed) / frq:.3f} s (the reset came after this)")
    # the prediction that applies: the registers', else 6boot's alone when no driver took the watchdog over
    driver = bool(p["wdog_hdr"]) and "not registered" not in p["wdog_hdr"]
    main = next((pr for n, pr in preds if n == "registers"), None) or \
        (next((pr for n, pr in preds if n == "6boot alone"), None) if not driver else None)
    if p["armed_6boot"] and driver:
        print("  (6boot's alone does not apply: the header says a driver registered and took the watchdog over)")
    if main and not p["stopped"]:
        loss(p, rep, a, frq, c_last, main, main["source"])
    print(f"ends with stopped: {'yes' if p['stopped'] else 'no'}")
    if a.stopped and not p["stopped"]:
        rep.fail("stopped", "no stopped line")
    trial, nxt = surveys(p, rep, a, frq, k0)
    window(p, rep, a, frq, k0, trial, nxt, c_last, [(main["source"], main)] if main else [])
    chain(p, rep, frq, armed, probe, nobody, wdt, c_last, trial, nxt, preds, main,
          {"6boot alone"} if driver else ())
    return rep


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
    ap.add_argument("--flush-window-s", type=float, default=1.0, help="R5 nvme-apple flush_interval")
    ap.add_argument("--trial-survey")
    ap.add_argument("--next-survey")
    ap.add_argument("--next-boot-realtime", type=float)
    ap.add_argument("--json")
    a = ap.parse_args()
    try:
        rep = run(a)
    except Exception as exc:                                   # the checker failing is a FAIL, never a pass
        rep = Report()
        rep.fail("checker_error", f"{type(exc).__name__}: {exc}")
        traceback.print_exc()
    for f in rep.flags:
        print(f"{f['level']}: {f['text']}")
    failed = any(f["level"] == "FAIL" for f in rep.flags) or (a.strict and bool(rep.flags))
    print("heartbeat check:", "FAILED" if failed else "passed",
          f"({sum(f['level'] == 'WARN' for f in rep.flags)} warnings)")
    if a.json:
        with open(a.json, "w") as f:
            json.dump({"flags": rep.flags, "events": rep.events, "data": rep.data, "failed": bool(failed)}, f,
                      indent=1, default=str)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
