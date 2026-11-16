#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The ESP reader and the whole trial check on disks built around FAT32 volumes that macOS made.

Disks with 4096-byte blocks, as on the J713's NVMe, are assembled here: a GPT (with its CRCs) and one
partition at block 256 holding a FAT32 volume made and written by macOS through hdiutil. A disk with
512-byte blocks is made by hdiutil itself, GPT included. Each scenario changes the files on the volume
through the mounted file system and keeps the disk (sparse); every check runs `esp-test` on a disk and
compares with what this file worked out itself: a directory walk, hashlib, the record's offset found
by searching the raw bytes. Never touches a real disk.

usage: esp_fixture_test.py ESP_TEST                  build the disks in a temporary folder, check, clean up
       esp_fixture_test.py ESP_TEST --build DIR      only build the disks and DIR/manifest.json
       esp_fixture_test.py ESP_TEST --check DIR      only check (mutate_esp.sh: build once, check per mutant)
"""
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid as uuidlib
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import bootstate as bs  # noqa: E402

UUID = "3f1a2b4c-5d6e-4f70-8192-a3b4c5d6e7f8"
ZERO = "0" * 64


def sh(*a):
    subprocess.run(a, check=True)


def walk(raw, path):
    """First cluster and size of a file on a FAT32 volume, by its directories (independent of the C code)."""
    bps, spc = struct.unpack_from("<H", raw, 11)[0], raw[13]
    reserved, fats = struct.unpack_from("<H", raw, 14)[0], raw[16]
    fat_size, root = struct.unpack_from("<I", raw, 36)[0], struct.unpack_from("<I", raw, 44)[0]
    data, csize = (reserved + fats * fat_size) * bps, spc * bps

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
                name = "".join(lfn[k] for k in sorted(lfn)).split("\0")[0] if lfn else \
                    e[0:8].decode().strip() + ("." + e[8:11].decode().strip() if e[8:11].strip() else "")
                lfn = {}
                yield name, struct.unpack_from("<H", e, 20)[0] << 16 | struct.unpack_from("<H", e, 26)[0], \
                    struct.unpack_from("<I", e, 28)[0]

    c, size = root, 0
    for part in path.split("/"):
        found = [(f, s) for n, f, s in entries(c) if n.lower() == part.lower()]
        if len(found) != 1:
            return None
        c, size = found[0]
    return c, size


def gpt_disk(vol, part_uuid, block=4096, first=256, damage_crc=False, damage_ecrc=False, sig=b"EFI PART"):
    """A GPT disk of `block`-byte blocks with one partition holding `vol`."""
    nblk = (len(vol) + block - 1) // block
    last = first + nblk - 1
    total = last + 1 + 34
    entries = bytearray(128 * 128)
    entries[0:16] = uuidlib.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B").bytes_le      # EFI system
    entries[16:32] = uuidlib.UUID(part_uuid).bytes_le
    struct.pack_into("<QQ", entries, 32, first, last)
    ecrc = zlib.crc32(bytes(entries)) ^ (1 if damage_ecrc else 0)
    hdr = bytearray(92)
    struct.pack_into("<8sIIIIQQQQ16sQIII", hdr, 0, sig, 0x10000, 92, 0, 0, 1, total - 1, 34, last,
                     uuidlib.uuid4().bytes_le, 2, 128, 128, ecrc)
    struct.pack_into("<I", hdr, 16, zlib.crc32(bytes(hdr)) ^ (1 if damage_crc else 0))
    return {0: bytes(block), block: bytes(hdr), 2 * block: bytes(entries), first * block: vol}, total * block


def write_sparse(path, parts, size):
    """Write {offset: bytes} into a sparse file of `size` bytes, skipping zero runs."""
    with open(path, "wb") as f:
        for off, data in sorted(parts.items()):
            for o in range(0, len(data), 1 << 20):
                chunk = data[o:o + (1 << 20)]
                if chunk.count(0) != len(chunk):
                    f.seek(off + o)
                    f.write(chunk)
        f.truncate(size)


class Volume:
    """A FAT32 volume made by macOS, changed through the mounted file system."""

    def __init__(self, tmp, name, size, fsargs):
        self.tmp = tmp
        self.img, self.mnt = os.path.join(tmp, name + ".dmg"), os.path.join(tmp, name + "-mnt")
        sh(*(["hdiutil", "create", "-quiet", "-size", size, "-fs", "MS-DOS FAT32", "-volname", "ESP",
              "-layout", "NONE"] + (["-fsargs", fsargs] if fsargs else []) + [self.img]))
        os.mkdir(self.mnt)
        # 254 files in one directory: it runs over several clusters (macOS adds a ._ file for each)
        self.once = {f"FILL/F{k:03d}.BIN": bytes([k & 0xff]) for k in range(254)}

    def set_files(self, files):
        sh("hdiutil", "attach", "-quiet", "-nobrowse", "-mountpoint", self.mnt, self.img)
        try:
            for path, data in self.once.items():
                full = os.path.join(self.mnt, path)
                if not os.path.exists(full):
                    os.makedirs(os.path.dirname(full), exist_ok=True)
                    with open(full, "wb") as f:
                        f.write(data)
            self.once = {}
            for path in ("M1N1/BOOT.BIN", "M1N1/BOOT-TRIAL.BIN", "j713/bootstate.bin"):
                p = os.path.join(self.mnt, path)
                if os.path.exists(p):
                    os.unlink(p)
            for path, data in files.items():
                os.makedirs(os.path.dirname(os.path.join(self.mnt, path)), exist_ok=True)
                with open(os.path.join(self.mnt, path), "wb") as f:
                    f.write(data)
        finally:
            sh("hdiutil", "detach", "-quiet", self.mnt)
        out = os.path.join(self.tmp, "raw")
        if os.path.exists(out + ".cdr"):
            os.unlink(out + ".cdr")
        sh("hdiutil", "convert", "-quiet", self.img, "-format", "UDTO", "-o", out)
        with open(out + ".cdr", "rb") as f:
            raw = f.read()
        os.unlink(out + ".cdr")
        return raw


def build(d):
    tests = []
    trial = os.urandom(3 * 1024 * 1024 + 123)
    boot = os.urandom(200 * 1024)
    armed = bs.encode(bs.arm(trial, tries=1, prev=bs.Record(seq=40)))
    n = [0]

    def disk(vol, files, block=4096, first=256, uuid=UUID, patch=None, **kw):
        raw = vol.set_files(files)
        if patch:
            raw = patch(raw)
        parts, size = gpt_disk(raw, uuid, block=block, first=first, **kw)
        n[0] += 1
        name = f"disk{n[0]}.img"
        write_sparse(os.path.join(d, name), parts, size)
        return raw, name

    def check(what, name, want, block=4096):
        tests.append({"what": what, "args": ["check", name, block, UUID], "want": want})

    aligned = Volume(d, "aligned", "300m", "-c 8 -a 600")
    files = {"M1N1/BOOT.BIN": boot, "M1N1/BOOT-TRIAL.BIN": trial, "j713/bootstate.bin": armed}
    raw, name = disk(aligned, files)
    rec, tri = walk(raw, "j713/bootstate.bin"), walk(raw, "M1N1/BOOT-TRIAL.BIN")
    where = [i for i in range(0, len(raw), 512) if raw[i:i + 512] == armed]
    assert len(where) == 1 and where[0] % 4096 == 0
    check("armed, trial matches", name,
          {"boot": "trial", "reason": "trial", "write": "1", "watchdog": "1", "failed_at": "none",
           "record": f"{rec[0]}/{rec[1]}", "trial": f"{tri[0]}/{tri[1]}",
           "sha": hashlib.sha256(trial).hexdigest(), "block": str(256 + where[0] // 4096),
           "out": bs.encode(bs.Record(**{**bs.decode(armed).__dict__, "tries_left": 0, "seq": 42})).hex()})
    for path, data in (("M1N1/BOOT.BIN", boot), ("j713/bootstate.bin", armed), ("J713/BOOTSTATE.BIN", armed),
                       ("m1n1/boot-trial.bin", trial)):
        c = walk(raw, path)
        tests.append({"what": f"lookup {path}", "args": ["lookup", name, 4096, UUID, path],
                      "line": f"{c[0]} {len(data)} {hashlib.sha256(data).hexdigest()}"})
    fill = walk(raw, "FILL/F253.BIN")
    tests.append({"what": "the last name of a full directory", "args": ["lookup", name, 4096, UUID, "FILL/F253.BIN"],
                  "line": f"{fill[0]} 1 {hashlib.sha256(bytes([253])).hexdigest()}"})
    for path in ("j713/nothere.bin", "j713", "nothere/bootstate.bin", "FILL/NOTHERE.BIN"):
        tests.append({"what": f"lookup {path}", "args": ["lookup", name, 4096, UUID, path], "line": "err -5"})
    tests.append({"what": "no such partition", "args": ["find", name, 4096, "00000000-0000-4000-8000-000000000000"],
                  "line": "err -3"})

    changed = bytearray(trial)
    changed[len(changed) // 2] ^= 1
    _, name = disk(aligned, {"M1N1/BOOT-TRIAL.BIN": bytes(changed), "j713/bootstate.bin": armed})
    check("trial changed after arming", name, {"boot": "own", "reason": "trial-mismatch", "write": "0",
                                               "sha": hashlib.sha256(bytes(changed)).hexdigest()})
    _, name = disk(aligned, {"M1N1/BOOT-TRIAL.BIN": trial[:-1], "j713/bootstate.bin": armed})
    check("trial of another size: refused before any hashing", name,
          {"boot": "own", "reason": "trial-mismatch", "write": "0", "sha": ZERO})
    _, name = disk(aligned, {"j713/bootstate.bin": armed})
    check("no trial file", name, {"boot": "own", "reason": "trial-missing", "write": "0", "failed_at": "none"})
    _, name = disk(aligned, {"M1N1/BOOT-TRIAL.BIN": trial, "j713/bootstate.bin": bs.encode(bs.idle())})
    check("idle record: the trial is not even looked up", name,
          {"boot": "own", "reason": "idle", "write": "0", "trial": "0/0", "sha": ZERO})
    _, name = disk(aligned, {"M1N1/BOOT-TRIAL.BIN": trial})
    check("no record", name, {"boot": "own", "reason": "no-record", "write": "0", "failed_at": "none"})
    spent = bs.encode(bs.Record(seq=41, state=bs.ARMED, tries_left=0, tries_set=1, trial_size=len(trial),
                                trial_sha256=hashlib.sha256(trial).digest()))
    _, name = disk(aligned, {"M1N1/BOOT-TRIAL.BIN": trial, "j713/bootstate.bin": spent})
    check("the trial never reported back", name,
          {"boot": "own", "reason": "trial-failed", "show": "2", "write": "0", "trial": "0/0"})
    bad = bytearray(armed)
    bad[100] ^= 4
    _, name = disk(aligned, {"M1N1/BOOT-TRIAL.BIN": trial, "j713/bootstate.bin": bytes(bad)})
    check("damaged record", name, {"boot": "own", "reason": "bad-record", "parse": "4", "write": "0"})
    _, name = disk(aligned, {"M1N1/BOOT-TRIAL.BIN": trial, "j713/bootstate.bin": armed + b"\0"})
    check("record longer than 512 bytes", name,
          {"boot": "own", "reason": "bad-record", "write": "0", "failed_at": "read"})
    files = {"M1N1/BOOT-TRIAL.BIN": trial, "j713/bootstate.bin": armed}
    _, name = disk(aligned, files, uuid="11111111-2222-4333-8444-555555555555")
    check("the ESP's UUID is not on the disk", name,
          {"boot": "own", "reason": "no-record", "failed_at": "gpt", "err": "-3"})
    _, name = disk(aligned, files, damage_crc=True)
    check("GPT header CRC damaged", name, {"boot": "own", "reason": "no-record", "failed_at": "gpt", "err": "-2"})
    _, name = disk(aligned, files, sig=b"EFI PARX")
    check("GPT signature wrong, CRCs right", name,
          {"boot": "own", "reason": "no-record", "failed_at": "gpt", "err": "-2"})
    _, name = disk(aligned, files, damage_ecrc=True)
    check("GPT entry array CRC damaged", name,
          {"boot": "own", "reason": "no-record", "failed_at": "gpt", "err": "-2"})

    def fat_entry(cluster, value):
        """Set one FAT32 entry in both FAT copies, as a damaged volume would have it."""
        def patch(raw):
            raw = bytearray(raw)
            bps, reserved = struct.unpack_from("<H", raw, 11)[0], struct.unpack_from("<H", raw, 14)[0]
            fats, fat_size = raw[16], struct.unpack_from("<I", raw, 36)[0]
            for k in range(fats):
                struct.pack_into("<I", raw, (reserved + k * fat_size) * bps + 4 * cluster, value)
            return bytes(raw)
        return patch
    first_trial = tri[0]
    _, name = disk(aligned, files, patch=fat_entry(first_trial, 0x0FFFFFFF))
    check("the trial's cluster chain ends after one cluster", name,
          {"boot": "own", "reason": "trial-missing", "failed_at": "hash", "err": "-6", "write": "0"})
    _, name = disk(aligned, files, patch=fat_entry(first_trial, first_trial))
    check("the trial's cluster chain loops on itself", name,
          {"boot": "own", "reason": "trial-missing", "failed_at": "hash", "err": "-6", "write": "0"})

    unaligned = Volume(d, "unaligned", "300m", "-c 8")
    _, name = disk(unaligned, files)
    check("FAT not aligned to 4096-byte blocks", name,
          {"boot": "own", "reason": "sector-mismatch", "failed_at": "locate", "write": "0"})
    _, name = disk(unaligned, files, block=512, first=2048)
    check("512-byte blocks", name, {"boot": "trial", "reason": "trial", "write": "1"}, block=512)

    mac = os.path.join(d, "mac.dmg")
    sh("hdiutil", "create", "-quiet", "-size", "120m", "-fs", "MS-DOS FAT32", "-volname", "MACGPT",
       "-layout", "GPTSPUD", mac)
    sh("hdiutil", "convert", "-quiet", mac, "-format", "UDTO", "-o", os.path.join(d, "mac"))
    with open(os.path.join(d, "mac.cdr"), "rb") as f:
        raw = f.read(1 << 20)
    hdr = raw[512:512 + 92]
    cnt, esize, lba = struct.unpack_from("<I", hdr, 80)[0], struct.unpack_from("<I", hdr, 84)[0], \
        struct.unpack_from("<Q", hdr, 72)[0]
    for i in range(cnt):
        e = raw[lba * 512 + i * esize: lba * 512 + (i + 1) * esize]
        if any(e[:16]):
            lo, hi = struct.unpack_from("<QQ", e, 32)
            tests.append({"what": "a GPT macOS wrote", "args": ["find", "mac.cdr", 512,
                                                                 str(uuidlib.UUID(bytes_le=e[16:32])).upper()],
                          "line": f"{lo} {hi - lo + 1}"})
    for f in ("aligned.dmg", "unaligned.dmg", "mac.dmg"):
        os.unlink(os.path.join(d, f))
    with open(os.path.join(d, "manifest.json"), "w") as f:
        json.dump(tests, f, indent=1)


def check_all(c_test, d, quiet=False):
    with open(os.path.join(d, "manifest.json")) as f:
        tests = json.load(f)
    checks = failures = 0
    for t in tests:
        args = [os.path.join(d, a) if isinstance(a, str) and (a.endswith(".img") or a.endswith(".cdr")) else a
                for a in t["args"]]
        try:
            out = subprocess.run([c_test, *map(str, args)], capture_output=True, text=True, timeout=60).stdout
        except subprocess.TimeoutExpired:
            out = "timeout"
        if "line" in t:
            checks += 1
            if out.strip() != t["line"]:
                failures += 1
                if not quiet:
                    print(f"FAIL: {t['what']}: {out.strip()!r}, expected {t['line']!r}", file=sys.stderr)
            continue
        got = dict(w.split("=", 1) for line in out.split("\n") for w in line.split() if "=" in w)
        for k, v in t["want"].items():
            checks += 1
            if got.get(k) != v:
                failures += 1
                if not quiet:
                    print(f"FAIL: {t['what']}: {k}={got.get(k)}, expected {v}", file=sys.stderr)
    return checks, failures


if __name__ == "__main__":
    if sys.platform != "darwin" or not shutil.which("hdiutil"):
        print("esp fixture: skipped, needs macOS hdiutil")
        sys.exit(0)
    c_test = sys.argv[1]
    if len(sys.argv) == 4 and sys.argv[2] == "--build":
        os.makedirs(sys.argv[3], exist_ok=True)
        build(sys.argv[3])
        sys.exit(0)
    if len(sys.argv) == 4 and sys.argv[2] == "--check":
        checks, failures = check_all(c_test, sys.argv[3], quiet=True)
        print(f"esp fixture: {checks} checks, {failures} failed")
        sys.exit(1 if failures else 0)
    tmp = tempfile.mkdtemp(prefix="esp-fixture-")
    try:
        build(tmp)
        checks, failures = check_all(c_test, tmp)
    finally:
        subprocess.run(["rm", "-rf", tmp], check=False)
    print(f"esp fixture: {checks} checks, {failures} failed")
    sys.exit(1 if failures else 0)
