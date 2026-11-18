#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Unit tests of check_heartbeat.py on written logs (any host with Python 3; no heartbeat binary needed).

    python3 -I test_check_heartbeat.py

- the J713's 9D-a log (fixtures/heartbeat-9da.log, sha256 as in docs/level2-j713.md) still checks as before,
  and its wall clock step is flagged;
- a 9D-b-like log at 1 GHz (6boot arms at A, the driver's set_timeout at R = A + 2 s, pings every 15 s until
  R + 90 s, nobody opens it): six zeroings 15 s apart, the prediction at R + 120 s, and the next boot's wall
  clock inside or outside the window;
- broken logs: a missing read, a read whose counter values are out of order, the counter going back,
  CLOCK_BOOTTIME jumping on the counter, a slow read, reads in a log whose header says they are off, a log
  that ends between a heartbeat and its read.
"""
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
CHECK = os.path.join(HERE, "check_heartbeat.py")
FIX = os.path.join(HERE, "fixtures", "heartbeat-9da.log")
G, HZ = 1_000_000_000, 24_000_000
EPOCH = 1_795_000_000 * G                      # wall clock ns at counter 0, for the written logs


def run(text, *args):
    with tempfile.TemporaryDirectory() as d:
        log, js = os.path.join(d, "x.log"), os.path.join(d, "x.json")
        with open(log, "w") as f:
            f.write(text)
        p = subprocess.run([sys.executable, "-I", CHECK, log, "--json", js, *args], capture_output=True, text=True)
        with open(js) as f:
            return p.returncode, p.stdout, json.load(f)


def header(wdt=True, frq=G, cnt=3_000_000_000):
    h = ["boot test", "cmdline x6e3.hb=1" + (" x6e3.hbwdt=1" if wdt else ""),
         "chosen xboot,watchdog = armed=60 s at cntpct=10000000000",
         "watchdog dev: /dev/watchdog0, apple-watchdog driver: registered",
         f"clock cntfrq={frq} cntvct={cnt} boottime_ns={cnt} boottime0_cnt=0",
         "watchdog path prefix: /dev/watchdog",
         ("wdt node=watchdog@3882b0000 base=0x3882b0000 size=0x4000 page=16384 read-only" if wdt
          else "wdt off (no x6e3.hbwdt=1 on the command line)")]
    return h


def ninedb(end_s=119.5, read_us=2, realtime=True):
    """6boot arms at A; set_timeout at R = A + 2 s; pings at R + 15 k, k = 1..6; heartbeats every 0.5 s."""
    A = 10 * G
    R = A + 2 * G
    zeros = [R + 15 * k * G for k in range(0, 7)]
    lines = header()
    lines.append("w %d %d none" % (A + 3 * G, A + 3 * G))
    t = A + 3 * G
    while t <= A + end_s * G:
        rt = f" {(EPOCH + t) // G}.{(EPOCH + t) % G:09d}" if realtime else ""
        lines.append(f"t {t} {t}{rt}")
        c1, c2 = t + 20_000, t + 20_000 + read_us * 1000
        last = max(z for z in zeros if z <= c1)
        cur = ((c1 + c2) // 2 - last) * HZ // G
        lines.append(f"d {c1} {c2} {cur} {30 * HZ} 4")
        t += G // 2
    return A, R, lines


class NineDA(unittest.TestCase):
    def test_fixture_is_the_recorded_log(self):
        with open(FIX, "rb") as f:
            self.assertEqual(hashlib.sha256(f.read()).hexdigest(),
                             "014c7fe8e5fe7d618e098c3ce93954bce63f6ee1ebcb9e05e5231578e23b183d")

    def test_still_checks_as_before_and_flags_the_wall_clock_step(self):
        with open(FIX) as f:
            rc, out, js = run(f.read(), "--armed", "13058109843")
        self.assertEqual(rc, 0)
        for s in ("watchdog armed -> CLOCK_BOOTTIME 0: 1.475 s", "watchdog armed -> first heartbeat: 3.348 s",
                  "watchdog armed -> last heartbeat:  58.901 s", "watchdog registers: not in this log"):
            self.assertIn(s, out)
        self.assertEqual([f["code"] for f in js["flags"]], ["realtime_step"])
        self.assertIn("+17.945 s between boottime 35.918 and 36.419 s", js["flags"][0]["text"])


class NineDB(unittest.TestCase):
    def test_feeds_spacing_and_prediction(self):
        A, R, lines = ninedb()
        rc, out, js = run("\n".join(lines) + "\n", "--armed", str(A))
        self.assertEqual(rc, 0, out)
        w = js["data"]["wdt"]
        self.assertEqual(len(w["zeroings"]), 6)
        for k, z in enumerate(w["zeroings"], 1):
            self.assertLess(abs(z["cnt"] - (R + 15 * k * G)), G // 1000)
            self.assertLess(z["bound_s"], 1e-3)
        self.assertTrue(all(abs(z["since_previous_s"] - 15) < 1e-3 for z in w["zeroings"][1:]))
        self.assertLess(abs(w["prediction"]["cnt"] - (R + 120 * G)), G // 1000)
        self.assertLess(abs(w["rate_hz"] - HZ), 1)
        self.assertIn("PREDICTION (not an observation)", out)
        self.assertEqual(js["flags"], [])

    def test_next_boot_window(self):
        A, R, lines = ninedb()
        text = "\n".join(lines) + "\n"
        inside = (EPOCH + R + 140 * G) / G
        _, _, js = run(text, "--next-boot-realtime", str(inside))
        self.assertEqual(js["flags"], [])
        before = (EPOCH + R + 110 * G) / G                          # before the predicted reset
        _, out, js = run(text, "--next-boot-realtime", str(before))
        self.assertEqual([f["code"] for f in js["flags"]], ["next_boot"])
        self.assertIn("does NOT fall", out)


class Broken(unittest.TestCase):
    def codes(self, lines, *args):
        rc, out, js = run("\n".join(lines) + "\n", *args)
        return rc, [(f["level"], f["code"]) for f in js["flags"]]

    def test_missing_read(self):
        _, _, lines = ninedb(end_s=10)
        i = next(i for i, l in enumerate(lines) if l.startswith("d ")) + 4
        rc, c = self.codes(lines[:i] + lines[i + 1:])
        self.assertEqual(rc, 1)
        self.assertIn(("FAIL", "read_missing"), c)

    def test_ends_between_heartbeat_and_read(self):
        _, _, lines = ninedb(end_s=10)
        rc, c = self.codes(lines[:-1])
        self.assertEqual((rc, c), (0, [("WARN", "ends_before_read")]))

    def test_read_bracket_out_of_order(self):
        _, _, lines = ninedb(end_s=10)
        i = next(i for i, l in enumerate(lines) if l.startswith("d "))
        f = lines[i].split()
        lines[i] = " ".join(["d", f[2], f[1]] + f[3:])
        self.assertIn(("FAIL", "bracket"), self.codes(lines)[1])

    def test_counter_back(self):
        _, _, lines = ninedb(end_s=10)
        ts = [i for i, l in enumerate(lines) if l.startswith("t ")]
        lines[ts[5]], lines[ts[6]] = lines[ts[6]], lines[ts[5]]
        self.assertIn(("FAIL", "monotonic"), self.codes(lines)[1])

    def test_boottime_jumps_on_the_counter(self):
        _, _, lines = ninedb(end_s=10)
        i = [i for i, l in enumerate(lines) if l.startswith("t ")][8]
        f = lines[i].split()
        lines[i] = " ".join(["t", f[1], str(int(f[2]) + 5_000_000)] + f[3:])
        self.assertIn(("FAIL", "boottime"), self.codes(lines)[1])

    def test_slow_read(self):
        _, _, lines = ninedb(end_s=10, read_us=1500)
        self.assertIn(("WARN", "slow_read"), self.codes(lines)[1])

    def test_reads_in_a_log_that_says_off(self):
        _, _, lines = ninedb(end_s=10)
        lines = [l if not l.startswith("wdt ") else "wdt off (no x6e3.hbwdt=1 on the command line)" for l in lines]
        self.assertIn(("FAIL", "wdt_lines"), self.codes(lines)[1])

    def test_heartbeat_gap(self):
        _, _, lines = ninedb(end_s=20)
        ts = [i for i, l in enumerate(lines) if l.startswith("t ")]
        lines = lines[:ts[10]] + lines[ts[16]:]                       # three seconds of heartbeats gone
        rc, c = self.codes(lines)
        self.assertEqual(rc, 0)
        self.assertIn(("WARN", "gap"), c)

    def test_strict_fails_on_warnings(self):
        _, _, lines = ninedb(end_s=10)
        self.assertEqual(self.codes(lines[:-1], "--strict")[0], 1)


if __name__ == "__main__":
    unittest.main(verbosity=1)
