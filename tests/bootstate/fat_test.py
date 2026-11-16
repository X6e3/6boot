#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""xb_bs_locate() on FAT32 file systems that macOS made and wrote, never on a real disk.

For each layout: make an image with hdiutil, write files on it through the mounted volume, take the
raw bytes, find j713/bootstate.bin's first cluster by walking the directories here, independently of
the C code, and check that the device block xb_bs_locate() gives holds the record, and that the
record is nowhere else. With 4096-byte device blocks, as on the J713's NVMe, a layout whose clusters
are smaller than a block or not aligned to one must be refused.

usage: fat_test.py BOOTSTATE_TEST
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import bootstate as bs  # noqa: E402

if sys.platform != "darwin" or not shutil.which("hdiutil"):
    print("bootstate fat: skipped, needs macOS hdiutil")
    sys.exit(0)

C = sys.argv[1]
checks = failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print("FAIL:", what, file=sys.stderr)


def walk(raw, path):
    """First cluster and size of a file, from the FAT32 directories."""
    bps, spc = struct.unpack_from("<H", raw, 11)[0], raw[13]
    reserved, fats = struct.unpack_from("<H", raw, 14)[0], raw[16]
    fat_size, root = struct.unpack_from("<I", raw, 36)[0], struct.unpack_from("<I", raw, 44)[0]
    data = (reserved + fats * fat_size) * bps
    csize = spc * bps

    def chain(c):
        while 2 <= c < 0x0FFFFFF8:
            yield raw[data + (c - 2) * csize: data + (c - 1) * csize]
            c = struct.unpack_from("<I", raw, reserved * bps + 4 * c)[0] & 0x0FFFFFFF

    def entries(c):
        lfn = {}
        for clus in chain(c):
            for o in range(0, csize, 32):
                e = clus[o:o + 32]
                if e[0] == 0:
                    return
                if e[0] == 0xE5:
                    lfn = {}
                    continue
                if e[11] == 0x0F:
                    lfn[e[0] & 0x3F] = (e[1:11] + e[14:26] + e[28:32]).decode("utf-16-le")
                    continue
                if lfn:
                    name = "".join(lfn[k] for k in sorted(lfn)).split("\0")[0]
                else:
                    base, ext = e[0:8].decode().strip(), e[8:11].decode().strip()
                    name = base + ("." + ext if ext else "")
                lfn = {}
                first = struct.unpack_from("<H", e, 20)[0] << 16 | struct.unpack_from("<H", e, 26)[0]
                yield name, first, struct.unpack_from("<I", e, 28)[0]

    c, size = root, 0
    for part in path.split("/"):
        found = [(f, s) for n, f, s in entries(c) if n.lower() == part.lower()]
        if len(found) != 1:
            raise RuntimeError(f"{part} not found once in {path}")
        c, size = found[0]
    return c, size


def run(*args):
    return subprocess.run([C, *map(str, args)], check=True, capture_output=True, text=True).stdout.strip()


# (name, size, newfs_msdos arguments): 512-byte clusters; 4 KiB clusters with the data area off a
# 4 KiB boundary (macOS's default placement); 4 KiB clusters on a 4 KiB boundary (FAT size chosen).
LAYOUTS = [("c1", "64m", ""), ("c8", "300m", "-c 8"), ("c8-aligned", "300m", "-c 8 -a 600")]
tmp = tempfile.mkdtemp(prefix="bootstate-fat-")
try:
    for name, size, fsargs in LAYOUTS:
        img, mnt = os.path.join(tmp, name + ".dmg"), os.path.join(tmp, name + "-mnt")
        args = ["hdiutil", "create", "-quiet", "-size", size, "-fs", "MS-DOS FAT32", "-volname",
                "XBST", "-layout", "NONE"] + (["-fsargs", fsargs] if fsargs else []) + [img]
        subprocess.run(args, check=True)
        os.mkdir(mnt)
        subprocess.run(["hdiutil", "attach", "-quiet", "-nobrowse", "-mountpoint", mnt, img], check=True)
        try:
            os.makedirs(os.path.join(mnt, "M1N1"))
            os.makedirs(os.path.join(mnt, "j713"))
            trial = os.urandom(300 * 1024)
            with open(os.path.join(mnt, "M1N1", "BOOT.BIN"), "wb") as f:
                f.write(os.urandom(200 * 1024))
            with open(os.path.join(mnt, "M1N1", "BOOT-TRIAL.BIN"), "wb") as f:
                f.write(trial)
            record = bs.encode(bs.arm(trial, tries=2))
            with open(os.path.join(mnt, "j713", "bootstate.bin"), "wb") as f:
                f.write(record)
        finally:
            subprocess.run(["hdiutil", "detach", "-quiet", mnt], check=True)
        subprocess.run(["hdiutil", "convert", "-quiet", img, "-format", "UDTO", "-o",
                        os.path.join(tmp, name)], check=True)
        raw = open(os.path.join(tmp, name + ".cdr"), "rb").read()
        bpb = os.path.join(tmp, name + ".bpb")
        open(bpb, "wb").write(raw[:512])

        first, fsize = walk(raw, "j713/bootstate.bin")
        check(fsize == 512, f"{name}: the record file is {fsize} bytes")
        where = [i for i in range(0, len(raw), 512) if raw[i:i + 512] == record]
        check(len(where) == 1, f"{name}: the record is in {len(where)} places")
        bps, spc = struct.unpack_from("<H", raw, 11)[0], raw[13]
        blocks512 = len(raw) // 512

        got = run("locate", bpb, 0, blocks512, 512, first, fsize)
        check(got.isdigit() and int(got) * 512 == where[0],
              f"{name}: 512-byte blocks: locate says {got}, the record is at {where[0] // 512}")

        offset = where[0]
        fits = spc * bps >= 4096 and offset % 4096 == 0
        got = run("locate", bpb, 0, len(raw) // 4096, 4096, first, fsize)
        if fits:
            check(got.isdigit() and int(got) * 4096 == offset,
                  f"{name}: 4096-byte blocks: locate says {got}, the record is at {offset / 4096}")
        else:
            check(got == "err -3", f"{name}: 4096-byte blocks should be refused (align), got {got}")
        print(f"  {name}: {bps}-byte sectors, {spc} per cluster, record in cluster {first} at byte "
              f"{offset}; 4096-byte blocks: {'block ' + got if fits else 'refused'}")
finally:
    subprocess.run(["rm", "-rf", tmp], check=False)

print(f"bootstate fat: {checks} checks, {failures} failed")
sys.exit(1 if failures else 0)
