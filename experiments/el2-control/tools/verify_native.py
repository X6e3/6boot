#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read-only independent bundle/boot format verification; no libfdt needed."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

def sha(b):return hashlib.sha256(b).hexdigest()
def tree(data):
    h=struct.unpack_from('>10I',data)
    assert h[0]==0xd00dfeed and h[1]==len(data)
    s=h[2];strings=data[h[3]:h[3]+h[8]];stack=[];props={}
    while True:
        token=struct.unpack_from('>I',data,s)[0];s+=4
        if token==1:
            end=data.index(0,s);stack.append(data[s:end].decode());s=(end+4)&~3
        elif token==2:stack.pop()
        elif token==3:
            size,off=struct.unpack_from('>II',data,s);s+=8
            name=strings[off:strings.index(0,off)].decode();key='/'.join(stack)+'/'+name
            assert key not in props and s+size<=h[2]+h[9]
            props[key]=data[s:s+size];s=(s+size+3)&~3
        elif token==4:pass
        elif token==9:break
        else:raise ValueError('invalid FDT token')
    assert not stack
    return props

def main():
    p=argparse.ArgumentParser();p.add_argument('--bundle',type=Path,default=Path(__file__).resolve().parent);a=p.parse_args()
    b=a.bundle;m=json.loads((b/'manifest.json').read_text());facts=m['rel3_inputs']
    original=(b/'boot-rel3-original.bin').read_bytes();assert sha(original)==facts['boot-rel3-original.bin']['sha256']
    size=facts['stage2-original.bin']['bytes'];oldstage=original[:size];assert sha(oldstage)==facts['stage2-original.bin']['sha256']
    args=b'chosen.bootargs='+m['cmdline_base'].encode()+b'\n';assert original[size:size+len(args)]==args;size+=len(args)
    dtb=original[size:size+facts['j713-rel3.dtb']['bytes']];size+=len(dtb);assert sha(dtb)==facts['j713-rel3.dtb']['sha256']
    kernel=original[size:size+facts['Image.gz']['bytes']];size+=len(kernel);initrd=original[size:]
    assert sha(kernel)==facts['Image.gz']['sha256'] and sha(initrd)==facts['initramfs-p2t.bin']['sha256']
    props=tree(dtb);stage=(b/'m1n1-el2-native.bin').read_bytes();assert sha(stage)==m['native_stage2_sha256']
    module=(b/'el2-foundation.bin').read_bytes();assert sha(module)==m['module_sha256'] and stage.count(module)==1
    for mode,meta in m['images'].items():
        newtree=(b/('j713-rel3-el2-'+mode+'.dtb')).read_bytes();newprops=tree(newtree)
        assert newprops.pop('/chosen/6boot,el2-mode')==mode.encode()+b'\0' and newprops==props
        assert sha(newtree)==meta['dtb_sha256']
        image=(b/meta['file']).read_bytes()
        assert len(image)==meta['bytes'] and sha(image)==meta['sha256']
        assert image==stage+b'chosen.bootargs='+meta['cmdline'].encode()+b'\n'+newtree+kernel+initrd
        print('PASS',mode,meta['sha256'],'exact REL3 kernel/initrd; DT only adds mode')
    for name,digest in m['source'].items():assert sha((b/'source/6boot'/name).read_bytes())==digest
    print('PASS original REL3 reconstruction, stage2/module/source manifest; build',m['build_id'])

if __name__=='__main__':main()
