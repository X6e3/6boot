#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check native ELF placement, embedded bytes and opt-out control build."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True);p.add_argument('--nm',default='aarch64-elf-nm');a=p.parse_args()
b=a.build;manifest=json.loads((b/'source-manifest.json').read_text());source=b/'source'
def symbols(path):
    result={}
    for line in subprocess.check_output([a.nm,'-n','-S',str(path)],text=True).splitlines():
        parts=line.split()
        if len(parts)==4:result[parts[3]]=(int(parts[0],16),int(parts[1],16))
        elif len(parts)==3:
            try:result[parts[2]]=(int(parts[0],16),0)
            except ValueError:pass
    return result
s=symbols(source/'build/m1n1-raw.elf');control=symbols(b/'control-source/build/m1n1-raw.elf')
checks=[]
def check(ok,name):
    checks.append({'check':name,'pass':bool(ok)})
    if not ok:raise SystemExit('FAIL '+name)
def contains(a,r):return a[0]<=r[0] and r[0]+r[1]<=a[0]+a[1]
loader=(s['_base'][0],s['_end'][0]-s['_base'][0]);bss=(s['_bss_start'][0],s['_bss_end'][0]-s['_bss_start'][0])
check(contains(loader,bss),'BSS wholly in reserved loader')
storage=s['arena_storage'];stack=s['caller_stack']
check(storage[1]==0x30000 and storage[0]%0x4000==0 and contains(bss,storage),'owned alignment slack')
check(stack[1]==0x4000 and stack[0]%0x4000==0 and contains(bss,stack),'16 KiB native caller stack in BSS')
for residue in range(0,0x10000,0x4000):
    start=(storage[0]+residue+0xffff)&~0xffff;arena=(start,0x20000)
    check(contains((storage[0]+residue,storage[1]),arena),'arena fits for load residue '+hex(residue))
    for name in ['caller_stack','request','result','el2_native_call_state']:
        r=(s[name][0]+residue,s[name][1])
        check(not(start<r[0]+r[1] and r[0]<start+0x20000),'arena disjoint '+name+' residue '+hex(residue))
blob=(b/'module/el2-foundation.bin').read_bytes();stage=(b/'m1n1-el2-native.bin').read_bytes()
check(hashlib.sha256(blob).hexdigest()==manifest['module_sha256'],'embedded module source hash')
check(len(blob)<=0x4000 and stage.count(blob)==1 and s['el2_native_image'][1]==len(blob),'exactly one whole module fits RX page')
check(s['_end'][0]==len(stage),'bare stage2 size equals payload start')
check(manifest['build_id'].encode() in stage,'native build ID present')
for name,digest in manifest['source'].items():check(hashlib.sha256((source/name).read_bytes()).hexdigest()==digest,'source manifest '+name)
for name in ['el2_native_invoke','el2_native_run','en_protect_arena','en_fdt_memory','en_result']:
    check(name in s and name not in control,'explicit opt-in only '+name)
out={'checks':checks,'link_offsets':{n:{'address':hex(s[n][0]),'size':hex(s[n][1])} for n in ['_base','_end','_bss_start','_bss_end','arena_storage','caller_stack','request','result','el2_native_call_state','el2_native_image']},
     'note':'Offsets in relocatable ELF; actual physical load and ownership remain runtime gates.'}
(b/'native-audit.json').write_text(json.dumps(out,indent=2)+'\n')
print('PASS: native ELF placement/embedding/control,',len(checks),'checks')
