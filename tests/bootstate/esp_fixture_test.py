#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The ESP reader and the whole trial check on disks built around FAT32 volumes that macOS made.

Disks with 4096-byte blocks, as on the J713's NVMe, are assembled here: a GPT (with its CRCs) and one
partition at block 256 holding a FAT32 volume made and written by macOS through hdiutil. A disk with
512-byte blocks is made by hdiutil itself, GPT included. Each scenario changes the files on the volume
through the mounted file system, rebuilds the disk and runs `esp-test check` on it; lookups are checked
against a directory walk done here and hashes against hashlib. Never touches a real disk.

usage: esp_fixture_test.py ESP_TEST
"""
import hashlib
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

if sys.platform != "darwin" or not shutil.which("hdiutil"):
    print("esp fixture: skipped, needs macOS hdiutil")
    sys.exit(0)

C = sys.argv[1]
TMP = tempfile.mkdtemp(prefix="esp-fixture-")
checks = failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print("FAIL:", what, file=sys.stderr)


def sh(*a):
    subprocess.run(a, check=True)


def run(*args):
    return subprocess.run([C, *map(str, args)], check=True, capture_output=True, text=True).stdout


def kv(out):
    return dict(w.split("=", 1) for line in out.split("\n") for w in line.split() if "=" in w)


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


def gpt_disk(vol, part_uuid, block=4096, first=256, damage_crc=False):
    """A GPT disk of `block`-byte blocks with one partition holding `vol`."""
    nblk = (len(vol) + block - 1) // block
    last = first + nblk - 1
    total = last + 1 + 34
    entries = bytearray(128 * 128)
    entries[0:16] = uuidlib.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B").bytes_le      # EFI system
    entries[16:32] = uuidlib.UUID(part_uuid).bytes_le
    struct.pack_into("<QQ", entries, 32, first, last)
    ecrc = zlib.crc32(bytes(entries))
    hdr = bytearray(92)
    struct.pack_into("<8sIIIIQQQQ16sQIII", hdr, 0, b"EFI PART", 0x10000, 92, 0, 0, 1, total - 1, 34, last,
                     uuidlib.uuid4().bytes_le, 2, 128, 128, ecrc)
    struct.pack_into("<I", hdr, 16, zlib.crc32(bytes(hdr)) ^ (1 if damage_crc else 0))
    disk = bytearray(total * block)
    disk[block:block + 92] = hdr
    disk[2 * block:2 * block + len(entries)] = entries
    disk[first * block:first * block + len(vol)] = vol
    return bytes(disk)


class Volume:
    """A FAT32 volume made by macOS, changed through the mounted file system."""

    def __init__(self, name, size, fsargs):
        self.img, self.mnt = os.path.join(TMP, name + ".dmg"), os.path.join(TMP, name + "-mnt")
        args = ["hdiutil", "create", "-quiet", "-size", size, "-fs", "MS-DOS FAT32", "-volname", "ESP",
                "-layout", "NONE"] + (["-fsargs", fsargs] if fsargs else []) + [self.img]
        sh(*args)
        os.mkdir(self.mnt)

    def change(self, fn):
        sh("hdiutil", "attach", "-quiet", "-nobrowse", "-mountpoint", self.mnt, self.img)
        try:
            fn(self.mnt)
        finally:
            sh("hdiutil", "detach", "-quiet", self.mnt)

    def raw(self):
        out = os.path.join(TMP, "raw")
        if os.path.exists(out + ".cdr"):
            os.unlink(out + ".cdr")
        sh("hdiutil", "convert", "-quiet", self.img, "-format", "UDTO", "-o", out)
        return open(out + ".cdr", "rb").read()


def put(mnt, path, data):
    os.makedirs(os.path.dirname(os.path.join(mnt, path)), exist_ok=True)
    with open(os.path.join(mnt, path), "wb") as f:
        f.write(data)


def rm(mnt, path):
    p = os.path.join(mnt, path)
    if os.path.exists(p):
        os.unlink(p)


UUID = "3f1a2b4c-5d6e-4f70-8192-a3b4c5d6e7f8"
trial = os.urandom(3 * 1024 * 1024 + 123)
boot = os.urandom(200 * 1024)
armed = bs.encode(bs.arm(trial, tries=1, prev=bs.Record(seq=40)))


def scenario(vol, what, files, want, block=4096, **kw):
    def change(mnt):
        for path in ("M1N1/BOOT.BIN", "M1N1/BOOT-TRIAL.BIN", "j713/bootstate.bin"):
            rm(mnt, path)
        for path, data in files.items():
            put(mnt, path, data)
    vol.change(change)
    raw = vol.raw()
    disk = gpt_disk(raw, kw.pop("disk_uuid", UUID), block=block, **kw)
    path = os.path.join(TMP, "disk.img")
    open(path, "wb").write(disk)
    out = run("check", path, block, UUID)
    got = kv(out)
    for k, v in want.items():
        check(got.get(k) == v, f"{what}: {k}={got.get(k)}, expected {v}\n{out}")
    return raw, path, got


tmp_ok = False
try:
    aligned = Volume("aligned", "300m", "-c 8 -a 600")
    raw, disk, got = scenario(aligned, "armed, trial matches",
                              {"M1N1/BOOT.BIN": boot, "M1N1/BOOT-TRIAL.BIN": trial, "j713/bootstate.bin": armed},
                              {"boot": "trial", "reason": "trial", "write": "1", "watchdog": "1", "failed_at": "none"})
    # what the C code found, against this file's own reading of the volume and hashlib
    rec = walk(raw, "j713/bootstate.bin")
    tri = walk(raw, "M1N1/BOOT-TRIAL.BIN")
    check(got.get("record") == f"{rec[0]}/{rec[1]}", f"record cluster/size {got.get('record')} vs {rec}")
    check(got.get("trial") == f"{tri[0]}/{tri[1]}", f"trial cluster/size {got.get('trial')} vs {tri}")
    check(got.get("sha") == hashlib.sha256(trial).hexdigest(), "trial SHA-256")
    where = [i for i in range(0, len(raw), 512) if raw[i:i + 512] == armed]
    check(len(where) == 1 and got.get("block") == str(256 + where[0] // 4096),
          f"record block {got.get('block')} vs {[256 + w // 4096 for w in where]}")
    after = bs.decode(bytes.fromhex(got["out"]))
    check(after.tries_left == 0 and after.seq == 42, f"the record 6boot would write: {after}")  # armed at 41
    for path in ("M1N1/BOOT.BIN", "j713/bootstate.bin"):
        line = run("lookup", disk, 4096, UUID, path).split()
        want = {"M1N1/BOOT.BIN": boot, "j713/bootstate.bin": armed}[path]
        check(line[2] == hashlib.sha256(want).hexdigest() and int(line[1]) == len(want), f"lookup {path}: {line}")
    check(run("lookup", disk, 4096, UUID, "j713/nothere.bin").strip() == "err -5", "a missing file")
    check(run("lookup", disk, 4096, UUID, "j713").strip() == "err -5", "a directory is not a file")
    check(run("find", disk, 4096, "00000000-0000-4000-8000-000000000000").strip() == "err -3", "no such partition")

    changed = bytearray(trial)
    changed[len(changed) // 2] ^= 1
    scenario(aligned, "trial changed after arming",
             {"M1N1/BOOT-TRIAL.BIN": bytes(changed), "j713/bootstate.bin": armed},
             {"boot": "own", "reason": "trial-mismatch", "write": "0"})
    scenario(aligned, "trial of another size",
             {"M1N1/BOOT-TRIAL.BIN": trial[:-1], "j713/bootstate.bin": armed},
             {"boot": "own", "reason": "trial-mismatch", "write": "0"})
    scenario(aligned, "no trial file", {"j713/bootstate.bin": armed},
             {"boot": "own", "reason": "trial-missing", "write": "0", "failed_at": "none"})
    scenario(aligned, "idle record", {"M1N1/BOOT-TRIAL.BIN": trial, "j713/bootstate.bin": bs.encode(bs.idle())},
             {"boot": "own", "reason": "idle", "write": "0"})
    scenario(aligned, "no record", {"M1N1/BOOT-TRIAL.BIN": trial},
             {"boot": "own", "reason": "no-record", "write": "0", "failed_at": "none"})
    spent = bs.encode(bs.Record(seq=41, state=bs.ARMED, tries_left=0, tries_set=1, trial_size=len(trial),
                                trial_sha256=hashlib.sha256(trial).digest()))
    scenario(aligned, "the trial never reported back", {"M1N1/BOOT-TRIAL.BIN": trial, "j713/bootstate.bin": spent},
             {"boot": "own", "reason": "trial-failed", "show": "2", "write": "0"})
    bad = bytearray(armed)
    bad[100] ^= 4
    scenario(aligned, "damaged record", {"M1N1/BOOT-TRIAL.BIN": trial, "j713/bootstate.bin": bytes(bad)},
             {"boot": "own", "reason": "bad-record", "parse": "4", "write": "0"})
    scenario(aligned, "record longer than 512 bytes",
             {"M1N1/BOOT-TRIAL.BIN": trial, "j713/bootstate.bin": armed + b"\0"},
             {"boot": "own", "reason": "bad-record", "write": "0", "failed_at": "read"})
    files = {"M1N1/BOOT-TRIAL.BIN": trial, "j713/bootstate.bin": armed}
    scenario(aligned, "the ESP's UUID is not on the disk", files,
             {"boot": "own", "reason": "no-record", "failed_at": "gpt", "err": "-3"},
             disk_uuid="11111111-2222-4333-8444-555555555555")
    scenario(aligned, "GPT header CRC damaged", files,
             {"boot": "own", "reason": "no-record", "failed_at": "gpt", "err": "-2"}, damage_crc=True)

    # macOS's default FAT32 layout: data area off a 4 KiB boundary, so no block can be written
    unaligned = Volume("unaligned", "300m", "-c 8")
    scenario(unaligned, "FAT not aligned to 4096-byte blocks", files,
             {"boot": "own", "reason": "sector-mismatch", "failed_at": "locate", "write": "0"})
    # the same volume on a disk of 512-byte blocks: the record's block is the record's own sector
    scenario(unaligned, "512-byte blocks", files, {"boot": "trial", "reason": "trial", "write": "1"},
             block=512, first=2048)

    # a GPT that macOS wrote itself (512-byte blocks), found by the partition's unique UUID
    mac = os.path.join(TMP, "mac.dmg")
    sh("hdiutil", "create", "-quiet", "-size", "120m", "-fs", "MS-DOS FAT32", "-volname", "MACGPT",
       "-layout", "GPTSPUD", mac)
    sh("hdiutil", "convert", "-quiet", mac, "-format", "UDTO", "-o", os.path.join(TMP, "mac"))
    raw = open(os.path.join(TMP, "mac.cdr"), "rb").read()
    hdr = raw[512:512 + 92]
    n, esize, lba = struct.unpack_from("<I", hdr, 80)[0], struct.unpack_from("<I", hdr, 84)[0], \
        struct.unpack_from("<Q", hdr, 72)[0]
    parts = []
    for i in range(n):
        e = raw[lba * 512 + i * esize: lba * 512 + (i + 1) * esize]
        if any(e[:16]):
            parts.append((str(uuidlib.UUID(bytes_le=e[16:32])), *struct.unpack_from("<QQ", e, 32)))
    check(len(parts) >= 1, "macOS wrote a partition")
    for u, lo, hi in parts:
        got = run("find", os.path.join(TMP, "mac.cdr"), 512, u.upper()).split()
        check(got == [str(lo), str(hi - lo + 1)], f"macOS GPT: {u} at {lo}..{hi}, found {got}")
    tmp_ok = True
finally:
    subprocess.run(["rm", "-rf", TMP], check=False)

print(f"esp fixture: {checks} checks, {failures} failed")
sys.exit(1 if failures or not tmp_ok else 0)
