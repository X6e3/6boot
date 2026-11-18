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
  that ends between a heartbeat and its read, reads that do not advance (no regression, a clean FAIL), a
  line the heartbeat does not write (FAIL) against a torn tail (WARN), failed writes, the checker itself
  failing (FAIL, never a pass);
- the register telemetry against the command line: every combination of x6e3.hbwdt=1 and the wdt header;
  missing telemetry is never a silent pass;
- a possible zeroing (a write of 0 inside a gap, which a stop explains as well) kept apart from zeroings;
- the estimated uncertainty's basis: measured (clock fit) or NOT measured (nominal 24 MHz);
- the x6e3-survey folders: the trial's must be this boot with this command line; the next one's RTC line
  gives the next kernel's start, and the reset window.
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


BOOT = "0123abcd-1111-2222-3333-444455556666"
CMDLINE = "console=tty0 x6e3.v=6boot-r5-trial-9db x6e3.hb=1"


def header(wdt=True, frq=G, cnt=3_000_000_000, asked=None, hdr=None):
    asked = wdt if asked is None else asked
    h = [f"boot {BOOT}", "cmdline " + CMDLINE + (" x6e3.hbwdt=1" if asked else ""),
         "chosen xboot,watchdog = armed=60 s at cntpct=10000000000",
         "watchdog dev: /dev/watchdog0, apple-watchdog driver: registered",
         f"clock cntfrq={frq} cntvct={cnt} boottime_ns={cnt} boottime0_cnt=0",
         "watchdog path prefix: /dev/watchdog",
         hdr if hdr is not None else
         ("wdt node=watchdog@3882b0000 base=0x3882b0000 size=0x4000 page=16384 read-only" if wdt
          else "wdt off (no x6e3.hbwdt=1 on the command line)")]
    return [l for l in h if l != ""]


def ninedb(end_s=121.0, read_us=2, realtime=True, stop=None, **hk):
    """6boot arms at A; set_timeout at R = A + 2 s; pings at R + 15 k, k = 1..6; heartbeats every 0.5 s until
    end_s after A. A full log ends 1 s before the predicted reset (R + 120 s = A + 122 s), as a reset with the
    delayed flush may leave it; a short one (end_s < 100, a log cut for one test) ends with "stopped" unless
    stop=False."""
    A = 10 * G
    R = A + 2 * G
    zeros = [R + 15 * k * G for k in range(0, 7)]
    lines = header(**hk)
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
    if stop if stop is not None else end_s < 100:
        lines.append(f"stopped {t}")
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
            self.assertLess(z["uncertainty_s"], 1e-3)
        self.assertTrue(all(abs(z["since_previous_s"] - 15) < 1e-3 for z in w["zeroings"][1:]))
        self.assertLess(abs(w["prediction"]["cnt"] - (R + 120 * G)), G // 1000)
        self.assertLess(abs(w["rate_hz"] - HZ), 1)
        self.assertIn("PREDICTION (not an observation)", out)
        self.assertEqual(js["flags"], [])
        self.assertIn("estimated from the clock fit", w["prediction"]["basis"])

    def test_next_boot_window(self):
        A, R, lines = ninedb()
        text = "\n".join(lines) + "\n"
        inside = (EPOCH + R + 140 * G) / G
        _, _, js = run(text, "--next-boot-realtime", str(inside))
        self.assertEqual(js["flags"], [])
        before = (EPOCH + R + 110 * G) / G                          # before the predicted reset
        _, out, js = run(text, "--next-boot-realtime", str(before))
        self.assertEqual([f["code"] for f in js["flags"]], ["next_boot"])
        self.assertIn("OUTSIDE the window", out)


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
        _, _, lines = ninedb(end_s=10, stop=False)
        rc, c = self.codes(lines[:-1])
        # both are true: the read may not have returned, and the heartbeat stopped long before the predicted reset
        self.assertEqual((rc, c), (0, [("WARN", "ends_before_read"), ("WARN", "stopped_early")]))

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
        _, _, lines = ninedb(end_s=10, stop=False)
        self.assertEqual(self.codes(lines[:-1], "--strict")[0], 1)


class Telemetry(unittest.TestCase):
    """x6e3.hbwdt=1 on the command line against the wdt header: missing telemetry is never a silent pass"""

    def case(self, asked, hdr, reads):
        _, _, lines = ninedb(end_s=10, asked=asked, hdr=hdr)
        if not reads:
            lines = [l for l in lines if not l.startswith("d ")]
        rc, out, js = run("\n".join(lines) + "\n")
        return rc, [f["code"] for f in js["flags"] if f["level"] == "FAIL"]

    def test_matrix(self):
        node = "wdt node=watchdog@3882b0000 base=0x3882b0000 size=0x4000 page=16384 read-only"
        off = "wdt off (no x6e3.hbwdt=1 on the command line)"
        none = "wdt none: watchdog@3882b0000 base=0x3882b0000: open /dev/mem: Permission denied"
        self.assertEqual(self.case(False, "", False), (0, []))                       # old heartbeat, not asked
        self.assertEqual(self.case(False, off, False), (0, []))                      # off, as asked
        self.assertEqual(self.case(True, node, True), (0, []))                       # asked and read
        self.assertEqual(self.case(True, "", False), (1, ["wdt_missing"]))           # asked, no header at all
        self.assertEqual(self.case(True, off, False), (1, ["wdt_cmdline"]))          # asked, header says off
        self.assertEqual(self.case(True, none, False), (1, ["wdt_unavailable"]))     # asked, not available
        self.assertEqual(self.case(False, none, False), (1, ["wdt_cmdline"]))        # tried without being asked
        self.assertEqual(self.case(False, node, True), (1, ["wdt_cmdline"]))         # read without being asked
        rc, codes = self.case(True, node, False)                                     # asked, header, no reads
        self.assertEqual((rc, sorted(set(codes))), (1, ["read_missing", "wdt_few"]))
        self.assertIn("wdt_lines", self.case(False, off, True)[1])                   # reads under "off"

    def test_expect_feed_is_its_own_claim(self):
        _, _, lines = ninedb(end_s=10, asked=False, hdr="wdt off (no x6e3.hbwdt=1 on the command line)")
        lines = [l for l in lines if not l.startswith("d ")]
        rc, _, js = run("\n".join(lines) + "\n", "--expect-feed")
        self.assertEqual((rc, [f["code"] for f in js["flags"]]), (1, ["no_feed"]))


class Ambiguity(unittest.TestCase):
    def test_possible_zeroing_is_kept_apart(self):
        A, R, lines = ninedb(end_s=60, stop=True)
        ts = [i for i, l in enumerate(lines) if l.startswith("t ")]
        first_after = next(i for i in ts if int(lines[i].split()[1]) > R + 15 * G)  # just after the ping at R + 15
        k = ts.index(first_after) + 1
        lines = lines[:ts[k]] + lines[ts[k + 32]:]                                  # 16 s gap over the ping at R + 30
        rc, out, js = run("\n".join(lines) + "\n", "--armed", str(A))
        w = js["data"]["wdt"]
        self.assertEqual(rc, 0)
        self.assertEqual(len(w["possible_zeroings"]), 1)
        self.assertEqual(len(w["zeroings"]), 2)                       # R + 15 and R + 45 (R + 60 is after the log)
        self.assertEqual(sorted(f["code"] for f in js["flags"]), ["gap", "possible_zeroing"])
        self.assertIn("explain it equally", next(f["text"] for f in js["flags"] if f["code"] == "possible_zeroing"))

    def test_nominal_rate_is_said_to_be_not_measured(self):
        _, _, lines = ninedb(end_s=3.5)                                            # 2 reads: no stretch to fit
        rc, out, js = run("\n".join(lines) + "\n")
        self.assertIn("NOT measured", js["data"]["wdt"]["prediction"]["basis"])
        self.assertIn("estimated uncertainty", out)


class Robust(unittest.TestCase):
    def test_reads_that_do_not_advance_fail_cleanly(self):
        _, _, lines = ninedb(end_s=6)
        d = [i for i, l in enumerate(lines) if l.startswith("d ")]
        f0 = lines[d[0]].split()
        for i in d:
            f = lines[i].split()
            lines[i] = " ".join(["d", f0[1], f0[2]] + f[3:])
        rc, out, js = run("\n".join(lines) + "\n")
        codes = [f["code"] for f in js["flags"]]
        self.assertEqual(rc, 1)
        self.assertIn("read_monotonic", codes)
        self.assertNotIn("checker_error", codes)
        self.assertIn("no classification, no clock fit, no prediction", out)
        self.assertIsNone(js["data"]["wdt"]["prediction"])

    def test_malformed_line_in_the_middle_fails(self):
        _, _, lines = ninedb(end_s=6)
        lines.insert(12, "t 12 x")
        rc, _, js = run("\n".join(lines) + "\n")
        self.assertEqual(rc, 1)
        self.assertIn("malformed", [f["code"] for f in js["flags"]])

    def test_torn_tail_is_a_warning(self):
        _, _, lines = ninedb(end_s=6)
        rc, _, js = run("\n".join(lines) + "\n" + "\x00" * 300 + "\nt 1234")
        self.assertEqual(rc, 0)
        self.assertEqual([f["code"] for f in js["flags"]], ["torn_tail"])

    def test_failed_writes_are_reported(self):
        _, _, lines = ninedb(end_s=6)
        lines.insert(14, "e 10000000000 write_errors=0 fsync_errors=1 errno=5")
        rc, _, js = run("\n".join(lines) + "\n")
        self.assertEqual([f["code"] for f in js["flags"]], ["io_errors"])

    def test_checker_error_is_a_fail(self):
        with tempfile.TemporaryDirectory() as d:
            js = os.path.join(d, "x.json")
            p = subprocess.run([sys.executable, "-I", CHECK, d, "--json", js], capture_output=True, text=True)
            self.assertEqual(p.returncode, 1)
            with open(js) as f:
                self.assertEqual([x["code"] for x in json.load(f)["flags"]], ["checker_error"])


def survey(root, seq, tag, boot8, cmdline, dmesg, iomem="3882b0000-3882b3fff : 3882b0000.watchdog"):
    d = os.path.join(root, f"{seq:04d}-{tag}-{boot8}")
    os.makedirs(d)
    for name, text in (("cmdline.txt", cmdline + "\n"), ("dmesg.txt", dmesg), ("iomem.txt", iomem + "\n")):
        with open(os.path.join(d, name), "w") as f:
            f.write(text)
    return d


class Surveys(unittest.TestCase):
    def setUp(self):
        self.A, self.R, lines = ninedb()
        self.text = "\n".join(lines) + "\n"
        self.tmp = tempfile.TemporaryDirectory()
        cmd = CMDLINE + " x6e3.hbwdt=1"
        self.trial = survey(self.tmp.name, 7, "6boot-r5-trial-9db", BOOT[:8], cmd,
                            "[    1.500000] probe of 3882b0000.watchdog returned 0 after 120 usecs\n")

    def tearDown(self):
        self.tmp.cleanup()

    def next_boot(self, wall0, seq=8, boot8="9999aaaa"):
        epoch = int(wall0 + 2.0)                                     # the RTC line at kernel 2.0 s
        return survey(self.tmp.name, seq, "6boot-r5-tm-root2", boot8, "console=tty0 x6e3.v=6boot-r5-tm-root2",
                      f"[    2.000000] macsmc-rtc macsmc-rtc: setting system clock to X UTC ({epoch})\n")

    def test_window_from_the_next_survey(self):
        wall_pred = (EPOCH + self.R + 120 * G) / G
        nxt = self.next_boot(wall_pred + 25)
        rc, out, js = run(self.text, "--trial-survey", self.trial, "--next-survey", nxt)
        self.assertEqual((rc, js["flags"]), (0, []))
        self.assertIn("the next survey's RTC line", js["data"]["window"]["next_source"])
        self.assertTrue(js["data"]["window"]["checks"][0]["inside"])
        self.assertIn("seq 8 follows the trial's 7", out)
        self.assertIn("the trial survey's dmesg has it", out)

    def test_next_kernel_before_the_prediction(self):
        nxt = self.next_boot((EPOCH + self.R + 100 * G) / G)
        _, _, js = run(self.text, "--trial-survey", self.trial, "--next-survey", nxt)
        self.assertIn("next_boot", [f["code"] for f in js["flags"]])

    def test_wrong_boot_and_seq_gap(self):
        other = survey(self.tmp.name, 7, "6boot-r5-trial-9db", "deadbeef", CMDLINE + " x6e3.hbwdt=1", "")
        nxt = self.next_boot((EPOCH + self.R + 140 * G) / G, seq=9)
        rc, _, js = run(self.text, "--trial-survey", other, "--next-survey", nxt)
        codes = [f["code"] for f in js["flags"]]
        self.assertEqual(rc, 1)
        self.assertIn("survey_boot", codes)
        self.assertIn("survey_seq", codes)


if __name__ == "__main__":
    unittest.main(verbosity=1)
