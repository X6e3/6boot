#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the heartbeat against every fake watchdog scenario (fake_wdt.py) and check what check_heartbeat.py
makes of each. Linux on aarch64 (the build VM); no real watchdog, no /dev/mem.

    wdt_scenarios.py HEARTBEAT_BINARY WORKDIR

Each scenario: the fake starts, the heartbeat runs about 3.3 s with a command line file that has (or, for
"off", lacks) x6e3.hbwdt=1, then the check runs with --json and the expectations below are asserted.
"unavailable" asks for the read with no device tree node to find: the check must FAIL, not pass.
"gap" stops the heartbeat (SIGSTOP) from 0.8 to 2.6 s: reads are missing, and the two writes of 0 inside
that window cannot be told from CUR having stopped for a while: the check must say both.
"""
import json
import os
import shutil
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def codes(r, level=None):
    return [f["code"] for f in r["flags"] if level is None or f["level"] == level]


def kinds(r, k):
    return [e for e in r["events"] if e["kind"] == k]


EXPECT = {
    "feeds": lambda r: len(r["data"]["wdt"]["zeroings"]) == 2 and not r["flags"],
    "takeover": lambda r: (len(r["data"]["wdt"]["zeroings"]) == 2
                           and [(e["from"], e["to"]) for e in kinds(r, "bite")] == [(1_440_000_000, 720_000_000)]
                           and "BITE changed" in r["data"]["wdt"]["zeroings"][0]["with"][0]
                           and "ping" in r["data"]["wdt"]["zeroings"][1]["with"][-1] and not r["flags"]),
    "ctrl": lambda r: ([(e["from"], e["to"]) for e in kinds(r, "ctrl")][:3] == [(4, 0), (0, 4), (4, 5)]
                       and any("RESET_EN came on" in w for z in r["data"]["wdt"]["zeroings"] for w in z["with"])),
    "wrap": lambda r: len(kinds(r, "wrap")) == 1 and not r["data"]["wdt"]["zeroings"] and not r["flags"]
                      and r["data"]["wdt"]["prediction"] is None,
    "stall": lambda r: sorted(set(codes(r, "WARN"))) == ["cur_slow", "cur_stalled"] and not codes(r, "FAIL"),
    "lowered": lambda r: codes(r, "WARN") == ["cur_lowered"] and not r["data"]["wdt"]["zeroings"],
    "fast": lambda r: codes(r, "WARN") == ["cur_fast"],
    "pastbite": lambda r: "past_bite" in codes(r, "WARN"),
    "gap": lambda r: (sorted(codes(r, "WARN")) == ["gap", "possible_zeroing"] and not r["data"]["wdt"]["zeroings"]
                      and len(r["data"]["wdt"]["possible_zeroings"]) == 1 and not codes(r, "FAIL")),
    "off": lambda r: r["data"]["wdt_state"] == "off" and "wdt" not in r["data"] and not r["flags"],
    "unavailable": lambda r: codes(r, "FAIL") == ["wdt_unavailable"] and r["failed"],
}


def scenario(hb, work, name):
    run = os.path.join(work, name)
    shutil.rmtree(run, ignore_errors=True)
    os.makedirs(run)
    with open(os.path.join(run, "cmdline"), "w") as f:
        f.write("console=tty0 x6e3.hb=1" + ("" if name == "off" else " x6e3.hbwdt=1") + "\n")
    fake = subprocess.Popen([sys.executable, "-I", os.path.join(HERE, "fake_wdt.py"), run, "3.8",
                             "feeds" if name in ("gap", "off", "unavailable") else name])
    while not os.path.exists(os.path.join(run, "fake-wdt.ready")):
        time.sleep(0.01)
    logdir = os.path.join(run, "log")
    soc = os.path.join(run, "no-dt" if name == "unavailable" else "dt", "soc")
    beat = subprocess.Popen([hb, logdir, os.path.join(run, "no-watchdog"), soc,
                             os.path.join(run, "fake-mem"), os.path.join(run, "cmdline")])
    if name == "gap":
        time.sleep(0.8)
        beat.send_signal(signal.SIGSTOP)
        time.sleep(1.8)
        beat.send_signal(signal.SIGCONT)
        time.sleep(0.7)
    else:
        time.sleep(3.3)
    beat.terminate()
    beat.wait()
    fake.wait()
    log = [os.path.join(logdir, f) for f in os.listdir(logdir) if f.endswith(".log")][0]
    out = os.path.join(run, "check.json")
    subprocess.run([sys.executable, "-I", os.path.join(HERE, "check_heartbeat.py"), log, "--stopped",
                    "--wdt-tol", "0.05", "--cur-tol-ms", "5", "--json", out], stdout=subprocess.DEVNULL)
    with open(out) as f:
        return json.load(f)


def main():
    hb, work = sys.argv[1], sys.argv[2]
    bad = 0
    for name, ok in EXPECT.items():
        r = scenario(hb, work, name)
        w = r["data"].get("wdt", {})
        summary = (f"zeroings {len(w.get('zeroings', []))}, intervals {w.get('intervals')}, "
                   f"changes {[(e['kind'], e['from'], e['to']) for e in w.get('changes', [])]}, "
                   f"flags {[f['level'] + ':' + f['code'] for f in r['flags']]}")
        good = ok(r)
        bad += not good
        print(f"{'ok  ' if good else 'FAIL'} {name:9s} {summary}")
    print("wdt scenarios:", "passed" if not bad else f"{bad} FAILED")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
