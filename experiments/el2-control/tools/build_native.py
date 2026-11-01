#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a separate native development stage2 in a fresh scratch directory.

Never installs, boots, mounts an ESP or modifies the input checkout.
Dependencies are supplied by the caller; no automatic toolchain installation.
"""
import argparse,hashlib,json,os,shutil,subprocess,shlex
from pathlib import Path

p=argparse.ArgumentParser()
p.add_argument('--work',type=Path,required=True)
p.add_argument('--make',default='gmake')
p.add_argument('--cc',default='clang --target=aarch64-none-elf')
p.add_argument('--ld',default='aarch64-elf-ld')
p.add_argument('--objcopy',default='aarch64-elf-objcopy')
p.add_argument('--nm',default='aarch64-elf-nm')
p.add_argument('--objdump',default='aarch64-elf-objdump')
p.add_argument('--control',action='store_true',help='also build an ordinary non-experimental stage2')
a=p.parse_args()
repo=Path(__file__).resolve().parents[3]
work=a.work.resolve()
if work==repo or repo in work.parents: p.error('scratch must be outside the source checkout')
work.mkdir(parents=True,exist_ok=False)
source=work/'source'
shutil.copytree(repo,source,ignore=shutil.ignore_patterns('.git','build','out','__pycache__'))
logs=work/'logs'; logs.mkdir()

def run(cmd,name,cwd=source,env=None):
    with (logs/name).open('w') as f:
        f.write(json.dumps(cmd)+'\n');f.flush()
        r=subprocess.run(cmd,cwd=cwd,env=env,stdout=f,stderr=subprocess.STDOUT)
    if r.returncode:
        tail=(logs/name).read_text(errors='replace').splitlines()[-12:]
        raise SystemExit('FAILED '+name+'\n'+'\n'.join(tail))

module=source/'experiments/el2-control'
cc=shlex.split(a.cc)
run([a.make,'BUILD='+str(work/'module'),'FW_CC='+cc[0],
     'FW_TARGET='+shlex.join(cc[1:]),'FW_LD='+a.ld,
     'OBJCOPY='+a.objcopy,'OBJDUMP='+a.objdump,'NM='+a.nm,'all'],'module.log',module)
probe=source/'entry-probe'
run([a.make,'CC=clang','BUILD='+str(work/'probe'),'test'],'p3-host.log',probe)
run([a.make,'CC='+a.cc,'LD='+a.ld,'BUILD='+str(work/'probe'),'firmware'],'p3-firmware.log',probe)
run([a.objcopy,'-O','binary','--only-section=.text','--only-section=.rodata',str(work/'probe/xboot-entry-probe.elf'),str(work/'probe.bin')],'probe-objcopy.log')
(source/'build').mkdir(exist_ok=True)
run(['python3','-I',str(source/'tools/embed_entry_probe.py'),str(work/'probe.bin'),str(source/'build/entry_probe_image.h')],'embed-p3.log')
shutil.copy2(source/'data/die_blocks.h',source/'build/die_blocks.h')
blob=(work/'module/el2-foundation.bin').read_bytes()
symbols={}
for line in subprocess.check_output([a.nm,'-n',str(work/'module/el2-foundation.elf')],text=True).splitlines():
    s=line.split()
    if len(s)==3: symbols[s[-1]]=int(s[0],16)
files={}
for d in ['experiments/el2-control/include','experiments/el2-control/arch','experiments/el2-control/core','experiments/el2-control/native']:
    for f in sorted((source/d).rglob('*')):
        if f.is_file():files[str(f.relative_to(source))]=hashlib.sha256(f.read_bytes()).hexdigest()
for n in ['Makefile','src/kboot.c','src/payload.c','src/entry_probe.c','src/el2_native.h','experiments/el2-control/module.ld']:
    files[n]=hashlib.sha256((source/n).read_bytes()).hexdigest()
digest=hashlib.sha256(blob).hexdigest()
build_id=hashlib.sha256(json.dumps({'source':files,'module':digest},sort_keys=True).encode()).hexdigest()[:16]
rows=[', '.join(f'0x{x:02x}' for x in blob[i:i+16]) for i in range(0,len(blob),16)]
header=['/* Generated experimental module; not used by ordinary builds. */',
        '#define EL2_NATIVE_BUILD_ID "'+build_id+'"','#define EL2_NATIVE_IMAGE_SHA "'+digest+'"',
        '#define EL2_NATIVE_IMAGE_BYTES '+str(len(blob))]
for key,sym in [('PREFLIGHT','sixboot_el2_entry'),('BRK','sixboot_el2_exception_entry'),('GUEST','sixboot_el2_guest_entry')]:
    header.append('#define EL2_NATIVE_'+key+' '+str(symbols[sym]))
header+=['static const unsigned char el2_native_image[] __attribute__((aligned(16))) = {',',\n'.join(rows),'};']
(source/'build/el2_native_image.h').write_text('\n'.join(header)+'\n')
manifest={'build_id':build_id,'module_sha256':digest,'module_bytes':len(blob),'source':files,'entry_offsets':{k:symbols[k] for k in ['sixboot_el2_entry','sixboot_el2_exception_entry','sixboot_el2_guest_entry']},'trace_symbols':{k:symbols[k] for k in ['e2_expected_brk','e2_guest_after_probe','e2_guest_after_signature','e2_guest_after_done','e2_guest_return']}}
(work/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
common=[a.make,'-j6','USE_CLANG=1','TOOLCHAIN=/usr/bin/','LLDDIR=/usr/bin/',
        'CC='+a.cc,'AS='+a.cc,'LD='+a.ld,'OBJCOPY='+a.objcopy,'CARGO_FLAGS=--locked','build/m1n1.bin']
env=os.environ.copy();env['M1N1_VERSION_TAG']='6boot-rel3-el2-'+build_id
run(common+['CFG=FB_SILENT_MODE SIXBOOT_USER SIXBOOT_EL2_NATIVE'],'native-stage2.log',env=env)
shutil.copy2(source/'build/m1n1.bin',work/'m1n1-el2-native.bin')
if a.control:
    control=work/'control-source'
    shutil.copytree(source,control,ignore=shutil.ignore_patterns('build'))
    (control/'build').mkdir()
    for n in ['entry_probe_image.h','die_blocks.h']:shutil.copy2(source/'build'/n,control/'build'/n)
    # Rust sources are unchanged; a separate cargo invocation records this build too.
    env['M1N1_VERSION_TAG']='6boot-rel3-control-'+build_id
    run(common+['CFG=FB_SILENT_MODE SIXBOOT_USER'],'control-stage2.log',control,env)
    shutil.copy2(control/'build/m1n1.bin',work/'m1n1-control.bin')
    syms=subprocess.check_output([a.nm,str(control/'build/m1n1-raw.elf')],text=True)
    if 'el2_native_invoke' in syms or 'el2_native_run' in syms: raise SystemExit('native code leaked into control build')
print(json.dumps({'work':str(work),'build_id':build_id,'stage2_sha256':hashlib.sha256((work/'m1n1-el2-native.bin').read_bytes()).hexdigest()}))
