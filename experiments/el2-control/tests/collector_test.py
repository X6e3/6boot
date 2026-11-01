#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Decoder tests using unmodified records exported by the QEMU native gate.

Ownership/mapping/DT/P3 metadata are synthetic. These fixtures do not assert
J713 memory ownership or a successful Linux capture on a physical device.
"""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
from unittest.mock import patch

p=argparse.ArgumentParser();p.add_argument('--evidence',type=Path,required=True);p.add_argument('--manifest',type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('collector',root/'tools/collect_native.py');c=importlib.util.module_from_spec(spec);spec.loader.exec_module(c)
manifest=json.loads(a.manifest.read_text());manifest['images']={m:{'cmdline':'root=test x6e3.el2='+m} for m in c.MODES}
checks=0
def check(ok):
    global checks
    checks+=1
    if not ok: raise AssertionError('collector check '+str(checks))
def put(b,o,v,n=8): b[o:o+n]=v.to_bytes(n,'little')

def fixture(e):
    mode=e['mode'];native=bytes.fromhex(e['native']);result=bytes.fromhex(e['result']);arena=0x40800000
    props={'compatible':b'apple,j713\0apple,t8132\0'}
    def prop(k,b):props['6boot,el2-'+k]=b
    def be(k,v,n=4):prop(k,v.to_bytes(n,'big'))
    prop('mode',mode.encode()+b'\0');prop('build-id',manifest['build_id'].encode()+b'\0');prop('module-sha256',manifest['module_sha256'].encode()+b'\0')
    for key,val in [('called',1),('adapter-status',0),('safe',1),('return',c.le(native,32)),('refusal-reason',0)]:be(key,val)
    be('load-address',arena,8);prop('native-state',native);prop('result',result)
    names,after_off,gpr_off,tail=c.native_layout(native)
    top=c.le(native,tail+8)
    # 592 is E2_REQUEST_MAX; observer is 528 (v1) or 560 (v2).
    spans=[(0x40400000,0x800000),(0x40400000,0x700000),(arena,0x30000),(arena,0x20000),(top-0x4000,0x4000),
           (0x40400100,592),(0x40400400,2048),(0x40400d00,len(native)-64),(0x41000000,0x100000),(0x40400000,0x800000)]
    o=bytearray(176);put(o,0,0x45324f57,4);put(o,4,1,2);put(o,6,176,2);put(o,8,10,4)
    for i,(start,size) in enumerate(spans):put(o,16+16*i,start);put(o,24+16*i,size)
    prop('ownership',bytes(o))
    m=bytearray(1056);put(m,0,0x45324d50,4);put(m,4,1,2);put(m,6,1056,2)
    for off,v in [(8,15),(12,32),(16,32),(20,32)]:put(m,off,v,4)
    put(m,24,arena)
    for i in range(32):
        par=(255<<56)+arena+4096*i
        for j,v in enumerate([par,par,par,1 if i<4 else par]):put(m,32+32*i+8*j,v)
    prop('mappings',bytes(m))
    q=bytearray(128)
    for off,v,n in [(0,0x45324354,4),(4,1,2),(6,80,2),(8,128,4),(16,0x8132,4),(24,3,4),(32,arena,8),(40,131072,8),(48,1,4),(56,2,4),
                    (80,0x40000000,8),(88,0x8000000,8),(96,0,8),(104,arena,8),(112,arena+131072,8),(120,2**64-1-arena-131072,8)]:put(q,off,v,n)
    q[20:24]=b'J713';prop('request',bytes(q));p3=bytearray(64);put(p3,0,0x58505242,4);props['6boot,p3-result']=bytes(p3)
    return props

for e in json.loads(a.evidence.read_text()):
    props=fixture(e);mode=e['mode'];cmd=manifest['images'][mode]['cmdline']
    def evaluate(p):return c.evaluate(p,manifest,'7.1.13-x6e3-rel3',cmd)
    r=evaluate(props)
    if r['verdict']!='PASS':raise AssertionError(mode+': '+json.dumps(r['errors']))
    check(r['verdict']=='PASS')
    bad=copy.deepcopy(props);b=bytearray(bad['6boot,el2-ownership'])
    put(b,16+16*7+8,1);bad['6boot,el2-ownership']=bytes(b)
    check(evaluate(bad)['verdict']=='FAIL')
    if mode=='preflight':
        data=props['6boot,el2-result']
        if c.le(data,4,2)==2:
            check(r['feature_registers']=={name:hex(c.le(data,128+8*i)) for i,name in enumerate(
                ['ID_AA64MMFR0_EL1','ID_AA64MMFR1_EL1','ID_AA64MMFR2_EL1','ID_AA64PFR0_EL1'])})
            for off,value in [(4,1),(6,128)]:
                bad=copy.deepcopy(props);b=bytearray(data);put(b,off,value,2)
                bad['6boot,el2-result']=bytes(b);check(evaluate(bad)['verdict']=='FAIL')

    for key,off in [('native-state',64),('native-state',c.native_layout(props['6boot,el2-native-state'])[2]),('native-state',c.native_layout(props['6boot,el2-native-state'])[3]+8),('ownership',64),('mappings',32),('result',8)]:
        bad=copy.deepcopy(props);b=bytearray(bad['6boot,el2-'+key]);b[off]^=1;bad['6boot,el2-'+key]=bytes(b)
        check(evaluate(bad)['verdict']!='PASS')
    for key in ['native-state','result','ownership','mappings','request']:
        for length in [0,3,7,31]:
            bad=copy.deepcopy(props);bad['6boot,el2-'+key]=bad['6boot,el2-'+key][:length];check(evaluate(bad)['verdict']!='PASS')
    bad=copy.deepcopy(props);bad['6boot,el2-build-id']=b'wrong';check(evaluate(bad)['verdict']!='PASS')
    bad=copy.deepcopy(props);bad['6boot,el2-called']=bytes(4);check(evaluate(bad)['verdict']=='REFUSED')
    check(c.evaluate(props,manifest,'wrong',cmd)['verdict']!='PASS')
    check(c.evaluate(props,manifest,'7.1.13-x6e3-rel3',cmd+' x6e3.el2=wrong')['verdict']!='PASS')
    if mode!='preflight':
        header=272 if c.le(props['6boot,el2-result'],4,2)==3 else 256
        for off in [header,header+256,header+264,header+272,header+320,header+336,header+344,header+368]:
            bad=copy.deepcopy(props);b=bytearray(bad['6boot,el2-result']);b[off]^=1;bad['6boot,el2-result']=bytes(b);check(evaluate(bad)['verdict']!='PASS')

with tempfile.TemporaryDirectory() as d:
    volume=(Path(d)/'mounted-volume').resolve();volume.mkdir()
    def output(cmd,**kw):
        if cmd[0]=='lsblk':return json.dumps({'blockdevices':[{'name':'sda1','label':'X63-R','fstype':'exfat','mountpoint':str(volume)}]}).encode()
        return json.dumps({'filesystems':[{'target':str(volume),'fstype':'exfat'}]}).encode()
    with patch.object(c.subprocess,'check_output',side_effect=output):check(c.destination_root()==volume)
    with patch.object(c.subprocess,'check_output',return_value=b'{"blockdevices":[]}'):
        try:c.destination_root(str(volume))
        except ValueError:check(True)
        else:check(False)
print('PASS: collector decoder/destination,',checks,'checks; QEMU records + synthetic host metadata')
