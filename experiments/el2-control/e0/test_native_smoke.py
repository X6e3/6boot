#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Execute the actual native E0 smoke runner and FDT publisher in diskless TCG.

The local debugger observes real framebuffer calls and real udelay calls. Only
disposable emulator ELF copies receive architectural restore fault mutations.
No physical device, network backend, disk backend, or production file is used.
"""
import argparse
from contextlib import contextmanager
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import socket
import struct
import subprocess
import tempfile
import time
import traceback
import xml.etree.ElementTree as ET
import zlib

TEST_ID = 0x4E53514D55323032
WIDTH, HEIGHT, STRIDE, GUARD = 640, 480, 656, 256
PIXEL_BYTES = STRIDE * HEIGHT * 4
ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('native_smoke_decoder', HERE / 'visual_smoke_decode.py')
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)


def require(valid, message):
    if not valid:
        raise AssertionError(message)


class Debugger:
    """Minimal RSP client restricted to the local child emulator."""
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(15)
        self.sock.connect(path)
        self.registers, self.next_reg = {}, 0
        self.command('qSupported:qXfer:features:read+')
        self.command('?')
        self.xml('target.xml')

    def read(self, size):
        data = b''
        while len(data) < size:
            chunk = self.sock.recv(size - len(data))
            if not chunk:
                raise EOFError('QEMU debugger disconnected')
            data += chunk
        return data

    def receive(self):
        while self.read(1) != b'$':
            pass
        raw = bytearray()
        while True:
            value = self.read(1)
            if value == b'#':
                break
            raw += value
        require(int(self.read(2), 16) == sum(raw) % 256, 'RSP checksum')
        self.sock.sendall(b'+')
        result, index = bytearray(), 0
        while index < len(raw):
            value = raw[index]
            index += 1
            if value == 125:
                result.append(raw[index] ^ 32)
                index += 1
            elif value == 42:
                result.extend(result[-1:] * (raw[index] - 29))
                index += 1
            else:
                result.append(value)
        return bytes(result)

    def command(self, text):
        data = text.encode()
        self.sock.sendall(b'$' + data + b'#' + f'{sum(data) % 256:02x}'.encode())
        return self.receive()

    def xml(self, name):
        data = b''
        while True:
            reply = self.command(f'qXfer:features:read:{name}:{len(data):x},1000')
            require(reply[:1] in (b'm', b'l'), 'QEMU register XML')
            data += reply[1:]
            if reply[:1] == b'l':
                break
        document = ET.fromstring(data.replace(b'<target>',
            b'<target xmlns:xi="http://www.w3.org/2001/XInclude">'))
        for entry in document:
            if entry.tag.endswith('include'):
                self.xml(entry.attrib['href'])
            elif entry.tag == 'reg':
                number = int(entry.attrib.get('regnum', self.next_reg))
                self.registers[entry.attrib['name']] = number
                self.next_reg = number + 1

    def reg(self, name):
        reply = self.command(f'p{self.registers[name]:x}')
        require(reply and not reply.startswith(b'E'), f'read register {name}')
        return int.from_bytes(bytes.fromhex(reply.decode()), 'little')

    def set_reg(self, name, value):
        data = value.to_bytes(8, 'little').hex()
        require(self.command(f'P{self.registers[name]:x}={data}') == b'OK', f'write {name}')

    def memory(self, address, size):
        reply = self.command(f'm{address:x},{size:x}')
        require(len(reply) == size * 2, f'read {size} fixture bytes at {address:x}: {reply[:80]!r}')
        return bytes.fromhex(reply.decode())

    def set_u32(self, address, value):
        require(self.command(f'M{address:x},4:' + struct.pack('<I', value).hex()) == b'OK',
                'set fixture parameter')

    def breakpoint(self, address, install=True):
        require(self.command(f'{"Z" if install else "z"}0,{address:x},4') == b'OK',
                f'{"install" if install else "remove"} local breakpoint')


def run_command(command, out, label):
    completed = subprocess.run(command, cwd=ROOT, text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    with (out / 'build.log').open('a') as log:
        log.write('$ ' + shlex.join(str(word) for word in command) + '\n')
        log.write(completed.stdout)
        log.write(f'exit={completed.returncode}\n\n')
    require(completed.returncode == 0, f'{label} failed; see {out / "build.log"}')
    return completed.stdout


def build(args, out):
    generated = out / 'generated'
    copied = out / 'source'
    generated.mkdir()
    (copied / 'src').mkdir(parents=True)
    (copied / 'build').mkdir()
    config = '#define SIXBOOT_E0_VISUAL 1\n#define SIXBOOT_E0_FB_SMOKE 1\n'
    (generated / 'build_cfg.h').write_text(config)
    (generated / 'e0_visual_build.h').write_text(f'#define E0_VISUAL_BUILD_ID UINT64_C(0x{TEST_ID:016x})\n')
    (copied / 'build/build_cfg.h').write_text(config)
    (copied / 'build/die_blocks.h').write_bytes((ROOT / 'data/die_blocks.h').read_bytes())
    # fb.c's existing ../build includes require this location. Its bytes are
    # copied unchanged: there are no source substitutions or test-side hooks.
    (copied / 'src/fb.c').write_bytes((ROOT / 'src/fb.c').read_bytes())
    (copied / 'src/memory.c').write_bytes((ROOT / 'src/memory.c').read_bytes())
    font = generated / 'font.S'
    font.write_text('.section .rodata.font,"a"\n.global _binary_build_font_bin_start\n'
                    '_binary_build_font_bin_start:\n.incbin ' + json.dumps(str(ROOT / 'font/font.bin')) +
                    '\n.section .note.GNU-stack,"",%progbits\n')
    flags = [args.cc, '--target=aarch64-none-elf', '-march=armv8.1-a', '-std=gnu11',
             '-O2', '-g', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
             '-fno-pic', '-mgeneral-regs-only', '-ffunction-sections', '-fdata-sections',
             '-Wall', '-Wextra', '-Werror', '-I' + str(generated), '-I' + str(HERE),
             '-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'src/libfdt'),
             '-I' + str(ROOT / 'sysinc'), '-DE2_TEST_NATIVE=1', '-DE2_TEST_FB_NC=1',
             '-DE2_TEST_HCR=0x32488000038']
    flags += ['-DE2_TEST_FB_ATTR=' + str({'nc': 0x44, 'nc-xs0': 0x40,
                                       'wb': 0xff, 'device': 0x00}[args.attribute])]
    sources = [ROOT / 'experiments/el2-control/tests/qemu/start.S', HERE / 'native_smoke_harness.c',
               HERE / 'visual_smoke.c', HERE / 'visual_smoke_record.c', copied / 'src/fb.c',
               copied / 'src/memory.c', ROOT / 'src/utils.c', ROOT / 'src/string.c', font]
    sources += [ROOT / 'src/libfdt' / (name + '.c') for name in
                ('fdt', 'fdt_ro', 'fdt_rw', 'fdt_wip', 'fdt_sw', 'fdt_empty_tree', 'fdt_check')]
    objects = []
    for source in sources:
        obj = out / (source.stem + '.o')
        run_command(flags + ['-c', str(source), '-o', str(obj)], out, source.name)
        objects.append(obj)
    elf = out / 'native-smoke.elf'
    run_command([args.ld, '--gc-sections', '--no-undefined', '--fatal-warnings',
                 '-z', 'max-page-size=0x4000', '-T', str(HERE / 'native_smoke_harness.ld'),
                 *map(str, objects), '-o', str(elf)], out, 'link actual native runner fixture')
    disassembly = run_command([args.objdump, '-d', str(elf)], out, 'disassembly')
    (out / 'native-smoke.dis').write_text(disassembly)
    symbol_text = run_command([args.nm, '-n', str(elf)], out, 'symbol inventory')
    (out / 'native-smoke.nm').write_text(symbol_text)
    source_hashes = {str(source): hashlib.sha256(source.read_bytes()).hexdigest() for source in sources}
    source_hashes[str(ROOT / 'src/fb.c')] = hashlib.sha256((ROOT / 'src/fb.c').read_bytes()).hexdigest()
    require(source_hashes[str(ROOT / 'src/fb.c')] == source_hashes[str(copied / 'src/fb.c')],
            'actual fb.c copied without substitutions')
    (out / 'sources.json').write_text(json.dumps(source_hashes, indent=2) + '\n')
    return elf


def symbols_from(text):
    symbols = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) == 3:
            symbols[fields[2]] = int(fields[0], 16)
    return symbols


def elf_file_offset(image, address):
    phoff, = struct.unpack_from('<Q', image, 32)
    size, count = struct.unpack_from('<HH', image, 54)
    for index in range(count):
        kind, _, offset, va, _, filesz, _, _ = struct.unpack_from('<IIQQQQQQ', image, phoff + index * size)
        if kind == 1 and va <= address < va + filesz:
            return offset + address - va
    raise ValueError('mutation outside file-backed disposable ELF')


def restore_site(disassembly, name):
    function = re.search(r'^([0-9a-f]+) <fb_e0_frame>:\n(.*?)(?=^[0-9a-f]+ <|\Z)',
                         disassembly, re.M | re.S)
    require(function is not None, 'actual fb_e0_frame disassembly')
    sites = [int(match[1], 16) for match in re.finditer(
        r'^\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+msr\s+' + name + r',\s*x\d+\b', function[2], re.M)]
    if name == 'daif':
        require(len(sites) == 2, 'one real DAIF mask and one real DAIF restore')
        return max(sites)
    require(len(sites) == 1, 'unique real PAR restore')
    return sites[0]


@contextmanager
def emulator(args, case_out, image):
    case_out.mkdir()
    elf = case_out / 'fixture.elf'
    elf.write_bytes(image)
    with tempfile.TemporaryDirectory(prefix='native-smoke-gdb-', dir='/tmp') as temp:
        path = temp + '/stub'
        command = [args.qemu, '-machine', 'virt,virtualization=on,secure=off,gic-version=3',
                   '-cpu', 'max', '-accel', 'tcg', '-smp', '1', '-m', '128M', '-nodefaults',
                   '-display', 'none', '-serial', 'none', '-monitor', 'none', '-nic', 'none',
                   '-semihosting-config', 'enable=on,target=native', '-S', '-gdb',
                   'unix:' + path + ',server=on,wait=off', '-device',
                   'loader,file=' + str(elf) + ',cpu-num=0']
        (case_out / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
        env = os.environ.copy()
        if args.qemu_lib_dir:
            env['DYLD_LIBRARY_PATH'] = str(Path(args.qemu_lib_dir).resolve())
        with (case_out / 'serial.log').open('wb') as log:
            process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
            debugger = None
            try:
                for _ in range(100):
                    if Path(path).exists():
                        break
                    require(process.poll() is None, 'QEMU startup failed')
                    time.sleep(.02)
                debugger = Debugger(path)
                yield debugger, process
            finally:
                if debugger:
                    debugger.sock.close()
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=5)


def png(path, raw):
    rows = []
    for row in range(180):
        source = raw[row * 1600:(row + 1) * 1600]
        rgb = bytearray(400 * 3)
        for x in range(400):
            pixel = int.from_bytes(source[x * 4:x * 4 + 4], 'little')
            rgb[x * 3:x * 3 + 3] = bytes([(pixel >> 16) & 255, (pixel >> 8) & 255, pixel & 255])
        rows.append(b'\0' + rgb)
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>2I5B', 400, 180, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b''))


def pixels(debugger, base):
    x0, y0 = (WIDTH - 400) // 2, (HEIGHT - 180) // 2
    left = int.from_bytes(debugger.memory(base + ((y0 + 20) * STRIDE + x0 + 50) * 4, 4), 'little')
    right = int.from_bytes(debugger.memory(base + ((y0 + 20) * STRIDE + x0 + 230) * 4, 4), 'little')
    return left, right


def run_case(args, out, symbols, original, disassembly, name, geometry=0, publication=0, fault=0):
    image = bytearray(original)
    mutation = None
    if fault in (1, 2):
        address = restore_site(disassembly, 'par_el1' if fault == 1 else 'daif')
        offset = elf_file_offset(image, address)
        old, = struct.unpack_from('<I', image, offset)
        struct.pack_into('<I', image, offset, 0xD503201F)  # NOP in an own disposable ELF.
        mutation = {'address': hex(address), 'old_opcode': hex(old), 'new_opcode': '0xd503201f'}
    case_out = out / name
    report = {'case': name, 'geometry': geometry, 'publication': publication, 'fault': fault,
              'attribute': args.attribute, 'mutation': mutation, 'frame_calls': [], 'delays': [], 'status': 'RUNNING'}
    try:
        with emulator(args, case_out, image) as (debugger, process):
            # Adopt the fixture's already-created host tables into the existing
            # mapper's ownership pointer. No production source hook or table rewrite.
            require(debugger.command(f'M{symbols["mmu_pt_L0"]:x},8:' +
                    struct.pack('<Q', 0 if geometry == 4 else symbols['test_l0']).hex()) == b'OK',
                    'fixture table owner (or intentional missing owner)')
            for field, value in [('native_smoke_case', geometry), ('native_smoke_publication', publication),
                                 ('native_smoke_fault', fault)]:
                debugger.set_u32(symbols[field], value)
            stops = {symbols['fb_e0_frame']: 'frame', symbols['udelay']: 'delay',
                     symbols['e0_visual_smoke_publish']: 'publish', symbols['test_exit']: 'exit'}
            for address in stops:
                debugger.breakpoint(address)
            base = symbols['native_smoke_pixels'] + GUARD
            published = False
            for _ in range(24):
                debugger.command('c')
                pc = debugger.reg('pc')
                require(pc in stops, f'unexpected stop at {pc:x}')
                kind = stops[pc]
                if kind == 'frame':
                    report['frame_calls'].append({'world': debugger.reg('x0'), 'state': debugger.reg('x1')})
                elif kind == 'delay':
                    pair = pixels(debugger, base)
                    index = len(report['delays'])
                    report['delays'].append({'microseconds': debugger.reg('x0'),
                        'left': hex(pair[0]), 'right': hex(pair[1]),
                        'hcr': hex(debugger.reg('HCR_EL2')), 'par': hex(debugger.reg('PAR_EL1'))})
                    if geometry == 0 and not fault and publication == 0:
                        x0, y0 = (WIDTH - 400) // 2, (HEIGHT - 180) // 2
                        raw = b''.join(debugger.memory(base + ((y0 + y) * STRIDE + x0) * 4, 1600)
                                       for y in range(180))
                        png(case_out / f'frame-{index + 1}.png', raw)
                        report['delays'][-1]['sha256'] = hashlib.sha256(raw).hexdigest()
                    if fault == 3 and index == 0:
                        before = debugger.reg('HCR_EL2')
                        saved_x0 = debugger.reg('x0')
                        hcr_msr = int(re.search(r'^\s*([0-9a-f]+):\s+d51c1100\s+msr\s+hcr_el2,\s*x0\b',
                                                disassembly, re.M)[1], 16)
                        debugger.set_reg('x0', before ^ 4)  # PTW is inert with VM=0.
                        debugger.set_reg('pc', hcr_msr)
                        debugger.command('s')
                        debugger.command('s')
                        debugger.set_reg('x0', saved_x0)
                        debugger.set_reg('pc', pc)
                        require(debugger.reg('HCR_EL2') == (before ^ 4), 'HCR_EL2 mutation applied')
                        report['hcr_mutation'] = {'before': hex(before), 'after': hex(debugger.reg('HCR_EL2')),
                            'point': 'entry to real udelay after first actual frame'}
                elif kind == 'publish':
                    require(not published, 'only one final publication attempt')
                    published = True
                    report['pixels_before_publication'] = [hex(value) for value in pixels(debugger, base)]
                else:
                    require(debugger.reg('x0') == 0, 'target harness reported failure')
                    report['record'] = debugger.memory(symbols['native_smoke_result'], 256).hex()
                    report['translation'] = debugger.memory(symbols['native_smoke_translation'], 168).hex()
                    require(debugger.memory(base - GUARD, GUARD) == bytes([0xa5]) * GUARD and
                            debugger.memory(base + PIXEL_BYTES, GUARD) == bytes([0xa5]) * GUARD,
                            'pixel buffer guards')
                    for address in stops:
                        debugger.breakpoint(address, False)
                    try:
                        debugger.command('c')
                    except (EOFError, OSError):
                        pass
                    require(process.wait(timeout=5) == 0, 'QEMU semihost exit status')
                    break
                # Step off each observed breakpoint without replacing any
                # production instruction, then reinstall for later calls.
                debugger.breakpoint(pc, False)
                debugger.command('s')
                debugger.breakpoint(pc)
            else:
                raise AssertionError('too many native smoke operations')
        serial = (case_out / 'serial.log').read_text(errors='replace')
        require('NATIVE_SMOKE_QEMU_PASS' in serial and 'CHECK_FAIL' not in serial, 'target self-checks')
        record_match = re.search(r'^WIRE_RESULT=([0-9a-f]+)$', serial, re.M)
        require(record_match and record_match[1] == report['record'], 'serial and observed actual record match')
        receipt = re.search(r'^WIRE_FDT=([0-9a-f]+)$', serial, re.M)
        require((not publication and receipt and receipt[1] == report['record']) or
                (publication and not receipt), 'actual FDT receipt visibility')
        translation_receipt = re.search(r'^WIRE_FDT_XLAT=([0-9a-f]+)$', serial, re.M)
        require((not publication and translation_receipt and translation_receipt[1] == report['translation']) or
                (publication and not translation_receipt), 'actual companion FDT receipt visibility')
        report['decoded'] = decoder.decode(bytes.fromhex(report['record']), f'{TEST_ID:016x}',
                                            bytes.fromhex(report['translation']))
        expected_verdict = 'BLOCKED_CONTEXT' if fault else 'BLOCKED_EVIDENCE' if publication else \
                           'REFUSED' if geometry else 'SMOKE_TRACE_VALID'
        require(report['decoded']['verdict'] == expected_verdict, 'independent decoder verdict')
        frames = report['frame_calls']
        delays = report['delays']
        expected_worlds = [0, 1, 0] if geometry == 0 and fault == 0 else [0]
        require([frame['world'] for frame in frames] == expected_worlds and
                all(frame['state'] == 3 for frame in frames), 'actual A/B/A calls or stop after first refusal')
        expected_delays = 1 if fault == 3 else 3 if geometry == 0 and not fault else 0
        require(len(delays) == expected_delays and all(delay['microseconds'] == 750000 for delay in delays),
                'real observation delays; no delay after refusal or failed restoration')
        active, inactive = '0x207dd7', '0x2d3741'
        for index, delay in enumerate(delays):
            wanted = [active, inactive] if index % 2 == 0 else [inactive, active]
            require([delay['left'], delay['right']] == wanted, 'actual A/B/A pixels after cache publication')
        if geometry or fault == 1:
            require(report['pixels_before_publication'] == ['0x101820', '0x101820'],
                    'refusal and PAR restore failure perform no drawing')
        if fault in (2, 3):
            require(report['pixels_before_publication'] == [active, inactive],
                    'state failure produces no additional drawing')
        require(published, 'actual libfdt publication function executed')
        report['status'] = 'PASS'
        print(f'PASS {name}: fb calls={len(frames)}; real delays={len(delays)}', flush=True)
    except Exception:
        report['status'] = 'FAIL'
        report['failure'] = traceback.format_exc()
        raise
    finally:
        if case_out.exists():
            (case_out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True, help='fresh output directory')
    parser.add_argument('--qemu', required=True)
    parser.add_argument('--qemu-lib-dir', default='')
    parser.add_argument('--cc', default='clang')
    parser.add_argument('--ld', default='aarch64-elf-ld')
    parser.add_argument('--objdump', default='aarch64-elf-objdump')
    parser.add_argument('--nm', default='aarch64-elf-nm')
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--reuse-build', action='store_true', help='resume a fixture build while developing tests')
    parser.add_argument('--attribute', choices=['nc', 'nc-xs0', 'wb', 'device'], default='nc')
    parser.add_argument('--case', default='all', choices=['all', 'success', 'geometry', 'unmapped',
        'missing-slot', 'reservation-failed', 'par-restore-failed', 'daif-restore-failed', 'hcr-changed',
        'table-owner-refused', 'table-bounds-refused'])
    args = parser.parse_args()
    out = args.out.resolve()
    summary = {'test': 'actual native smoke runner + real libfdt in diskless single-PE QEMU TCG',
               'status': 'RUNNING', 'test_build': f'{TEST_ID:016x}', 'cases': []}
    try:
        out.mkdir(parents=True, exist_ok=args.reuse_build)
        elf = out / 'native-smoke.elf' if args.reuse_build else build(args, out)
        require(elf.exists(), 'native fixture build exists')
        summary['elf_sha256'] = hashlib.sha256(elf.read_bytes()).hexdigest()
        if args.build_only:
            summary['status'] = 'BUILT'
            print('BUILT actual native smoke fixture: ' + str(elf), flush=True)
            return
        symbols = symbols_from((out / 'native-smoke.nm').read_text())
        original = elf.read_bytes()
        disassembly = (out / 'native-smoke.dis').read_text()
        cases = [('success', 0, 0, 0), ('geometry', 1, 0, 0), ('unmapped', 2, 0, 0),
                 ('missing-slot', 0, 1, 0), ('reservation-failed', 1, 2, 0),
                 ('par-restore-failed', 0, 0, 1), ('daif-restore-failed', 0, 0, 2),
                 ('hcr-changed', 0, 0, 3)]
        cases += [('table-owner-refused', 4, 0, 0), ('table-bounds-refused', 5, 0, 0)]
        if args.attribute in ('wb', 'device'):
            require(args.case == 'all', 'attribute refusal uses its own single control case')
            cases = [('attribute-refused', 3, 0, 0)]
        for name, geometry, publication, fault in cases:
            if args.case in ('all', name):
                summary['cases'].append(run_case(args, out, symbols, original, disassembly,
                                                 name, geometry, publication, fault))
        summary['status'] = 'PASS'
        print(f'NATIVE_SMOKE_INTEGRATION_PASS: {len(summary["cases"])} actual runner cases; no hardware', flush=True)
    except Exception:
        summary['status'] = 'FAIL'
        summary['failure'] = traceback.format_exc()
        raise
    finally:
        if out.exists():
            (out / 'native-smoke-result.json').write_text(json.dumps(summary, indent=2) + '\n')


if __name__ == '__main__':
    main()
