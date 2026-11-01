#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reject tampered real emulator records, including false isolation claims."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__); p.add_argument('--records', type=Path, required=True)
p.add_argument('--c-validator', type=Path)
a = p.parse_args()
spec = importlib.util.spec_from_file_location('decoder', Path(__file__).with_name('worlds_decode.py'))
decoder = importlib.util.module_from_spec(spec); spec.loader.exec_module(decoder)
expect = json.loads((a.records/'expect.json').read_text())
records = sorted(a.records.glob('trace-*.bin')) or [a.records/'trace.bin']
for p in records: assert decoder.decode(p.read_bytes(), expect)['verdict'] == 'PASS'
blob = (a.records/('trace-2.bin' if len(records) > 1 else 'trace.bin')).read_bytes()
def c_check(data, wanted):
    if not a.c_validator: return
    with tempfile.TemporaryDirectory(prefix='e0-wire-check-') as d:
        f=Path(d)/'trace';f.write_bytes(data)
        arena=expect.get('arena',0x40800000)
        cmd=[str(a.c_validator),str(f),'0x'+expect['build_id'],hex(arena)]+[hex(expect['pcs'][k]) for k in ['yield','permission','foreign','done']]
        r=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        assert r.returncode==(0 if wanted else 1),(cmd,r.returncode,r.stderr.decode())
c_check(blob,True)
mutations = [(8, 2), (16, 1855), (24, 0), (32, 5), (40, 2), (48, 0), (56, 0), (64, 4), (72, 1),
             (80, 0), (88, 0x110), (96, 3 << 24), (104, 0), (112, 1), (120, 0)]
for w in range(2):
    off = 128+96*w
    mutations += [(off+8*i, 0) for i in range(12)]
for i in range(8):
    off = 320+128*i
    for field in ('world','kind','vmid','root','hcr','vtcr','esr','elr','spsr','expected','tlbis'):
        at = off+8*decoder.EVENT.index(field)
        original = struct.unpack_from('<Q',blob,at)[0]
        mutations.append((at, original ^ (1 << 10 if field == 'esr' else 1)))
    if i not in (0,1,4,7):
        for field in ('far','ipa','validity'):
            at = off+8*decoder.EVENT.index(field)
            mutations.append((at,struct.unpack_from('<Q',blob,at)[0]^1))
for at, value in mutations:
    bad = bytearray(blob); struct.pack_into('<Q',bad,at,value)
    try: decoder.decode(bad, expect)
    except ValueError: c_check(bad,False); continue
    raise AssertionError(('accepted false trace',at,value))
for bad in (blob[:-1],blob+b'\0'):
    try: decoder.decode(bad, expect)
    except ValueError: c_check(bad,False); continue
    raise AssertionError('accepted wrong size')
# Permission faults cannot gain or lose validity based on UNKNOWN HPFAR.
bad = bytearray(blob);struct.pack_into('<Q',bad,320+128*2+8*10,0xdeadbeefcafebabe)
assert decoder.decode(bad,expect)['verdict'] == 'PASS'
c_check(bad,True)
# Translation fault must reconstruct IPA from a valid HPFAR.
bad = bytearray(blob);at = 320+128*3+8*10
struct.pack_into('<Q',bad,at,struct.unpack_from('<Q',bad,at)[0]^0x10)
try: decoder.decode(bad,expect)
except ValueError: c_check(bad,False)
else: raise AssertionError('accepted corrupt FIPA')
print('EW_TRACE_NEGATIVE_PASS:',len(mutations)+3,'invalid records rejected; UNKNOWN permission HPFAR ignored')
