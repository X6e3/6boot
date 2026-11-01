#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read this boot's EL2 evidence into a NEW folder on an already mounted X63-R.

Linux, Python 3 standard library. No mount/install/ESP/reboot/network operations.
Only the explicitly named RAM MTD log may be read; no flash/block device reads.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import struct
import subprocess
import uuid

MODES={'preflight':1,'brk':2,'guest':3}
SYSTEMS=['VBAR_EL2','HCR_EL2','ELR_EL2','SPSR_EL2','ESR_EL2','FAR_EL2','SP_EL0','SP_EL1',
         'SCTLR_EL12','VBAR_EL12','ELR_EL12','SPSR_EL12','PAR_EL1','DAIF','SPSel','PAN',
         'SCTLR_EL2','TCR_EL2','TTBR0_EL2','TTBR1_EL2','MAIR_EL2','MPIDR_EL1','CurrentEL','private_SP']

def native_layout(n):
    if le(n,0,4)!=0x45324e41: raise ValueError('native magic')
    version,size=le(n,4,2),le(n,6,2)
    if (version,size,len(n))==(1,592,592): return SYSTEMS,256,448,544
    if (version,size,len(n))==(2,624,624): return SYSTEMS+['ESR_EL12','FAR_EL12'],272,480,576
    raise ValueError('unsupported native version/size')

def le(b,o,n=8):
    if o<0 or o+n>len(b): raise ValueError('truncated record at '+str(o))
    return int.from_bytes(b[o:o+n],'little')

def text(b): return b.rstrip(b'\0').decode('utf-8','strict')
def contains(a,b): return a[1]>0 and b[1]>0 and a[0]<=b[0] and b[0]+b[1]<=a[0]+a[1]<=2**64-1
def overlap(a,b): return a[1]>0 and b[1]>0 and a[0]<b[0]+b[1] and b[0]<a[0]+a[1]

def evaluate(props,manifest,release,cmdline):
    """Independent, strict decoder. Returns diagnostics even on truncated input."""
    report={'verdict':'FAIL','checks':[],'errors':[],'frames':[]}
    def check(ok,name):
        report['checks'].append({'check':name,'pass':bool(ok)})
        if not ok: report['errors'].append(name)
    def prop(name): return props['6boot,el2-'+name]
    def be(name,n=4):
        b=prop(name);check(len(b)==n,'DT size '+name);return int.from_bytes(b,'big')
    try:
        mode=text(prop('mode'));report['mode']=mode
        check(mode in MODES,'known experiment mode')
        check(all(v in props['compatible'].split(b'\0') for v in [b'apple,j713',b'apple,t8132']),'live DT identifies J713/T8132')
        check(text(prop('build-id'))==manifest['build_id'],'build ID matches bundle')
        check(text(prop('module-sha256'))==manifest['module_sha256'],'module SHA matches bundle')
        check(release.strip()=='7.1.13-x6e3-rel3','REL3 kernel release')
        wanted=manifest['images'][mode]['cmdline'].split()
        check(all(t in cmdline.split() for t in wanted),'all REL3 and experiment bootargs present')
        check(sum(t.startswith('x6e3.el2=') for t in cmdline.split())==1,'unambiguous boot mode')
        called=be('called');status=be('adapter-status');safe=be('safe');ret=be('return');reason=be('refusal-reason')
        report.update(called=called,adapter_status=status,safe=safe,return_code=ret,refusal_reason=reason)
        if called==0:
            report['verdict']='REFUSED';report['errors'].append('module was not called');return report
        check(called==1 and status==0 and safe==1 and reason==0,'adapter returned PASS and safe')
        arena=be('load-address',8)
        check(arena>0 and arena%65536==0,'arena 64 KiB alignment')
        n=prop('native-state')
        names,after_off,gpr_off,tail=native_layout(n)
        report['native_version']=le(n,4,2)
        check(le(n,8,4)==MODES[mode] and le(n,12,4)==status and le(n,16,4)==called,'native record agrees with DT')
        check(le(n,24)==31 and le(n,32)==ret and le(n,tail+16)==ret and le(n,tail+32)==31,'all outer return checks passed')
        before=[le(n,64+8*i) for i in range(len(names))];after=[le(n,after_off+8*i) for i in range(len(names))]
        report['systems']={name:{'before':hex(before[i]),'after':hex(after[i])} for i,name in enumerate(names)}
        check(before==after,str(len(names))+' caller context values preserved')
        check(before[22]==8 and before[14]==1 and before[13]&0x3c0==0x3c0,'EL2h and masked module entry')
        check([le(n,gpr_off+8*i) for i in range(12)]==list(range(0x712,0x71e)),'x18..x29 caller preservation')
        check(le(n,tail)==le(n,tail+8)==before[23] and le(n,tail)%16==0,'caller SP restored and aligned')
        check(le(n,tail+24)%16==0,'original calling frame aligned')
        o=prop('ownership')
        check(len(o)==176 and le(o,0,4)==0x45324f57 and le(o,4,2)==1 and le(o,6,2)==176 and le(o,8,4)==10,'ownership E2OW record')
        spans=[(le(o,16+16*i),le(o,24+16*i)) for i in range(10)]
        loader,bss,storage,area,stack,request,result,state,kernel,ctrr=spans
        check(state[1]==len(n)-64,'observer span matches native ABI')
        check(area==(arena,131072) and contains(storage,area) and contains(bss,storage) and contains(loader,bss),'arena belongs to loader BSS')
        check(stack[1]==16384 and contains(bss,stack) and not overlap(area,stack),'separate 16 KiB caller stack')
        check(le(n,tail+8)==stack[0]+stack[1],'native private stack top')
        check(all(contains(bss,r) and not overlap(area,r) for r in [request,result,state]),'observer buffers outside module arena')
        check(not overlap(area,kernel) and kernel[1]>0,'kernel disjoint from arena')
        report['ownership']={name:{'start':hex(r[0]),'size':hex(r[1])} for name,r in zip(
            ['loader','bss','storage','arena','caller_stack','request','result','observer','kernel','ctrr'],spans)}
        m=prop('mappings')
        check(len(m)==1056 and le(m,0,4)==0x45324d50 and le(m,4,2)==1 and le(m,6,2)==1056,'mapping E2MP record')
        check(le(m,8,4)==15 and le(m,12,4)==le(m,16,4)==le(m,20,4)==32 and le(m,24)==arena,'all 32 subpages checked before and after')
        for i in range(32):
            a=arena+4096*i
            pr,pw,ar,aw=[le(m,32+32*i+8*j) for j in range(4)]
            good=lambda p: not p&1 and ((p>>12)&0xffffffffff)==a>>12
            check(good(pr) and good(pw) and good(ar) and pr>>56==ar>>56==255 and
                  (bool(aw&1) if i<4 else good(aw)),'identity Normal-WB + code write protection subpage '+str(i))
        q=prop('request')
        check(le(q,0,4)==0x45324354 and le(q,4,2)==1 and le(q,6,2)==80 and le(q,8,4)==len(q),'request E2CT ABI')
        count=le(q,48,4);excluded=le(q,56,4)
        check(1<=count<=16 and excluded==2 and len(q)==80+16*(count+excluded),'request range counts')
        check(le(q,16,4)==0x8132 and q[20:24]==b'J713' and le(q,24,4)==3 and le(q,32)==arena and le(q,40)==131072,'request board and private arena')
        check(any(contains((le(q,80+16*i),le(q,88+16*i)),loader) for i in range(min(count,16))),'declared RAM covers loader')
        ex=80+16*count
        check((le(q,ex),le(q,ex+8))==(0,arena) and (le(q,ex+16),le(q,ex+24))==
              (arena+131072,2**64-1-arena-131072),'request excludes every address outside arena')
        r=prop('result')
        if mode=='preflight':
            pair=(le(r,4,2),le(r,6,2),len(r))
            check(le(r,0,4)==0x45325253 and pair in [(1,128,128),(2,160,160)],'preflight E2RS version/size')
            if pair==(2,160,160):
                report['feature_registers']={name:hex(le(r,128+8*i)) for i,name in enumerate(
                    ['ID_AA64MMFR0_EL1','ID_AA64MMFR1_EL1','ID_AA64MMFR2_EL1','ID_AA64PFR0_EL1'])}
            else: report['feature_registers']=None # not captured by the historical protocol

            check(ret==le(r,8,4)==0 and le(r,12,4)==3 and le(r,16)==8 and le(r,24)==1,'preflight READY (no exception proof)')
            check(le(r,80)==arena and le(r,88)==131072,'preflight reports same arena')
        else:
            count=4 if mode=='guest' else 1
            version=le(r,4,2)
            check(len(r)==2048 and le(r,0,4)==0x45324558 and version in (2,3) and le(r,6,2)==2048,'exception E2EX version/size')
            header=272 if version==3 else 256
            if mode=='guest' and version==3 and len(names)==26:
                check(le(r,256)==before[24] and le(r,264)==before[25],'saved EL1 fault banks agree with observer')

            check(ret==(22 if mode=='guest' else 13) and le(r,8,4)==13 and le(r,12,4)==(31 if mode=='guest' else 7),'separate BRK/guest statuses')
            check(le(r,16,4)==count and le(r,20,4)==384 and le(r,24)==0 and le(r,32)==1023,'bounded trace and full restore mask')
            check(le(r,160)==le(r,168)==0,'handler inactive with zero depth')
            pairs={40:0,64:1,72:2,80:3,88:4,96:5,104:6,176:7,56:13}
            check(all(le(r,o)==before[i] for o,i in pairs.items()),'core saved context agrees with outer observer')
            if mode=='guest':
                check(le(r,184)==1 and le(r,192)==22 and le(r,200)==0 and le(r,208)==3 and le(r,248)==1,'guest DONE, verdict zero, three HVCs')
                check(le(r,224)==before[1]&~0x28000039 and le(r,240)==before[8],'guest HCR profile and saved SCTLR')
            sym=manifest['trace_symbols']
            pcs=[arena+sym[k] for k in ['e2_expected_brk','e2_guest_after_probe','e2_guest_after_signature','e2_guest_after_done']]
            for i in range(count):
                f=r[header+384*i:header+384*(i+1)];event=[13,21,28,22][i]
                esr=[0xf2006e32,0x5a006e33,0x5a006e37,0x5a006e34][i]
                regs=[(0x100 if i==0 else 0x200)+j for j in range(31)]
                if i==2: regs[0]=0x5a5
                if i==3: regs[0]=0
                check([le(f,8*j) for j in range(31)]==regs,'trace GPR pattern event '+str(i))
                check(le(f,256)==esr and le(f,264)==pcs[i] and le(f,336)==event and le(f,320)==(4 if i==0 else 8) and le(f,328)==1 and le(f,368)==1,'syndrome/PC/vector event '+str(i))
                check(le(f,272)&0x3df==(0x3c9 if i==0 else 0x3c5),'exception from masked correct EL event '+str(i))
                resume=pcs[i]+4 if i==0 else arena+sym['e2_guest_return'] if i==3 else pcs[i]
                check(le(f,344)==resume and le(f,352)==(le(r,120) if i==3 else le(f,272)),'architectural exception return event '+str(i))
                check(arena+0x8000<=le(f,248)<=arena+0xc000 and le(f,248)%16==0,'bounded EL2 handler SP event '+str(i))
                report['frames'].append({'event':event,'esr':hex(le(f,256)),'elr':hex(le(f,264)),'spsr':hex(le(f,272)),
                    'resume_elr':hex(le(f,344)),'far':hex(le(f,280)),'far_valid':False})
        p3=props['6boot,p3-result']
        check(len(p3)==64 and le(p3,0,4)==0x58505242 and le(p3,12,4)==0,'legacy P3 result present and successful')
        report['verdict']='PASS' if not report['errors'] else 'FAIL'
    except (KeyError,ValueError,IndexError,TypeError,OverflowError) as e:
        report['errors'].append('missing/invalid evidence: '+str(e))
    return report

def bounded(path,limit=1024*1024):
    with Path(path).open('rb') as f: data=f.read(limit+1)
    if len(data)>limit: raise ValueError(str(path)+' exceeds capture limit')
    return data

def destination_root(explicit=None):
    """Find an existing mounted X63-R. No fallback to a same-named local folder."""
    raw=subprocess.check_output(['lsblk','--json','--output','NAME,LABEL,FSTYPE,MOUNTPOINT'],timeout=10)
    def walk(nodes):
        for n in nodes:
            yield n
            yield from walk(n.get('children',[]))
    matches=[]
    for n in walk(json.loads(raw)['blockdevices']):
        if n.get('label')=='X63-R' and n.get('mountpoint') and n.get('fstype') in ('exfat','fuseblk'):
            mount=Path(n['mountpoint']).resolve()
            found=json.loads(subprocess.check_output(['findmnt','--json','--target',str(mount),'--output','TARGET,FSTYPE'],timeout=10))['filesystems'][0]
            if Path(found['target']).resolve()==mount and found['fstype'] in ('exfat','fuseblk'): matches.append(mount)
    if explicit:
        wanted=Path(explicit).resolve()
        matches=[m for m in matches if m==wanted]
    if len(matches)!=1: raise ValueError('Mount X63-R yourself first, then pass its mount root with --destination. No filesystem was mounted or written.')
    return matches[0]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--destination',help='existing mounted X63-R root (optional auto discovery)')
    p.add_argument('--manifest',type=Path,default=Path(__file__).resolve().with_name('manifest.json'))
    a=p.parse_args()
    if platform.system()!='Linux': p.error('run on J713 Linux after the experimental boot')
    if os.geteuid()!=0: p.error('run with sudo to capture dmesg and the RAM stage2 log')
    manifest=json.loads(a.manifest.read_text())
    mount=destination_root(a.destination)
    bootid=bounded('/proc/sys/kernel/random/boot_id',128).decode().strip()
    stamp=datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    out=mount/'j713-diag'/'el2-native'/(stamp+'-'+bootid[:8]+'-'+uuid.uuid4().hex[:6])
    out.mkdir(parents=True,exist_ok=False);errors=[]
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    def capture(src,name,limit=1024*1024):
        try:
            data=bounded(src,limit);(out/name).write_bytes(data);return data
        except (OSError,ValueError) as e:
            errors.append(str(e));return b''
    chosen=Path('/sys/firmware/devicetree/base/chosen')
    if not chosen.is_dir(): chosen=Path('/proc/device-tree/chosen')
    props={};(out/'chosen').mkdir()
    if chosen.is_dir():
        for f in sorted(chosen.iterdir()):
            if f.is_file() and (f.name.startswith('6boot,') or f.name in ['bootargs','linux,initrd-start','linux,initrd-end']):
                props[f.name]=capture(f,'chosen/'+f.name)
    else: errors.append('No chosen device-tree node')
    cmdline=capture('/proc/cmdline','cmdline.txt').decode(errors='replace')
    release=platform.release();(out/'uname.txt').write_text(' '.join(platform.uname())+'\n')
    (out/'kernel-release.txt').write_text(release+'\n');(out/'boot-id.txt').write_text(bootid+'\n')
    for src,name in [('/proc/version','version.txt'),('/proc/cpuinfo','cpuinfo.txt'),('/proc/meminfo','meminfo.txt'),('/proc/iomem','iomem.txt'),
                     ('/sys/firmware/devicetree/base/compatible','compatible.bin'),('/sys/firmware/fdt','firmware-fdt.bin')]:
        data=capture(src,name,2*1024*1024)
        if name=='compatible.bin': props['compatible']=data
    try:
        r=subprocess.run(['dmesg','--color=never'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=15)
        (out/'dmesg.txt').write_bytes(r.stdout[:8*1024*1024]);(out/'dmesg-stderr.txt').write_bytes(r.stderr[:65536])
        if r.returncode or len(r.stdout)>8*1024*1024: errors.append('dmesg failed or truncated')
    except (OSError,subprocess.TimeoutExpired) as e: errors.append(str(e))
    captured_mtd=[]
    for m in sorted(Path('/sys/class/mtd').glob('mtd*')):
        if not re.fullmatch('mtd[0-9]+',m.name): continue
        try:
            if text(bounded(m/'name',256)).strip()!='m1n1_stage2.log': continue
            if text(bounded(m/'type',256)).strip()!='ram':
                errors.append('named stage2 log is not RAM: refused read');continue
            size=int(bounded(m/'size',128))
            if not 0<size<=2*1024*1024: raise ValueError('RAM log size refused')
            raw=capture('/dev/'+m.name,'stage2-'+m.name+'.bin',size)
            if raw:
                readable='\n'.join(s.decode('ascii',errors='replace') for s in re.findall(rb'[\x09\x20-\x7e]{4,}',raw))
                (out/('stage2-'+m.name+'.txt')).write_text(readable+'\n');captured_mtd.append(m.name)
        except (OSError,ValueError) as e: errors.append(str(e))
    result=evaluate(props,manifest,release,cmdline)
    result.update(capture_errors=errors,ram_logs=captured_mtd,boot_id=bootid,physical_verification='Evidence captured by user-run Linux collector; no emulator result is substituted.')
    (out/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
    lines=['Verdict: '+result['verdict'],'Boot ID: '+bootid,'Mode: '+result.get('mode','missing'),
           'Return: '+str(result.get('return_code','missing')),'Errors:']+result['errors']+['Capture errors:']+errors
    (out/'summary.txt').write_text('\n'.join(lines)+'\n')
    sums=[]
    for f in sorted(out.rglob('*')):
        if f.is_file(): sums.append(hashlib.sha256(f.read_bytes()).hexdigest()+'  '+str(f.relative_to(out)))
    (out/'SHA256SUMS').write_text('\n'.join(sums)+'\n')
    fd=os.open(out/'SHA256SUMS',os.O_RDONLY)
    try: os.fsync(fd)
    finally: os.close(fd)
    os.sync()
    print(result['verdict']+': '+str(out))
    if not captured_mtd: print('RAM stage2 log unavailable; raw DT records and Linux logs were still captured.')
    return 0 if result['verdict']=='PASS' and not errors else 2

if __name__=='__main__':
    try: raise SystemExit(main())
    except (OSError,ValueError,subprocess.SubprocessError) as e: raise SystemExit('Capture stopped: '+str(e))
