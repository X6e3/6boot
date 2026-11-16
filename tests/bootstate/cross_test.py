#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""tools/bootstate.py (the Linux side) against src/xboot_bootstate.c (6boot), through the C test's CLI.

usage: cross_test.py BOOTSTATE_TEST
"""
import os
import random
import subprocess
import sys
import tempfile
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import bootstate as bs  # noqa: E402

C = sys.argv[1]
TMP = tempfile.mkdtemp(prefix="bootstate-cross-")
checks = failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print("FAIL:", what, file=sys.stderr)


def run(*args):
    return subprocess.run([C, *map(str, args)], check=True, capture_output=True, text=True).stdout.strip()


def c_parse(raw):
    path = os.path.join(TMP, "rec.bin")
    with open(path, "wb") as f:
        f.write(raw)
    return run("parse", path)


def c_decide(raw, trial, sector):
    path = os.path.join(TMP, "dec.bin")
    if raw is None:
        if os.path.exists(path):
            os.unlink(path)
    else:
        with open(path, "wb") as f:
            f.write(raw)
    return run("decide", path, trial, sector).split()


def as_c(r):
    return f"ok {r.seq} {r.state} {r.tries_left} {r.tries_set} {r.trial_size} {r.trial_sha256.hex()}"


rng = random.Random(713)


def random_record():
    if rng.random() < 0.3:
        return bs.Record(seq=rng.randrange(2**32))
    tries = rng.randint(1, bs.MAX_TRIES)
    return bs.Record(seq=rng.randrange(2**32), state=bs.ARMED, tries_left=rng.randint(0, tries),
                     tries_set=tries, trial_size=rng.randrange(1, 2**32),
                     trial_sha256=bytes(rng.randrange(256) for _ in range(32)))


# 1. The same fields give the same bytes on both sides, and each side reads the other's.
for _ in range(300):
    r = bs.Record() if _ == 0 else random_record()
    py = bs.encode(r)
    c = bytes.fromhex(run("encode", r.seq, r.state, r.tries_left, r.tries_set, r.trial_size,
                          r.trial_sha256.hex()))
    check(py == c, f"encode differs for {r}")
    check(c_parse(py) == as_c(r), f"C does not read Python's {r}")
    check(bs.decode(c) == r, f"Python does not read C's {r}")

# 2. Both refuse the same damaged records, with the same reason.
base = bs.encode(bs.arm(b"x" * 1000, tries=2))
for _ in range(400):
    raw = bytearray(base)
    kind = rng.randrange(4)
    if kind == 0:  # flip bits anywhere
        for _ in range(rng.randint(1, 3)):
            raw[rng.randrange(512)] ^= 1 << rng.randrange(8)
    elif kind == 1:  # change a field and fix the CRC, so only the field rules can refuse it
        raw[rng.choice([4, 6, 7, 12, 13, 14, 15]) if rng.random() < 0.7 else rng.randrange(52, 508)] = rng.randrange(256)
        raw[508:] = zlib.crc32(raw[:508]).to_bytes(4, "little")
    elif kind == 2:  # cut or extend
        raw = raw[: rng.randrange(512)] if rng.random() < 0.5 else raw + b"\0" * rng.randint(1, 8)
    else:  # wrong magic
        raw[rng.randrange(4)] ^= 0x20
    raw = bytes(raw)
    try:
        want = as_c(bs.decode(raw))
    except bs.RecordError as e:
        want = f"err {e.code}"
    check(c_parse(raw) == want, f"C and Python disagree on a damaged record ({want})")

# 3. A whole trial from the Linux side: arm, boots that never report back, then a disarm.
image = os.urandom(4096)
rec = bs.arm(image, tries=2, prev=bs.Record(seq=9))
raw = bs.encode(rec)
trials = 0
for boot in range(4):
    d = c_decide(raw, 4, 1)  # trial OK, sector OK
    if d[0] == "1":
        trials += 1
        after = bs.decode(bytes.fromhex(d[5]))
        check(after.tries_left == bs.decode(raw).tries_left - 1 and after.seq == bs.decode(raw).seq + 1,
              "6boot's write lowers tries left by one and adds one to seq")
        raw = bytes.fromhex(d[5])
    else:
        check(d[2] == "trial-failed" and d[1] == "2", f"after the tries: {d}")
check(trials == 2, f"the trial ran {trials} times, not 2")
done = bs.idle(prev=bs.decode(raw))
check(c_decide(bs.encode(done), 4, 1)[2] == "idle", "Linux's disarm reads as idle")
check(c_decide(None, 0, 0)[2] == "no-record", "no file, no trial")

# 4. A trial that booted fully on the first try: Linux promotes it and writes idle.
rec = bs.arm(image, tries=3)
d = c_decide(bs.encode(rec), 4, 1)
check(d[2] == "trial" and d[4] == "1", "a trial boot arms the watchdog")
check(bs.decode(bytes.fromhex(d[5])).tries_left == 2, "two tries left")
check(c_decide(bs.encode(bs.idle(prev=bs.decode(bytes.fromhex(d[5])))), 4, 1)[2] == "idle", "promoted")

print(f"bootstate cross: {checks} checks, {failures} failed")
sys.exit(1 if failures else 0)
