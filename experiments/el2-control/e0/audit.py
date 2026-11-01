#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the own E0 ELF layout and specific VHE/TLBI entry/exit sequences."""
import argparse
from pathlib import Path
import struct
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, required=True)
p.add_argument('--nm', default='aarch64-elf-nm')
a = p.parse_args()
elf = a.build/'e0.elf'
data = elf.read_bytes()
symbols = {}
for line in subprocess.check_output([a.nm, '-n', str(elf)], text=True).splitlines():
    fields = line.split()
    if len(fields) == 3: symbols[fields[2]] = int(fields[0], 16)
phoff, = struct.unpack_from('<Q', data, 32)
phsize, phnum = struct.unpack_from('<HH', data, 54)

def segments(blob):
    return [struct.unpack_from('<IIQQQQQQ', blob, phoff+i*phsize) for i in range(phnum)]

def offset(address):
    for kind, _, off, va, _, filesz, _, _ in segments(data):
        if kind == 1 and va <= address < va+filesz: return off+address-va
    raise ValueError('address not in file-backed PT_LOAD')

def check(blob):
    for kind, flags, *_ in segments(blob):
        assert kind != 1 or (flags & 3) != 3, 'W+X load segment'
    base = symbols['e0_arena_start']
    assert symbols['e2_vectors'] % 0x800 == 0
    assert symbols['e2_vectors_end']-symbols['e2_vectors'] == 0x800
    assert symbols['e0_guest_vectors'] == base+0x4000
    assert symbols['__e2_stack_start'] == base+0xc000
    assert symbols['__e2_stack_end'] == symbols['e0_tables'] == base+0x10000
    # AND clears TGE; then MSR HCR and ISB. TLBI must use the guest VMID
    # with TGE=0. Specific A64 encodings, not a search anywhere in the image.
    for name, words in (
        ('e0_tlbi_enter', (0x9264f94c,0xd51c110c,0xd5033fdf,0xd5033a9f,0xd50c83df,0xd5033b9f,0xd5033fdf)),
        ('e0_tlbi_exit', (0x9264f94a,0xd51c110a,0xd5033fdf,0xd5033b9f,0xd50c83df,0xd5033b9f,0xd5033fdf))):
        at = offset(symbols[name]-12)
        assert struct.unpack_from('<7I',blob,at) == words, name+' VHE/TLBI ordering'
    def word(at): return struct.unpack_from('<I',blob,offset(at))[0]
    def branch(at, conditional=False):
        w=word(at); bits,shift=(19,5) if conditional else (26,0)
        assert w & (0xff00001f if conditional else 0xfc000000) == (0x54000001 if conditional else 0x14000000)
        imm=(w >> shift)&((1<<bits)-1)
        if imm & (1<<(bits-1)): imm-=1<<bits
        return at+4*imm
    start=symbols['e0_restore_vtcr']
    for i in range(18):
        assert branch(start+24*i+20,True)==symbols['e0_restore_failed'], 'readback failure must park'
    # DAIF remains fully masked; inherited VBAR is the final control commit.
    assert word(start+24*16+4)==0xd51b422a, 'restore DAIF before VBAR'
    assert word(start+24*17+4)==0xd51cc00a, 'restore VBAR last'
    assert word(symbols['e0_restore_failed'])==0xd280006a, 'RESTORE_FAILED status'
    assert branch(symbols['e0_restore_failed']+4)==symbols['e0_terminal'], 'mismatch must not return'
    assert word(symbols['e2_exception_direct_abort'])==0xd280008a, 'CONTEXT_LOST status'
    assert symbols['e2_exception_direct_abort']+4==symbols['e0_terminal'], 'trap must park'
    terminal=symbols['e0_terminal']; park=symbols['e0_terminal_park']
    assert word(terminal)==0xd5034fdf and word(terminal+4)==0xd5033fdf, 'terminal DAIF mask/ISB'
    assert word(park)==0x14000000, 'terminal must not return'
    for at in range(terminal,park,4):
        w=word(at)
        assert w&0xfc000000!=0x94000000 and w&0xfe000000!=0xd6000000, 'no terminal call/return/ERET'
    assert word(symbols['e0_fault_pc'])==0xf9000023, 'fault matcher requires ordinary STR x3'
    for path in (a.build/'runtime.su', a.build/'stage2.su'):
        for line in path.read_text().splitlines():
            if 'e2_exception_dispatch' in line or 'e0_fault_valid' in line:
                _, size, kind = line.rsplit('\t',2)
                assert kind == 'static' and int(size) <= 512, 'handler stack usage'
    if 'ew_dispatch' in symbols:
        assert symbols['ew_tables_b'] == base+0x24000, 'second fixed table slot'
        assert word(symbols['ew_permission_pc']) == word(symbols['ew_foreign_pc']) == 0xf9000023, 'ordinary STR fault sites'
        # Re-decode the mutated bytes, not the original disassembly, for all
        # checks below. The surrounding words make ordering requirements local.
        start = symbols['select_world']
        end = min(v for v in symbols.values() if v > start)
        ops = [word(at) for at in range(start,end,4)]
        tlbis = [i for i,w in enumerate(ops) if w == 0xd50c83df]
        assert len(tlbis) == 2, 'old/new VMID invalidation'
        assert ops[tlbis[0]-1:tlbis[0]+3] == [0xd5033b9f,0xd50c83df,0xd5033b9f,0xd5033fdf], 'old VMID barriers'
        assert ops[tlbis[1]-1:tlbis[1]+3] == [0xd5033a9f,0xd50c83df,0xd5033b9f,0xd5033fdf], 'new VMID barriers'
        # Mask Rt only; VTCR/VTTBR must be committed together, followed by ISB.
        vttbr = next(i for i,w in enumerate(ops) if w & ~31 == 0xd51c2100)
        assert ops[vttbr-1] & ~31 == 0xd51c2140 and ops[vttbr+1] == 0xd5033fdf, 'table register commit'
        hcrs = [i for i,w in enumerate(ops) if w & ~31 == 0xd51c1100]
        assert len(hcrs) == 2 and hcrs[0] < tlbis[0] < vttbr < tlbis[1] < hcrs[1], 'VM off, flush, select, flush, VM on'
        usage = {}
        for path in (a.build/'worlds.su', a.build/'worlds_policy.su'):
            for line in path.read_text().splitlines():
                name, size, kind = line.rsplit('\t',2)
                assert kind == 'static' and int(size) <= 256, 'bounded world helper stack'
                usage[name.rsplit(':',1)[-1]] = int(size)
        for name in ('load_bank','ew_validate','ew_pair','ew_fault','save_bank'):
            assert usage[name] == 0, 'leaf world helper stack'
        assert usage['ew_dispatch'] + usage['select_world'] <= 1024, 'shared vector C stack budget'
        if 'fb_e0_frame' in symbols:
            for path in (a.build/'visual.su',a.build/'fb.su',a.build/'fb_memory.su'):
                for line in path.read_text().splitlines():
                    name,size,kind=line.rsplit('\t',2)
                    usage[name.rsplit(':',1)[-1]]=int(size)
                    assert kind=='static', 'no dynamic display stack'
            assert usage['ew_dispatch']+usage['ew_visual_event']+usage['fb_e0_frame']+usage['bt_text']+usage['bt_fill']<=1024, 'bounded visual handler chain'
            assert usage['dc_cvac_range']==0, 'cache helper stack'

check(data)
for name, delta in (('e0_tlbi_enter',-12),('e0_tlbi_enter',4),('e0_tlbi_exit',-12),('e0_tlbi_exit',4),('e0_terminal',0),('e0_terminal_park',0),('e0_restore_vtcr',20)):
    bad = bytearray(data)
    struct.pack_into('<I',bad,offset(symbols[name]+delta),0xd503201f) # NOP in own image copy
    try:
        check(bad)
    except AssertionError:
        continue
    raise SystemExit('FAIL: audit accepted a missing TGE clear/TLBI')
print('E0_AUDIT_PASS: own ELF geometry/segment flags, VHE/TLBI, handler stack; seven mutations rejected; terminal/restore ordering')
if 'ew_dispatch' in symbols:
    start = symbols['select_world']; end = min(v for v in symbols.values() if v > start)
    sites = [at for at in range(start,end,4) if struct.unpack_from('<I',data,offset(at))[0] == 0xd50c83df]
    sites += [symbols['ew_permission_pc'], symbols['ew_foreign_pc']]
    for at in sites:
        bad = bytearray(data); struct.pack_into('<I',bad,offset(at),0xd503201f)
        try: check(bad)
        except AssertionError: continue
        raise SystemExit('FAIL: world audit accepted missing TLBI/fault instruction')
    print('EW_AUDIT_PASS: shared vectors, fixed pool, world TLBI/register ordering, bounded stack; four world mutations rejected')
