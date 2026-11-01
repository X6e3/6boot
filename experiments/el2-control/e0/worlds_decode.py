#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Independent E0W1 wire policy. Never accepts a successful boot as evidence."""
import argparse
import json
from pathlib import Path
import struct

HEADER = 'magic version size build mode status host_restored tlbis count refusal mmfr0 mmfr1 mmfr2 pfr0 hcr observer_ok'.split()
DOMAIN = 'vmid root vtcr code stack data ro signature completion context_ok observed guard_observed'.split()
EVENT = 'world kind vmid root hcr vtcr esr elr spsr far hpfar ipa validity expected context_ok tlbis'.split()

def decode(blob, expect):
    def check(ok, why):
        if not ok: raise ValueError(why)
    def fields(names, offset): return dict(zip(names, struct.unpack_from('<'+'Q'*len(names), blob, offset)))
    check(len(blob) == 1856, 'exact E0W1 size')
    h = fields(HEADER, 0)
    check((h['magic'], h['version'], h['size']) == (0x31573045, 1, 1856), 'E0W1 header')
    check(h['build'] == int(expect['build_id'], 16), 'source build identity')
    check(h['mode'] in (5, 6, 7), 'known mode')
    check(h['status'] == h['refusal'] == 0 and h['host_restored'] == h['observer_ok'] == 1, 'successful full host restoration')
    check(h['hcr'] & ((1 << 34) | (1 << 31) | (1 << 27)) == (1 << 34) | (1 << 31) | (1 << 27), 'VHE A64 host')
    check(not h['hcr'] & ((1 << 46) | (1 << 45) | (1 << 43) | (1 << 42) | (1 << 12) | 1), 'no VM/FWB/DC/NV host')
    check((h['mmfr1'] >> 4) & 15 in (0, 2) and (h['mmfr1'] >> 8) & 15 == 1, 'VMID/VHE features')
    check((h['mmfr2'] >> 24) & 15 <= 2, 'known NV feature encoding')
    check((h['pfr0'] >> 4) & 15 in (1, 2) and (h['pfr0'] >> 8) & 15 in (1, 2), 'A64 EL1/EL2')
    g1, g2, ps = (h['mmfr0'] >> 20) & 15, (h['mmfr0'] >> 32) & 15, h['mmfr0'] & 15
    check(ps <= 6 and (g2 in (2, 3) or (g2 == 0 and g1 in (1, 2))), '16 KiB Stage2 and PARange')
    vtcr = 0x8000b560 | min(ps, 5) << 16
    domains = [fields(DOMAIN, 128+96*w) for w in range(2)]
    base = domains[0]['root']-0x10000
    check(base == expect.get('arena', base), 'arena matches trusted build metadata')
    check(base > 0 and base % 65536 == 0 and base+0x40000 <= 1 << (32, 36, 40, 42, 44, 48)[min(ps, 5)], 'arena PA geometry')
    for w, d in enumerate(domains):
        offsets = (0x10000, 0x4000, 0x18000, 0x1c000, 0x20000) if w == 0 else (0x24000, 0x38000, 0x2c000, 0x30000, 0x34000)
        check([d[k] for k in ('root', 'code', 'stack', 'data', 'ro')] == [base+x for x in offsets], 'fixed nonoverlapping physical layout')
        check(d['vmid'] == 0x7e+w and d['vtcr'] == vtcr and d['signature'] == 0xa00+w*0x100, 'world config')
        ran = h['mode'] != 5 and (w == 0 or h['mode'] == 7)
        check(d['completion'] == d['context_ok'] == int(ran), 'world completion/context')
        check(d['observed'] == (d['signature'] if ran else 0) and d['guard_observed'] == 0x1357, 'private RAM and read-only canary')
    sequence = [] if h['mode'] == 5 else ([(0, 1), (1, 1), (1, 2), (1, 3), (1, 4), (0, 2), (0, 3), (0, 4)] if h['mode'] == 7 else [(0, 1), (0, 2), (0, 3), (0, 4)])
    check(h['count'] == len(sequence), 'exact exception sequence length')
    check(h['tlbis'] == {5: 0, 6: 3, 7: 7}[h['mode']], 'selection TLBI lifecycle')
    events = []
    for i, (w, kind) in enumerate(sequence):
        e = fields(EVENT, 320+128*i); d = domains[w]
        check((e['world'], e['kind']) == (w, kind), 'ordered world events')
        check(e['vmid'] == d['vmid'] and e['root'] == (d['vmid'] << 48 | d['root']) and e['vtcr'] == d['vtcr'], 'active translation domain')
        check(e['hcr'] & ((1 << 34) | (1 << 31) | 1) == (1 << 34) | (1 << 31) | 1 and not e['hcr'] & ((1 << 27) | (1 << 12) | (1 << 46)), 'VM enabled, guest TGE/DC/FWB clear')
        check(e['spsr'] & 0x1003df == 0x3c5 and e['expected'] == 1, 'EL1h and expected event')
        check(e['elr'] == expect['pcs'][{1:'yield', 2:'permission', 3:'foreign', 4:'done'}[kind]], 'exact payload PC')
        check(e['tlbis'] == (1 if i == 0 else (3 if h['mode'] == 6 or i == 1 else (5 if w == 1 else 7))), 'event TLBI generation')
        if kind in (1, 4):
            check(e['esr'] == (0x16 << 26 | 1 << 25 | (0x6e38 if kind == 1 else 0x6e39)), 'exact HVC syndrome')
            check(e['context_ok'] == 1 and e['validity'] == e['ipa'] == 0, 'HVC context and invalid fault addresses')
        else:
            address = 0x1000c000 if kind == 2 else domains[1-w]['data']
            check(e['esr'] >> 26 == 0x24 and e['esr'] & (1 << 25) and e['esr'] & 0x7ff == (0x4f if kind == 2 else 0x46), 'precise Stage2 fault class')
            check(e['far'] == e['ipa'] == address and e['validity'] == (5 if kind == 2 else 7), 'fault address validity')
            if kind == 3:
                check(((e['hpfar'] & 0xfffffffff0) << 8 | e['far'] & 0xfff) == address, 'HPFAR FIPA')
        events.append(e)
    check(not any(blob[320+128*len(sequence):]), 'unused events zero')
    return {'verdict': 'PASS', 'evidence': 'EMULATOR unless independently collected on J713', 'header': h, 'domains': domains, 'events': events}

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('record', type=Path); p.add_argument('--expect', type=Path, required=True)
    a = p.parse_args()
    try: print(json.dumps(decode(a.record.read_bytes(), json.loads(a.expect.read_text())), indent=2))
    except (ValueError, KeyError, struct.error) as e: raise SystemExit('FAIL: '+str(e))
