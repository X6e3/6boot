#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The trial-boot record of installer level 2, as src/xboot_bootstate.h describes it.

Linux arms a trial, disarms it, and promotes a trial that booted fully; 6boot only lowers
"tries left" by one before it boots the trial. This module is the Linux side's reference for the
file format. Standard library only, Python 3.9 or newer.
"""
import hashlib
import struct
import zlib
from dataclasses import dataclass

PATH = "j713/bootstate.bin"
TRIAL_PATH = "M1N1/BOOT-TRIAL.BIN"
SIZE = 512
MAGIC = b"XBST"
VERSION = 1
MAX_TRIES = 3
IDLE, ARMED = 0, 1
HASH_BYTES = 32

# The parse errors of xb_bs_parse(), with the same numbers.
E_SIZE, E_MAGIC, E_VERSION, E_CRC, E_FIELD = 1, 2, 3, 4, 5

_HEAD = struct.Struct("<4sHHIBBBBI32s")
assert _HEAD.size == 52


class RecordError(ValueError):
    def __init__(self, code, why):
        super().__init__(why)
        self.code = code


@dataclass(frozen=True)
class Record:
    seq: int = 0
    state: int = IDLE
    tries_left: int = 0
    tries_set: int = 0
    flags: int = 0
    trial_size: int = 0
    trial_sha256: bytes = bytes(HASH_BYTES)


def _fields_ok(r):
    if r.flags or len(r.trial_sha256) != HASH_BYTES:
        return False
    if r.state == IDLE:
        return not (r.tries_left or r.tries_set or r.trial_size or any(r.trial_sha256))
    if r.state == ARMED:
        return 1 <= r.tries_set <= MAX_TRIES and r.tries_left <= r.tries_set and r.trial_size > 0
    return False


def encode(r):
    if not _fields_ok(r) or not 0 <= r.seq < 2**32 or not 0 <= r.trial_size < 2**32:
        raise RecordError(E_FIELD, f"not a valid record: {r}")
    head = _HEAD.pack(MAGIC, VERSION, SIZE, r.seq, r.state, r.tries_left, r.tries_set, r.flags,
                      r.trial_size, r.trial_sha256)
    body = head + bytes(508 - len(head))
    return body + struct.pack("<I", zlib.crc32(body))


def decode(b):
    b = bytes(b)
    if len(b) != SIZE or struct.unpack_from("<H", b, 6)[0] != SIZE:
        raise RecordError(E_SIZE, f"{len(b)} bytes, or a size field that is not {SIZE}")
    magic, version, _, seq, state, left, tries, flags, size, sha = _HEAD.unpack_from(b)
    if magic != MAGIC:
        raise RecordError(E_MAGIC, f"magic {magic!r}")
    if version != VERSION:
        raise RecordError(E_VERSION, f"version {version}")
    if struct.unpack_from("<I", b, 508)[0] != zlib.crc32(b[:508]):
        raise RecordError(E_CRC, "CRC does not match")
    r = Record(seq, state, left, tries, flags, size, sha)
    if not _fields_ok(r) or any(b[52:508]):
        raise RecordError(E_FIELD, f"fields out of range or reserved bytes set: {r}")
    return r


def idle(prev=None):
    """The record after a disarm or a promotion: no trial."""
    return Record(seq=_next(prev))


def arm(trial_image, tries=1, prev=None):
    """A record that asks 6boot to boot trial_image (the bytes of M1N1/BOOT-TRIAL.BIN)."""
    if not 1 <= tries <= MAX_TRIES:
        raise RecordError(E_FIELD, f"tries must be 1 to {MAX_TRIES}")
    return Record(seq=_next(prev), state=ARMED, tries_left=tries, tries_set=tries,
                  trial_size=len(trial_image), trial_sha256=hashlib.sha256(trial_image).digest())


def _next(prev):
    return 0 if prev is None else (prev.seq + 1) % 2**32
