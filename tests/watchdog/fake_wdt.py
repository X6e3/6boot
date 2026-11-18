#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The host test's stand-in for the watchdog (heartbeat.c reads its registers read only through /dev/mem).

    fake_wdt.py RUNDIR SECONDS [SCENARIO]

Makes RUNDIR/dt/soc (#address-cells 2, #size-cells 2, watchdog@10000 with compatible apple,t8103-wdt and
reg 0x10000/0x4000) and RUNDIR/fake-mem, a file the heartbeat maps as its /dev/mem, and keeps WD1 there for
SECONDS as the scenario says (t in seconds from the start; CUR counts at 24 MHz unless said otherwise):

  feeds     BITE 30 s, RESET_EN; CUR written 0 at t = 1.0 and 2.0 (two pings)
  takeover  as 6boot leaves it (BITE 60 s, RESET_EN, CUR 3 s in); at 1.0 CUR 0 and BITE 30 s (the driver's
            set_timeout), at 2.0 CUR 0 (a ping)
  ctrl      BITE 30 s, RESET_EN; at 0.9 CTRL 0 (stop); at 1.6 CUR 0 then CTRL RESET_EN (start); at 2.3 CTRL
            RESET_EN|IRQ_EN; at 3.0 CTRL RESET_EN|IRQ_EN|IRQ_STATUS
  wrap      CTRL 0, CUR 0.9 s short of 2^32: it wraps at 0.9
  stall     CUR stops counting from 1.0 to 2.0, then goes on from where it stopped
  lowered   at 2.0 CUR (2 s in) is set 1.0 s lower, to 1 s: more than a read interval (0.5 s) after
            any read, so no write of 0 between two reads explains it
  fast      at 1.5 CUR jumps 2 s ahead
  pastbite  BITE 1 s, RESET_EN; CUR counts past it (a fake does not reset)
RUNDIR/fake-wdt.ready appears once the registers are set.
"""
import mmap
import os
import struct
import sys
import time

HZ, BASE, SIZE, U32 = 24_000_000, 0x10000, 0x4000, 1 << 32
CUR, BITE, CTRL = BASE + 0x10, BASE + 0x14, BASE + 0x1C


def setup(run):
    soc = os.path.join(run, "dt", "soc")
    node = os.path.join(soc, "watchdog@10000")
    os.makedirs(node, exist_ok=True)
    for name in ("#address-cells", "#size-cells"):
        with open(os.path.join(soc, name), "wb") as f:
            f.write(struct.pack(">I", 2))
    with open(os.path.join(node, "compatible"), "wb") as f:
        f.write(b"apple,t8132-wdt\0apple,t8103-wdt\0")
    with open(os.path.join(node, "reg"), "wb") as f:
        f.write(struct.pack(">QQ", BASE, SIZE))
    path = os.path.join(run, "fake-mem")
    with open(path, "wb") as f:
        f.truncate(BASE + SIZE)
    return path


def main():
    run, seconds = sys.argv[1], float(sys.argv[2])
    sc = sys.argv[3] if len(sys.argv) > 3 else "feeds"
    path = setup(run)
    with open(path, "r+b") as f:
        m = mmap.mmap(f.fileno(), BASE + SIZE)
        put = lambda off, v: struct.pack_into("<I", m, off, v % U32)
        bite, ctrl, base_ticks = 30 * HZ, 4, 0
        if sc == "takeover":
            bite, base_ticks = 60 * HZ, 3 * HZ
        elif sc == "wrap":
            ctrl, base_ticks = 0, U32 - round(0.9 * HZ)
        elif sc == "pastbite":
            bite = HZ
        put(BITE, bite)
        put(CTRL, ctrl)
        put(CUR, base_ticks)
        open(os.path.join(run, "fake-wdt.ready"), "w").close()
        t0 = time.monotonic()
        origin, offset, frozen = t0, base_ticks, None    # CUR = offset + (now - origin) * HZ, unless frozen
        events = {"feeds": [(1.0, "zero"), (2.0, "zero")],
                  "takeover": [(1.0, "zero", "bite30"), (2.0, "zero")],
                  "ctrl": [(0.9, "ctrl0"), (1.6, "zero", "ctrl4"), (2.3, "ctrl5"), (3.0, "ctrl7")],
                  "stall": [(1.0, "freeze"), (2.0, "thaw")],
                  "lowered": [(2.0, "lower")],
                  "fast": [(1.5, "ahead")]}.get(sc, [])
        while (now := time.monotonic()) - t0 < seconds:
            el = now - t0
            while events and el >= events[0][0]:
                for act in events.pop(0)[1:]:
                    cur = frozen if frozen is not None else offset + (now - origin) * HZ
                    if act == "zero":
                        origin, offset = now, 0
                    elif act == "bite30":
                        put(BITE, 30 * HZ)
                    elif act.startswith("ctrl"):
                        put(CTRL, int(act[4:]))
                    elif act == "freeze":
                        frozen = cur
                    elif act == "thaw":
                        origin, offset, frozen = now, frozen, None
                    elif act == "lower":
                        origin, offset = now, cur - 1.0 * HZ
                    elif act == "ahead":
                        origin, offset = now, cur + 2 * HZ
            put(CUR, int(frozen if frozen is not None else offset + (now - origin) * HZ))
            time.sleep(0.002)
        m.close()


if __name__ == "__main__":
    main()
