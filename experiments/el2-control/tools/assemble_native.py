#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Assemble three separate REL3 development images. Never installs anything.

Requires a host libfdt library, immutable REL3 input copies and a native build.
Reconstructs the original REL3 byte for byte before producing any new image.
"""
import argparse
import ctypes as C
import gzip
import hashlib
import json
from pathlib import Path
import shutil

def sha(b):
    return hashlib.sha256(b).hexdigest()

def modified_dtb(data, mode, library):
    lib=C.CDLL(str(library))
    lib.fdt_open_into.argtypes=[C.c_void_p,C.c_void_p,C.c_int]
    lib.fdt_path_offset.argtypes=[C.c_void_p,C.c_char_p]
    lib.fdt_setprop.argtypes=[C.c_void_p,C.c_int,C.c_char_p,C.c_void_p,C.c_int]
    lib.fdt_pack.argtypes=[C.c_void_p]
    src=C.create_string_buffer(data); out=C.create_string_buffer(len(data)+16384)
    def check(ret):
        if ret<0: raise ValueError('libfdt error '+str(ret))
        return ret
    check(lib.fdt_open_into(src,out,len(out)))
    node=check(lib.fdt_path_offset(out,b'/chosen'))
    value=C.create_string_buffer(mode.encode())
    check(lib.fdt_setprop(out,node,b'6boot,el2-mode',value,len(mode)+1))
    check(lib.fdt_pack(out))
    raw=out.raw; size=int.from_bytes(raw[4:8],'big')
    return raw[:size]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--inputs',type=Path,required=True)
    p.add_argument('--build',type=Path,required=True)
    p.add_argument('--libfdt',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    a=p.parse_args(); inputs=a.inputs.resolve(); build=a.build.resolve()
    facts=json.loads((inputs/'inputs.json').read_text())
    for name,meta in facts.items():
        if sha((inputs/name).read_bytes())!=meta['sha256']: raise SystemExit('Input changed: '+name)
    original=(inputs/'boot-rel3-original.bin').read_bytes()
    args=(inputs/'cmdline.txt').read_text().strip()
    kernel=(inputs/'Image.gz').read_bytes(); initrd=(inputs/'initramfs-p2t.bin').read_bytes()
    dtb=(inputs/'j713-rel3.dtb').read_bytes()
    assert gzip.decompress(kernel)[0x38:0x3c]==b'ARM\x64'
    assert gzip.decompress(initrd)[:5]==b'07070'
    def image(stage,tree,cmd):
        assert b'##m1n1_ver##' in stage and b'chosen.bootargs=' not in stage
        return stage+b'chosen.bootargs='+cmd.encode()+b'\n'+tree+kernel+initrd
    assert image((inputs/'stage2-original.bin').read_bytes(),dtb,args)==original
    assert sha(original)=='30e56865cd1c38802be96969e83b27ba803af6506e17f69f892adf78486d21e1'
    a.out.mkdir(parents=True,exist_ok=False)
    shutil.copytree(build/'source',a.out/'source',ignore=shutil.ignore_patterns('build','__pycache__','.git'))
    manifest=json.loads((build/'source-manifest.json').read_text())
    manifest.update({'rel3_reconstructed':True,'rel3_inputs':facts,'images':{},'cmdline_base':args})
    stage=(build/'m1n1-el2-native.bin').read_bytes()
    manifest['native_stage2_sha256']=sha(stage)
    (a.out/'m1n1-el2-native.bin').write_bytes(stage)
    (a.out/'el2-foundation.bin').write_bytes((build/'module/el2-foundation.bin').read_bytes())
    shutil.copy2(inputs/'boot-rel3-original.bin',a.out/'boot-rel3-original.bin')
    for mode in ['preflight','brk','guest']:
        cmd=args+' x6e3.el2='+mode+' x6e3.el2build='+manifest['build_id']
        tree=modified_dtb(dtb,mode,a.libfdt.resolve())
        (a.out/('j713-rel3-el2-'+mode+'.dtb')).write_bytes(tree)
        data=image(stage,tree,cmd)
        name='boot-rel3-el2-'+mode+'.bin';(a.out/name).write_bytes(data)
        manifest['images'][mode]={'file':name,'sha256':sha(data),'bytes':len(data),'cmdline':cmd,'dtb_sha256':sha(tree)}
    (a.out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps({'original_sha256':sha(original),'build_id':manifest['build_id'],'images':manifest['images']},indent=2))

if __name__=='__main__': main()
