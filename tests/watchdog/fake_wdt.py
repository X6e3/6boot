#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The host test's stand-in for the watchdog (heartbeat.c reads its registers read only through /dev/mem).

    fake_wdt.py RUNDIR SECONDS

Makes RUNDIR/dt/soc (#address-cells 2, #size-cells 2, watchdog@10000 with compatible apple,t8103-wdt and
reg 0x10000/0x4000) and RUNDIR/fake-mem, a file the heartbeat maps as its /dev/mem. For SECONDS it keeps WD1
there as a running watchdog would look: CUR counts at 24 MHz since the last feed, BITE is 30 s, CTRL has
RESET_EN; one feed (CUR back to 0) 1 s after the start. RUNDIR/fake-wdt.ready appears once the registers
are set; RUNDIR/fake-wdt.feed holds the time of the feed.
"""
import mmap
import os
import struct
import sys
import time

HZ, BASE, SIZE = 24_000_000, 0x10000, 0x4000


def main():
    run, seconds = sys.argv[1], float(sys.argv[2])
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
    with open(path, "r+b") as f:
        m = mmap.mmap(f.fileno(), BASE + SIZE)
        struct.pack_into("<I", m, BASE + 0x14, 30 * HZ)
        struct.pack_into("<I", m, BASE + 0x1C, 4)
        open(os.path.join(run, "fake-wdt.ready"), "w").close()
        t0 = fed = time.monotonic()
        done = False
        while (now := time.monotonic()) - t0 < seconds:
            if not done and now - t0 >= 1.0:
                fed, done = now, True
                with open(os.path.join(run, "fake-wdt.feed"), "w") as g:
                    g.write(f"{now - t0:.3f}\n")
            struct.pack_into("<I", m, BASE + 0x10, int((now - fed) * HZ))
            time.sleep(0.002)
        m.close()


if __name__ == "__main__":
    main()
