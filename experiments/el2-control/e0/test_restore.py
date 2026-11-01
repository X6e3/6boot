#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Observe own-ELF restore mutations through QEMU's local GDB stub.

No target-side printf/C return is allowed after context restoration fails.
Breakpoints and corruptions affect disposable emulator RAM/images only.
"""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import time
import xml.etree.ElementTree as ET

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--qemu', required=True)
p.add_argument('--lib', default='')
p.add_argument('--worlds', action='store_true', help='also observe the new SINGLE-world restore path; no offset mutations')
p.add_argument('--visual', action='store_true', help='capture actual fb.c pixel output at completed world events')
p.add_argument('--bad-trace', action='store_true', help='visual test only: invalidate completion before final wire validation')
p.add_argument('--visual-smoke', action='store_true', help='framebuffer-only fixture; no EL1 or VM=1')
a = p.parse_args()
if a.visual_smoke: a.visual = True
if a.bad_trace and (not a.visual or a.visual_smoke): p.error('--bad-trace requires dual-world --visual')
a.out.mkdir(parents=True, exist_ok=False)
image = (a.build/'e0.elf').read_bytes()
symbols = {}
for line in subprocess.check_output(['aarch64-elf-nm','-n',str(a.build/'e0.elf')],text=True).splitlines():
    f = line.split()
    if len(f) == 3: symbols[f[2]] = int(f[0],16)
phoff, = struct.unpack_from('<Q',image,32)
size,count = struct.unpack_from('<HH',image,54)

def offset(address):
    for n in range(count):
        kind,_,off,va,_,filesz,_,_ = struct.unpack_from('<IIQQQQQQ',image,phoff+n*size)
        if kind == 1 and va <= address < va+filesz: return off+address-va
    raise ValueError('mutation outside file-backed own ELF')

def site(reg):
    dis = (a.build/'e0.dis').read_text()
    matches = [int(m[1],16) for m in re.finditer(
        r'^\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+msr\s+'+reg+r', x10\b',dis,re.M)]
    return max(v for v in matches if symbols['e0_restore_vtcr'] <= v < symbols['e0_restore_failed'])

# Test only: minimal RSP transport for a local, diskless QEMU child.
class Debugger:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(5)
        self.sock.connect(path)
        self.registers = {}
        self.next_reg = 0
        self.command('qSupported:qXfer:features:read+')
        self.command('?')
        self.xml('target.xml')

    def read(self, n):
        data = b''
        while len(data) < n:
            b = self.sock.recv(n-len(data))
            if not b: raise EOFError('QEMU debugger disconnected')
            data += b
        return data

    def command(self, text):
        data = text.encode()
        self.sock.sendall(b'$'+data+b'#'+f'{sum(data)%256:02x}'.encode())
        return self.receive()

    def interrupt(self):
        self.sock.sendall(b'\x03')
        return self.receive()

    def receive(self):
        while self.read(1) != b'$': pass
        raw = bytearray()
        while True:
            c = self.read(1)
            if c == b'#': break
            raw += c
        assert int(self.read(2),16) == sum(raw)%256
        self.sock.sendall(b'+')
        out = bytearray(); i = 0
        while i < len(raw):
            c = raw[i]; i += 1
            if c == 125:
                out.append(raw[i]^32); i += 1
            elif c == 42:
                out.extend(out[-1:]*(raw[i]-29)); i += 1
            else: out.append(c)
        return bytes(out)

    def xml(self, name):
        data = b''
        while True:
            b = self.command(f'qXfer:features:read:{name}:{len(data):x},1000')
            assert b[:1] in (b'm',b'l')
            data += b[1:]
            if b[:1] == b'l': break
        doc = ET.fromstring(data.replace(b'<target>',b'<target xmlns:xi="http://www.w3.org/2001/XInclude">'))
        for e in doc:
            if e.tag.endswith('include'): self.xml(e.attrib['href'])
            elif e.tag == 'reg':
                n = int(e.attrib.get('regnum',self.next_reg))
                self.registers[e.attrib['name']] = n
                self.next_reg = n+1

    def reg(self, name):
        b = self.command(f'p{self.registers[name]:x}')
        assert b and not b.startswith(b'E'), (name,b)
        return int.from_bytes(bytes.fromhex(b.decode()),'little')

    def memory(self, address, length):
        b = self.command(f'm{address:x},{length:x}')
        assert len(b) == length*2, b
        return bytes.fromhex(b.decode())

    def breakpoint(self, address, install=True):
        assert self.command(f'{"Z" if install else "z"}0,{address:x},4') == b'OK'

vtcr, hcr, vbar, daif = [site(r) for r in ['vtcr_el2','hcr_el2','vbar_el2','daif']]
call = offset(symbols['test_call'])
# The standalone caller's unique BLR x16; +4 is the first caller instruction.
needle = struct.pack('<I',0xd63f0200)
region = image[call:call+256]
assert region.count(needle) == 1
returned = symbols['test_call']+region.index(needle)+4
cases = [('skip-vtcr-write',vtcr,0xd503201f,3),
         ('bad-vtcr-readback',vtcr+8,0xaa1f03eb,3),
         ('trap-vtcr-write',vtcr,0,4), ('trap-hcr-write',hcr,0,4),
         ('trap-vbar-write',vbar,0,4), ('bad-vbar-readback',vbar+8,0xaa1f03eb,3),
         ('trap-daif-write',daif,0,4)]
@contextmanager
def emulator(out, data):
    out.mkdir()
    (out/'e0.elf').write_bytes(data)
    with tempfile.TemporaryDirectory(prefix='e0-gdb-',dir='/tmp') as d:
        path=d+'/stub'
        cmd=[a.qemu,'-machine','virt,virtualization=on,secure=off,gic-version=3',
             '-cpu','max','-accel','tcg','-smp','1','-m','128M','-nodefaults',
             '-display','none','-serial','none','-monitor','none','-nic','none',
             '-semihosting-config','enable=on,target=native','-S','-gdb',
             'unix:'+path+',server=on,wait=off','-device',
             'loader,file='+str((out/'e0.elf').resolve())+',cpu-num=0']
        (out/'command.json').write_text(json.dumps(cmd,indent=2)+'\n')
        env=os.environ.copy()
        if a.lib: env['DYLD_LIBRARY_PATH']=str(Path(a.lib).resolve())
        with (out/'serial.log').open('wb') as log:
            proc=subprocess.Popen(cmd,env=env,stdout=log,stderr=subprocess.STDOUT)
            g=None
            try:
                for _ in range(100):
                    if Path(path).exists(): break
                    if proc.poll() is not None: raise RuntimeError('QEMU startup failed')
                    time.sleep(.02)
                g=Debugger(path)
                yield g
            finally:
                if g: g.sock.close()
                proc.terminate();proc.wait(timeout=5)

if a.visual:
    import importlib.util
    import zlib
    spec = importlib.util.spec_from_file_location('decoder',Path(__file__).with_name('worlds_decode.py'))
    decoder = importlib.util.module_from_spec(spec);spec.loader.exec_module(decoder)
    expect = {'build_id':re.search(r'0x([0-9a-f]{16})',(a.build/'worlds_build.h').read_text())[1], 'arena':symbols['e0_arena_start'],
              'pcs':{k:0x10000000+symbols[v]-symbols['e0_guest_vectors'] for k,v in
                     [('yield','ew_after_yield'),('permission','ew_permission_pc'),('foreign','ew_foreign_pc'),('done','ew_after_done')]}}
    (a.out/'expect.json').write_text(json.dumps(expect,indent=2)+'\n')
    def png(path,raw,width,height,stride,depth):
        rows=[]
        for y in range(height):
            p=raw[y*stride:y*stride+width*4]
            rgb=bytearray(width*3)
            for x in range(width):
                value=int.from_bytes(p[4*x:4*x+4],'little')
                rgb[3*x:3*x+3]=bytes([(value>>(22 if depth==30 else 16))&255,
                                     (value>>(12 if depth==30 else 8))&255,(value>>(2 if depth==30 else 0))&255])
            rows.append(b'\0'+rgb)
        def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
        path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>2I5B',width,height,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(b''.join(rows)))+chunk(b'IEND',b''))
    captures=[]
    with emulator(a.out/'machine',image) as g:
        g.breakpoint(symbols['ew_visual_checkpoint'])
        if a.bad_trace:g.breakpoint(symbols['ew_visual_finish'])
        final = 3 if a.visual_smoke else 4
        for stage in range(1,final+1):
            try: g.command('c')
            except TimeoutError:
                g.interrupt()
                report={n:g.reg(n) for n in ['pc','cpsr','HCR_EL2','PAR_EL1']+['x'+str(i) for i in range(31)]}
                report['refusals']=int.from_bytes(g.memory(symbols['ew_visual_refusals'],8),'little')
                (a.out/'timeout.json').write_text(json.dumps(report,indent=2)+'\n')
                raise
            if a.bad_trace and g.reg('pc')==symbols['ew_visual_finish']:
                at=symbols['ew_trace']+128+64 # World A completion
                assert int.from_bytes(g.memory(at,8),'little')==1
                assert g.command(f'M{at:x},8:'+bytes(8).hex())==b'OK'
                g.breakpoint(symbols['ew_visual_finish'],False);g.command('c')
            assert g.reg('pc')==symbols['ew_visual_checkpoint']
            step=int.from_bytes(g.memory(symbols['ew_visual_step'],8),'little')
            status=int.from_bytes(g.memory(symbols['ew_visual_status'],8),'little')
            assert step==stage and status==(3 if a.visual_smoke else (2 if a.bad_trace and stage==4 else (1 if stage==4 else 0)))
            assert int.from_bytes(g.memory(symbols['ew_visual_refusals'],8),'little')==10
            base,width,height,stride,depth=struct.unpack('<5Q',g.memory(symbols['ew_visual_meta'],40))
            assert (width,height,stride)==(640,360,656*4) and depth in (30,32)
            assert g.memory(base-128,128)==g.memory(base+stride*height,128)==bytes([0xa5])*128
            raw=b''.join(g.memory(base+i,min(2048,stride*height-i)) for i in range(0,stride*height,2048))
            (a.out/f'frame-{stage}.xrgb').write_bytes(raw)
            png(a.out/f'frame-{stage}.png',raw,width,height,stride,depth)
            rawpixel=lambda x,y:int.from_bytes(raw[y*stride+x*4:y*stride+x*4+4],'little')
            pixel=lambda x,y:rawpixel(x,y) if depth==32 else (((rawpixel(x,y)>>22)&255)<<16 | ((rawpixel(x,y)>>12)&255)<<8 | (rawpixel(x,y)>>2)&255)
            world=1 if stage==2 else 0;x0,y0=120,90
            assert pixel(x0+45,y0+18)==(0x2d3741 if world else 0x207dd7)
            assert pixel(x0+225,y0+18)==(0x207dd7 if world else 0x2d3741)
            for y in range(height):
                for x in range(stride//4):
                    if not (x0<=x<x0+400 and y0<=y<y0+180):assert rawpixel(x,y)==0x101820
            if a.visual_smoke:
                assert not g.reg('HCR_EL2') & 1 and g.reg('PAR_EL1')==0xabcd0000
                assert not any(g.memory(symbols['ew_trace'],1856)), 'smoke must not execute a world'
                assert sum(pixel(x,y)==0x4be682 for y in range(230,246) for x in range(160,440))==0, 'no green PASS in smoke'
            if stage==final and a.visual_smoke:
                s=importlib.util.spec_from_file_location('smoke',Path(__file__).with_name('visual_smoke_decode.py'))
                smoke=importlib.util.module_from_spec(s);s.loader.exec_module(smoke)
                blob=g.memory(symbols['ew_smoke_trace'],80);(a.out/'smoke.bin').write_bytes(blob)
                (a.out/'smoke.json').write_text(json.dumps(smoke.decode(blob,expect['build_id']),indent=2)+'\n')
                for at,value,size in [(0,0,4),(4,2,2),(6,79,2),(8,2,4),(12,1,4),(24,1,8),
                                      (48,0,8),(56,399,4),(60,0xffffffff,4),(64,4,4),(68,16,4),(72,0,8)]:
                    bad=bytearray(blob);bad[at:at+size]=value.to_bytes(size,'little')
                    try:
                        bad_report=smoke.decode(bad,expect['build_id'])
                        if bad_report['verdict'] != 'SMOKE_TRACE_VALID':continue
                    except ValueError:continue
                    raise AssertionError(('invalid smoke record accepted',at))
            elif stage==4:
                color=0xf04b46 if a.bad_trace else 0x4be682
                assert sum(pixel(x,y)==color for y in range(230,246) for x in range(160,320))>20
                blob=g.memory(symbols['ew_trace'],1856);(a.out/'trace.bin').write_bytes(blob)
                try:report=decoder.decode(blob,expect)
                except ValueError as e:
                    assert a.bad_trace;report={'verdict':'FAIL','reason':str(e)}
                assert report['verdict']==('FAIL' if a.bad_trace else 'PASS')
                (a.out/'trace.json').write_text(json.dumps(report,indent=2)+'\n')
            captures.append({'step':step,'status':status,'world':world,'pc':hex(g.reg('pc')),
                             'hcr':hex(g.reg('HCR_EL2')),'visual_ticks':int.from_bytes(g.memory(symbols['ew_visual_ticks'],8),'little')})
            print('VISUAL_FRAME_PASS',stage,'world',world,'status',status,flush=True)
            if stage < final:
                g.breakpoint(symbols['ew_visual_checkpoint'],False);g.command('s')
                g.breakpoint(symbols['ew_visual_checkpoint'])
    (a.out/'frames.json').write_text(json.dumps(captures,indent=2)+'\n')
    print('VISUAL_SMOKE_PASS: VM off, no world, no PASS label' if a.visual_smoke else ('VISUAL_NEGATIVE_PASS: corrupt binary trace displays FAIL' if a.bad_trace else 'VISUAL_PASS: actual A/B/A execution; C and Python wire policies agree'))
    raise SystemExit(0)

observed_names=['HCR_EL2','VBAR_EL2','VTCR_EL2','VTTBR_EL2','cpsr','sp',
                'SCTLR_EL1','VBAR_EL1','ELR_EL1','SPSR_EL1','ESR_EL1','FAR_EL1',
                'SP_EL1','SP_EL0','PAR_EL1','HPFAR_EL2','ELR_EL2','SPSR_EL2','ESR_EL2','FAR_EL2']
if a.worlds:
    observed_names += ['TCR_EL1','TTBR0_EL1','TTBR1_EL1','MAIR_EL1','AMAIR_EL1',
                       'CONTEXTIDR_EL1','TPIDR_EL1','TPIDR_EL0','TPIDRRO_EL0']
results=[]
for name,address,word,status in cases:
    out=a.out/name
    data=bytearray(image);struct.pack_into('<I',data,offset(address),word)
    with emulator(out,data) as g:
        g.breakpoint(symbols['sixboot_e0_run']); g.command('c')
        assert g.reg('pc') == symbols['sixboot_e0_run']
        before = {n:g.reg(n) for n in observed_names}
        if a.worlds: assert g.command('P0=0600000000000000') == b'OK' # SINGLE, not VM0 prepare
        g.breakpoint(symbols['sixboot_e0_run'],False)
        stops = {returned:'UNSAFE_C_RETURN',symbols['test_exception']:'INHERITED_EXCEPTION'}
        if 'e0_terminal_park' in symbols: stops[symbols['e0_terminal_park']] = 'TERMINAL'
        for at in stops: g.breakpoint(at)
        stop = g.command('c'); pc = g.reg('pc')
        after = {n:g.reg(n) for n in before}
        def value(sym): return int.from_bytes(g.memory(symbols[sym],8),'little')
        record = {'case':name,'mutation_pc':hex(address),'word':hex(word),'stop':stop.decode(),
                  'event':stops.get(pc,'UNKNOWN'),'pc':hex(pc),
                  'status':value('e0_status'),'before':before,'after':after,
                  'restoring':value('e0_restoring'),
                  'stage2_config':list(struct.unpack('<2Q',g.memory(symbols['e0_state']+64,16))),
                  'guest_returned':int.from_bytes(g.memory(symbols['e0_state']+16,8),'little'),
                  'saved_context':list(struct.unpack('<20Q',g.memory(symbols['e0_saved'],160))),
                  'trace_header':list(struct.unpack('<8Q',g.memory(symbols['e2_exception_trace'],64)))}
        record['pass'] = (record['event'] == 'TERMINAL' and record['status'] == status and
                          after['cpsr']&0x3df == 0x3c9 and record['restoring'] == 1 and
                          record['guest_returned'] == 0 and
                          symbols['__e2_stack_start'] <= after['sp'] <= symbols['__e2_stack_end'])
        if status == 4:
            record['pass'] &= record['trace_header'][1] == 1 and record['trace_header'][2] == 0x02000000 and record['trace_header'][3] == address and record['trace_header'][6] == 4
        # Before VBAR commit our own vectors must remain installed;
        # readback mismatch after a successful commit must park anyway.
        wanted_vbar = before['VBAR_EL2'] if name == 'bad-vbar-readback' else symbols['e2_vectors']
        record['pass'] &= after['VBAR_EL2'] == wanted_vbar
        wanted_hcr = before['HCR_EL2'] & ~0x8000000 if name in {
            'skip-vtcr-write','bad-vtcr-readback','trap-vtcr-write','trap-hcr-write'} else before['HCR_EL2']
        record['pass'] &= after['HCR_EL2'] == wanted_hcr
        early = name in {'skip-vtcr-write','bad-vtcr-readback','trap-vtcr-write'}
        wanted_vtcr = record['stage2_config'][0] if name in {'skip-vtcr-write','trap-vtcr-write'} else before['VTCR_EL2']
        wanted_vttbr = record['stage2_config'][1] if early else before['VTTBR_EL2']
        record['pass'] &= after['VTCR_EL2'] == wanted_vtcr and after['VTTBR_EL2'] == wanted_vttbr
        record['pass'] &= record['saved_context'][1:5] == [before[n] for n in ['VBAR_EL2','HCR_EL2','VTCR_EL2','VTTBR_EL2']]
        if a.worlds:
            fatal = list(struct.unpack('<8Q', g.memory(symbols['ew_trace'],64)))
            record['world_trace_header'] = fatal
            record['pass'] &= fatal[5] == status and fatal[6] == 0
        (out/'observation.json').write_text(json.dumps(record,indent=2)+'\n')
        results.append(record)
        print(name,record['event'],'status',record['status'],'PASS' if record['pass'] else 'FAIL',flush=True)

if a.worlds:
    # These checks observe both the return contract and the actual machine
    # registers. All mutations affect disposable QEMU images/RAM only.
    negative = ['prepare-vm0', 'missing-permission-fault', 'missing-foreign-fault',
                'wrong-permission-address', 'invalid-far', 'cross-world-map',
                'vmid-collision', 'missing-bank-load', 'missing-vttbr', 'capability-refusal', 'execute-data',
                'inherited-vmid-collision', 'stage1-enabled-at-fault', 'stack-guard']
    dis = (a.build/'e0.dis').read_text()
    def instruction(function, pattern):
        start = symbols[function]
        match = re.search(r'^\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+'+pattern, dis[dis.index(f'<{function}>:'):], re.M)
        assert match
        address = int(match[1],16)
        assert address >= start
        return address
    for name in negative:
        out = a.out/name
        data = bytearray(image)
        patches = {
            'missing-permission-fault': (symbols['ew_permission_pc'], 0xd503201f),
            'missing-foreign-fault': (symbols['ew_foreign_pc'], 0xd503201f),
            'missing-bank-load': (instruction('load_bank', r'msr\s+tpidr_el1,'), 0xd503201f),
            'missing-vttbr': (instruction('select_world', r'msr\s+vttbr_el2,'), 0xd503201f),
        }
        if name in patches:
            at, word = patches[name]; struct.pack_into('<I', data, offset(at), word)
        if name == 'capability-refusal':
            struct.pack_into('<2I', data, offset(symbols['ew_capable']), 0x52800000, 0xd65f03c0)
        if name == 'inherited-vmid-collision':
            # Execute the real harness MSR. QEMU's dynamic system-register
            # GDB writes may acknowledge without changing architectural state.
            at = instruction('test_main', r'movk\s+x\d+,\s*#0x3, lsl #48')
            old = struct.unpack_from('<I',data,offset(at))[0]
            assert old & 0xffe00000 == 0xf2e00000
            struct.pack_into('<I',data,offset(at),(old & ~(0xffff << 5)) | (0x7e << 5))
        with emulator(out, data) as g:
            g.breakpoint(symbols['sixboot_e0_run']); g.command('c')
            before = {n:g.reg(n) for n in observed_names}
            if name == 'inherited-vmid-collision':
                assert before['VTTBR_EL2'] >> 48 == 0x7e
            g.breakpoint(symbols['sixboot_e0_run'],False)
            mode = 5 if name == 'prepare-vm0' else 7
            assert g.command('P0='+struct.pack('<Q',mode).hex()) == b'OK'
            if name in {'cross-world-map','vmid-collision'}:
                g.breakpoint(symbols['e0_seed_brk']);g.command('c')
                assert g.reg('pc') == symbols['e0_seed_brk']
                at = symbols['ew_tables_b']+0x4000+16 if name == 'cross-world-map' else symbols['ew_trace']+128+96
                value = int.from_bytes(g.memory(symbols['e0_tables']+0x4000+16,8),'little') if name == 'cross-world-map' else 0x7e
                assert g.command(f'M{at:x},8:'+struct.pack('<Q',value).hex()) == b'OK'
                g.breakpoint(symbols['e0_seed_brk'],False)
            if name == 'prepare-vm0':
                g.breakpoint(symbols['e0_guest_return']);g.command('c')
                assert g.reg('pc') == symbols['e0_guest_return']
                assert g.reg('HCR_EL2') == before['HCR_EL2'] and not g.reg('HCR_EL2') & 1
                assert g.reg('VTTBR_EL2') == before['VTTBR_EL2'] and g.reg('VTCR_EL2') == before['VTCR_EL2']
                assert struct.unpack('<2Q', g.memory(symbols['ew_trace']+56,16)) == (0,0)
                g.breakpoint(symbols['e0_guest_return'],False)
            if name in {'wrong-permission-address','invalid-far','stage1-enabled-at-fault','stack-guard'}:
                at = 0x10000000+symbols['ew_permission_pc']-symbols['e0_guest_vectors']
                g.breakpoint(at);g.command('c');assert g.reg('pc') == at
                if name in {'wrong-permission-address','stack-guard'}:
                    target = 0x10010000 if name == 'stack-guard' else 0x1000c008
                    assert g.command('P1='+struct.pack('<Q',target).hex()) == b'OK'
                g.breakpoint(at,False)
                if name in {'invalid-far','stage1-enabled-at-fault'}:
                    # Inject into the saved frame. QEMU can normalize a
                    # direct ESR system-register write for this syndrome;
                    # this tests rejection of an invalid telemetry input.
                    g.breakpoint(symbols['ew_dispatch']);g.command('c')
                    if name == 'invalid-far':
                        frame_esr = g.reg('x0') + 256
                        esr = int.from_bytes(g.memory(frame_esr,8),'little') | 1 << 10
                        value = struct.pack('<Q',esr)
                        assert g.command(f'M{frame_esr:x},8:'+value.hex()) == b'OK'
                        assert g.memory(frame_esr,8) == value
                    else:
                        value = g.reg('SCTLR_EL1') | 1
                        resume, old_x8 = g.reg('pc'), g.reg('x8')
                        msr = instruction('load_bank', r'msr\s+sctlr_el12, x8')
                        for reg, val in [('x8',value),('pc',msr)]:
                            assert g.command(f'P{g.registers[reg]:x}='+struct.pack('<Q',val).hex()) == b'OK'
                        g.command('s');g.command('s') # own MSR, then its ISB
                        assert g.reg('pc') == msr+8
                        assert g.reg('SCTLR_EL1') == value
                        for reg, val in [('x8',old_x8),('pc',resume)]:
                            assert g.command(f'P{g.registers[reg]:x}='+struct.pack('<Q',val).hex()) == b'OK'
                    g.breakpoint(symbols['ew_dispatch'],False)
            if name == 'execute-data':
                at = 0x10000000+symbols['ew_after_yield']-symbols['e0_guest_vectors']
                g.breakpoint(at);g.command('c');assert g.reg('pc') == at
                assert g.command(f'P{g.registers["pc"]:x}='+struct.pack('<Q',0x10008000).hex()) == b'OK'
                g.breakpoint(at,False)
            if name == 'stage1-enabled-at-fault':
                post = instruction('e2_exception_common', r'bl\s+.*<e2_exception_dispatch>') + 4
                g.breakpoint(post);g.command('c');assert g.reg('pc') == post
                trace = g.memory(symbols['ew_trace'],1856)
                count = struct.unpack_from('<Q',trace,64)[0]
                last = struct.unpack_from('<16Q',trace,320+128*(count-1))
                if last[1] != 5 or last[11] or last[12] or last[13]:
                    # Stop the rejected implementation before its unsafe
                    # ERET. A later EL1 exception loop is not a safe rejection.
                    record = {'case':name,'event':'INVALID_STAGE1_FAULT_ACCEPTED',
                              'sctlr':g.reg('SCTLR_EL1'),'last_exception':list(last),'pass':False}
                    (out/'trace.bin').write_bytes(trace)
                    (out/'observation.json').write_text(json.dumps(record,indent=2)+'\n')
                    results.append(record);print(name,record['event'],'FAIL',flush=True)
                    continue
                g.breakpoint(post,False)
            stops = {returned:'C_RETURN', symbols['e0_terminal_park']:'TERMINAL',symbols['test_exception']:'INHERITED_EXCEPTION'}
            for at in stops: g.breakpoint(at)
            g.command('c'); event = stops.get(g.reg('pc'),'UNKNOWN')
            after = {n:g.reg(n) for n in observed_names}
            status = int.from_bytes(g.memory(symbols['e0_status'],8),'little')
            trace = g.memory(symbols['ew_trace'],1856);(out/'trace.bin').write_bytes(trace)
            fatal = name in {'cross-world-map','vmid-collision','missing-bank-load','missing-vttbr'}
            wanted = 4 if fatal else (1 if name in {'capability-refusal','inherited-vmid-collision'} else (0 if name == 'prepare-vm0' else 2))
            good = status == wanted and event == ('TERMINAL' if fatal else 'C_RETURN')
            if fatal:
                good &= after['cpsr'] & 0x3df == 0x3c9 and int.from_bytes(trace[48:56],'little') == 0
            else:
                good &= g.reg('x0') == wanted and all(after[n] == before[n] for n in before if n != 'cpsr')
                good &= after['cpsr'] & 0x3df == before['cpsr'] & 0x3df
            record = {'case':name,'event':event,'status':status,'before':before,'after':after,'pass':bool(good)}
            if name in {'execute-data','invalid-far','stage1-enabled-at-fault','stack-guard'}:
                count = struct.unpack_from('<Q',trace,64)[0]
                last = struct.unpack_from('<16Q',trace,320+128*(count-1))
                record['last_exception'] = list(last)
                if name == 'execute-data':
                    record['pass'] &= last[6] >> 26 == 0x20 and last[6] & 63 == 0xf and last[9] == 0x10008000
                elif name == 'invalid-far':
                    record['pass'] &= bool(last[6] & (1 << 10)) and last[11] == last[12] == 0
                elif name == 'stack-guard':
                    record['pass'] &= last[6] >> 26 == 0x24 and last[6] & 0x7ff == 0x47 and last[9] == 0x10010000
                else:
                    # FAR is not proof of IPA once the required Stage1-off
                    # execution contract is lost. Reject this very event.
                    record['pass'] &= count == 3 and last[1] == 5 and last[11] == last[12] == last[13] == 0
            (out/'observation.json').write_text(json.dumps(record,indent=2)+'\n');results.append(record)
            print(name,event,'status',status,'PASS' if record['pass'] else 'FAIL',flush=True)

# Byte stores isolate the IPA-offset question from unaligned 64-bit STR/page
# crossing rules. Only throwaway ELF copies change; production STR is audited.
# Every target differs from the expected canary, so E0 must reject it and fully
# restore its caller, even when raw FAR/HPFAR reconstruct a valid faulting IPA.
guest_fault=0x10000000+symbols['e0_fault_pc']-symbols['e0_guest_vectors']
for delta in ([] if a.worlds else [0,0xfff,0x1000,0x2000,0x3fff]):
    out=a.out/f'fault-offset-{delta:04x}'
    data=bytearray(image)
    assert struct.unpack_from('<I',data,offset(symbols['e0_fault_pc']))[0]==0xf9000023
    struct.pack_into('<I',data,offset(symbols['e0_fault_pc']),0x39000023) # STRB w3,[x1]
    with emulator(out,data) as g:
        g.breakpoint(symbols['sixboot_e0_run']);g.command('c')
        assert g.reg('pc')==symbols['sixboot_e0_run']
        names=observed_names
        before={n:g.reg(n) for n in names}
        assert g.command('P0=0400000000000000')==b'OK' # existing STATE_DENY mode
        g.breakpoint(symbols['sixboot_e0_run'],False)
        g.breakpoint(guest_fault);g.command('c')
        assert g.reg('pc')==guest_fault
        target=(symbols['e0_state_canary']&~0x3fff)+delta
        assert target!=symbols['e0_state_canary']
        assert g.command('P1='+struct.pack('<Q',target).hex())==b'OK'
        g.breakpoint(guest_fault,False)
        g.breakpoint(symbols['e2_exception_common']);g.command('c')
        assert g.reg('pc')==symbols['e2_exception_common']
        fault={n:g.reg(n) for n in ['ESR_EL2','FAR_EL2','HPFAR_EL2','ELR_EL2','SPSR_EL2','VTCR_EL2']}
        reconstructed=((fault['HPFAR_EL2']&0xfffffffff0)<<8)|(fault['FAR_EL2']&0xfff)
        assert fault['ESR_EL2']>>26==0x24 and fault['ESR_EL2']&0x7ff==0x46
        assert fault['VTCR_EL2']>>14&3==2 and fault['SPSR_EL2']&0x3df==0x3c5
        assert fault['FAR_EL2']==reconstructed==target and fault['ELR_EL2']==guest_fault
        g.breakpoint(symbols['e2_exception_common'],False)
        g.breakpoint(returned);g.command('c')
        assert g.reg('pc')==returned and g.reg('x0')==2 # UNEXPECTED, not accepted fault
        after={n:g.reg(n) for n in names}
        assert all(after[n]==before[n] for n in names if n!='cpsr')
        assert after['cpsr']&0x3df==before['cpsr']&0x3df
        assert int.from_bytes(g.memory(symbols['e0_state_canary'],8),'little')==0x6e376e376e376e37
        record={'case':out.name,'target':hex(target),'fault':fault,'reconstructed':hex(reconstructed),
                'return':2,'before':before,'after':after,'pass':True}
        (out/'observation.json').write_text(json.dumps(record,indent=2)+'\n')
        results.append(record)
        print(out.name,'IPA coherent; unexpected target rejected; caller restored PASS',flush=True)
(a.out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
if not all(r['pass'] for r in results): raise SystemExit('FAIL: unsafe restore return or unexpected terminal state; evidence retained')
print('E0_RESTORE_NEGATIVE_PASS: terminal stop, no caller continuation; register and emergency-trace checks')
if not a.worlds: print('E0_OFFSET_PASS: five actual 16 KiB Stage-2 byte-store faults; unexpected targets rejected')
