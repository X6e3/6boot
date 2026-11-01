#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify an uninstalled smoke package against its live, unmodified source inputs."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile

SCHEMA = 2
CFG = ['FB_SILENT_MODE', 'SIXBOOT_USER', 'SIXBOOT_E0_VISUAL', 'SIXBOOT_E0_FB_SMOKE']
IGNORED = {'.git', 'build', 'out', '__pycache__'}
TOOLS = {'objdump': 'aarch64-elf-objdump', 'nm': 'aarch64-elf-nm', 'objcopy': 'aarch64-elf-objcopy'}
BUILD_COMMAND = ['gmake', '-j6', 'USE_CLANG=1', 'TOOLCHAIN=/usr/bin/', 'LLDDIR=/usr/bin/',
                 'CC=clang --target=aarch64-none-elf', 'AS=clang --target=aarch64-none-elf',
                 'LD=aarch64-elf-ld', 'OBJCOPY=aarch64-elf-objcopy', 'CARGO_FLAGS=--locked',
                 'EXTRA_CFLAGS=-fstack-usage', 'CFG=' + ' '.join(CFG), 'build/m1n1.bin']
ARTIFACTS = {
    'boot-rel3-fb-smoke.bin', 'j713-rel3-fb-smoke.dtb', 'm1n1-fb-smoke.bin',
    'source/build/m1n1-raw.elf', 'source/build/m1n1.bin', 'source/build/e0_visual_build.h',
    'source/build/build_cfg.h', 'source/build/build_tag.h', 'logs/stage.dis', 'logs/stage.nm',
    'logs/build.log', 'logs/dt.log', 'logs/toolchain-versions.txt', 'command.json',
    'source-manifest.json', 'reproduce.sh', 'frozen-baseline-source-manifest.json',
}
BASELINE_ID = 'c625f8e729d0369a'
BASELINE_MANIFEST_SHA = '67e3ff8935131f950c73038cee761a95a2fda27f4cbe33d20ce9b28c59b8b83a'
FROZEN_SHA = {
    'entry_probe_image.h': '5a94ba77903272c457a13cfed349b3b8337b594c4e6491bd9c03481d1c8061c9',
    'die_blocks.h': '1d43f0ee8c8b1592b41b38a8db028fef93672b5687d3a78c9d2e7e12db70355e',
    'probe.bin': 'fdbd3d9b0949a54e2223f3b05a8bf347482d21394b3e4d984ab2c4499d3d5e23',
}
REL3_SHA = {
    'boot-rel3-original.bin': '30e56865cd1c38802be96969e83b27ba803af6506e17f69f892adf78486d21e1',
    'stage2-original.bin': '6896190259533c5b34ea651aa15078e65d77a9235b7671b5badee7e683ee2235',
    'j713-rel3.dtb': '5fd2368f6a79e5e0f8577aeaec080247d999c75e77899748448ff6db5913e598',
    'Image.gz': '16c6344c83c58fbdb87fb96e9bc197ec3f3a86a8fcaaf311f9844ce3a0dae8cf',
    'initramfs-p2t.bin': '6a24c599fde9a9c60cde63a81980d7f9dd27789685f14a6696bbe77cff684821',
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def file_sha(path):
    return sha(path.read_bytes())


def source_hashes(root):
    """The same complete source-file selection used by the snapshot copy."""
    return {str(path.relative_to(root)): file_sha(path)
            for path in sorted(root.rglob('*'))
            if path.is_file() and not any(part in IGNORED for part in path.relative_to(root).parts)}


def generated_probe_header(blob):
    rows = [', '.join(f'0x{byte:02x}' for byte in blob[i:i + 12])
            for i in range(0, len(blob), 12)]
    return ('/* Generated from entry-probe/ by tools/build-j713.sh. Do not edit. */\n'
            'static const u8 entry_probe_image[] __attribute__((aligned(16))) = {\n    '
            + ',\n    '.join(rows) + ',\n};\n'
            + f'static const char entry_probe_image_sha256[] = "{sha(blob)}";\n').encode()


def baseline_inputs(baseline):
    """Authenticate frozen inputs, without requiring current source to be c625."""
    manifest_path = baseline / 'source-manifest.json'
    require(file_sha(manifest_path) == BASELINE_MANIFEST_SHA, 'frozen baseline manifest hash')
    manifest = json.loads(manifest_path.read_text())
    require(manifest['build_id'] == BASELINE_ID, 'frozen baseline identity')
    for layout in ['source', 'control-source']:
        for name, digest in manifest['source'].items():
            require(file_sha(baseline / layout / name) == digest,
                    f'frozen baseline source provenance: {layout}/{name}')
    blob = (baseline / 'probe.bin').read_bytes()
    require(sha(blob) == FROZEN_SHA['probe.bin'], 'frozen P3 artifact hash')
    result = {}
    for name in ['entry_probe_image.h', 'die_blocks.h']:
        origin = baseline / 'control-source/build' / name
        require(file_sha(origin) == FROZEN_SHA[name], f'frozen {name} hash')
        require((baseline / 'source/build' / name).read_bytes() == origin.read_bytes(),
                f'frozen {name} native/control origins differ')
        result[name] = {'origin': str(origin), 'sha256': FROZEN_SHA[name],
                        'bytes': origin.stat().st_size}
    require((baseline / 'control-source/build/entry_probe_image.h').read_bytes()
            == generated_probe_header(blob), 'frozen P3 embed differs from probe.bin artifact')
    for layout in ['source', 'control-source']:
        require(file_sha(baseline / layout / 'data/die_blocks.h') == FROZEN_SHA['die_blocks.h'],
                f'frozen die header differs from {layout} data origin')
    return manifest, result


def rel3_inputs(bundle):
    manifest = json.loads((bundle / 'manifest.json').read_text())
    facts = manifest['rel3_inputs']
    original = (bundle / 'boot-rel3-original.bin').read_bytes()
    require(sha(original) == REL3_SHA['boot-rel3-original.bin']
            == facts['boot-rel3-original.bin']['sha256'], 'pinned REL3 image hash')
    require(len(original) == facts['boot-rel3-original.bin']['bytes'], 'REL3 image length')
    cursor = facts['stage2-original.bin']['bytes']
    parts = {'stage2-original.bin': original[:cursor]}
    prefix = b'chosen.bootargs=' + manifest['cmdline_base'].encode() + b'\n'
    require(original[cursor:cursor + len(prefix)] == prefix, 'original REL3 bootargs')
    cursor += len(prefix)
    for name in ['j713-rel3.dtb', 'Image.gz', 'initramfs-p2t.bin']:
        size = facts[name]['bytes']
        parts[name] = original[cursor:cursor + size]
        cursor += size
    require(cursor == len(original), 'REL3 reconstruction length')
    for name, data in parts.items():
        require(sha(data) == REL3_SHA[name] == facts[name]['sha256'], f'pinned REL3 {name}')
        require(len(data) == facts[name]['bytes'], f'REL3 {name} length')
    return manifest, parts


def smoke_cmdline(original, build_id):
    tokens = original.split(' ')
    require(all(tokens) and tokens.count('x6e3.esplog') == 1, 'expected one exact esplog token')
    return ' '.join(token for token in tokens if token != 'x6e3.esplog') + ' x6e3.fbsmoke=' + build_id


def build_identity(source, frozen):
    identity = {'source': source, 'frozen_inputs': {name: fact['sha256']
                for name, fact in frozen.items()}, 'cfg': CFG,
                'baseline_manifest_sha256': BASELINE_MANIFEST_SHA,
                'rel3_sha256': REL3_SHA['boot-rel3-original.bin']}
    return sha(json.dumps(identity, sort_keys=True).encode())[:16]


def fdt_contents(data):
    """Compare nodes, properties, reservations and semantic header fields."""
    header = struct.unpack_from('>10I', data)
    require(header[0] == 0xd00dfeed and header[1] == len(data), 'FDT header/length')
    cursor = header[2]
    strings = data[header[3]:header[3] + header[8]]
    reserve_cursor = header[4]
    reservations = []
    while True:
        entry = struct.unpack_from('>QQ', data, reserve_cursor)
        reserve_cursor += 16
        if entry == (0, 0):
            break
        reservations.append(entry)
    stack, nodes, properties = [], [], {}
    while True:
        require(cursor + 4 <= header[2] + header[9], 'FDT structure bounds')
        token = struct.unpack_from('>I', data, cursor)[0]
        cursor += 4
        if token == 1:
            end = data.index(0, cursor)
            stack.append(data[cursor:end].decode())
            nodes.append('/'.join(stack))
            cursor = (end + 4) & ~3
        elif token == 2:
            stack.pop()
        elif token == 3:
            size, offset = struct.unpack_from('>II', data, cursor)
            cursor += 8
            name = strings[offset:strings.index(0, offset)].decode()
            key = '/'.join(stack) + '/' + name
            require(key not in properties and cursor + size <= header[2] + header[9],
                    'FDT property bounds/uniqueness')
            properties[key] = data[cursor:cursor + size]
            cursor = (cursor + size + 3) & ~3
        elif token == 4:
            pass
        elif token == 9:
            break
        else:
            raise ValueError('invalid FDT token')
    require(not stack, 'FDT node balance')
    return {'nodes': nodes, 'properties': properties, 'reservations': reservations,
            'version': header[5:8]}


def verify_smoke_dt(original, changed):
    before, after = fdt_contents(original), fdt_contents(changed)
    key = '/chosen/6boot,e0-visual-mode'
    require(key not in before['properties'], 'original DT already has smoke mode')
    require(after['properties'].pop(key, None) == b'smoke\0', 'DT smoke property')
    require(after == before, 'DT changes beyond the smoke property')


def source_changes(baseline, source):
    return {name: {'baseline_sha256': digest, 'current_sha256': source.get(name)}
            for name, digest in baseline['source'].items() if source.get(name) != digest}


def complete_source_provenance(baseline, current):
    original = source_hashes(baseline / 'control-source')
    return {'origin': str(baseline / 'control-source'), 'files': len(original),
            'inventory_sha256': sha(json.dumps(original, sort_keys=True).encode()),
            'differences': {name: {'baseline_sha256': original.get(name),
                                   'current_sha256': current.get(name)}
                            for name in sorted(original.keys() | current.keys())
                            if original.get(name) != current.get(name)}}


def verify(bundle, rel3, baseline=None, live_source=None):
    manifest = json.loads((bundle / 'manifest.json').read_text())
    require(manifest['schema'] == SCHEMA, 'smoke manifest schema')
    require(manifest['scope'] == 'FRAMEBUFFER ONLY; NO STAGE2 / NO EL1 WORLD EXECUTION', 'smoke scope')
    require(manifest.get('translation_required') is True and
            manifest.get('trace_formats') == {'E0VS': 256, 'E0XT': 168}, 'required translation trace contract')
    require(manifest['physical_status'] == 'PENDING PHYSICAL TEST', 'physical test status')
    baseline = baseline or Path(manifest['baseline_build'])
    live_source = live_source or Path(manifest['source_origin'])
    require(bundle != live_source.resolve() and bundle not in live_source.resolve().parents,
            'live source must be external to the package')
    old, frozen = baseline_inputs(baseline)
    require(manifest['frozen_inputs'] == frozen, 'frozen input provenance')
    require(manifest['baseline_build_id'] == old['build_id'], 'baseline identity provenance')
    require(manifest['baseline_manifest_sha256'] == BASELINE_MANIFEST_SHA, 'baseline manifest pin')
    require(manifest['source'] == source_hashes(bundle / 'source'), 'source snapshot hashes/file set')
    require(manifest['source'] == source_hashes(live_source), 'snapshot differs from live source inputs')
    require(manifest['baseline_source_changes'] == source_changes(old, manifest['source']),
            'original/current source provenance')
    require(manifest['complete_baseline_source_comparison']
            == complete_source_provenance(baseline, manifest['source']), 'complete original/current source provenance')
    require(manifest['source_transformations'] == [], 'source transformations are forbidden')
    source_manifest = json.loads((bundle / 'source-manifest.json').read_text())
    require(all(manifest.get(key) == value for key, value in source_manifest.items()),
            'source manifest/provenance agreement')
    require(file_sha(bundle / 'frozen-baseline-source-manifest.json') == BASELINE_MANIFEST_SHA,
            'archived frozen baseline manifest')
    require(not (bundle / 'smoke-hook.patch').exists(), 'obsolete source patch artifact')
    require(manifest['cfg'] == CFG, 'smoke CFG')
    require(manifest['build_id'] == build_identity(manifest['source'], frozen), 'source build identity')
    require(set(manifest['artifacts']) == ARTIFACTS, 'complete artifact hash inventory')
    for name, fact in manifest['artifacts'].items():
        path = bundle / name
        require(path.is_relative_to(bundle) and '..' not in Path(name).parts, 'artifact path')
        require(path.stat().st_size == fact['bytes'] and file_sha(path) == fact['sha256'],
                'artifact hash/length: ' + name)
    for name, fact in frozen.items():
        require(file_sha(bundle / 'source/build' / name) == fact['sha256'], 'copied frozen ' + name)
    build = manifest['build_id']
    require((bundle / 'source/build/e0_visual_build.h').read_text()
            == '#define E0_VISUAL_BUILD_ID UINT64_C(0x' + build + ')\n', 'generated build ID header')
    require((bundle / 'source/build/build_cfg.h').read_text()
            == ''.join('#define ' + flag + '\n' for flag in CFG), 'compiled smoke CFG')
    command = json.loads((bundle / 'command.json').read_text())
    require(command['command'] == manifest['build_command'] == BUILD_COMMAND, 'recorded build command')
    require(command['command'].count('CFG=' + ' '.join(CFG)) == 1, 'command CFG')
    require(command['M1N1_VERSION_TAG'] == '6boot-fb-smoke-' + build, 'build version tag')
    require('SIXBOOT_EL2_NATIVE' not in ' '.join(command['command']), 'native CFG leaked')
    require(not list((bundle / 'source/build').glob('el2_native*.o')), 'native objects leaked')
    require((bundle / 'source/build/build_tag.h').read_text()
            == '#define BUILD_TAG "6boot-fb-smoke-' + build + '"\n', 'compiled version tag')
    rel3_manifest, parts = rel3_inputs(rel3)
    require(manifest['original_rel3_sha256'] == REL3_SHA['boot-rel3-original.bin'], 'REL3 anchor')
    require(manifest['rel3_kernel_sha256'] == REL3_SHA['Image.gz'], 'kernel pin')
    require(manifest['rel3_initramfs_sha256'] == REL3_SHA['initramfs-p2t.bin'], 'initramfs pin')
    require(manifest['cmdline_original'] == rel3_manifest['cmdline_base'], 'original bootargs provenance')
    require(manifest['cmdline'] == smoke_cmdline(rel3_manifest['cmdline_base'], build),
            'only exact esplog removal plus smoke tag is permitted')
    require(manifest['removed_bootargs'] == ['x6e3.esplog'], 'removed bootargs provenance')
    newdt = (bundle / 'j713-rel3-fb-smoke.dtb').read_bytes()
    verify_smoke_dt(parts['j713-rel3.dtb'], newdt)
    stage = (bundle / 'm1n1-fb-smoke.bin').read_bytes()
    require(stage == (bundle / 'source/build/m1n1.bin').read_bytes(), 'packaged/built stage identity')
    elf = bundle / 'source/build/m1n1-raw.elf'
    tools = manifest['tools']
    require(tools == TOOLS, 'artifact inspection tools')
    # GNU objcopy cannot use a pipe-backed /dev/stdout on macOS. Its only
    # verification output is temporary; the package and input checkout stay read-only.
    with tempfile.TemporaryDirectory(prefix='6boot-e0-verify-') as temporary:
        binary = Path(temporary) / 'stage.bin'
        subprocess.run([tools['objcopy'], '-O', 'binary', '--strip-debug',
                        str(elf), str(binary)], check=True, capture_output=True)
        require(binary.read_bytes() == stage, 'ELF/stage binary identity')
    require((bundle / 'boot-rel3-fb-smoke.bin').read_bytes()
            == stage + b'chosen.bootargs=' + manifest['cmdline'].encode() + b'\n'
            + newdt + parts['Image.gz'] + parts['initramfs-p2t.bin'], 'assembled REL3 image')
    dis = subprocess.check_output([tools['objdump'], '-d', str(elf)], text=True)
    require(dis == (bundle / 'logs/stage.dis').read_text(), 'actual ELF disassembly evidence')
    symbols = subprocess.check_output([tools['nm'], str(elf)], text=True)
    require(symbols == (bundle / 'logs/stage.nm').read_text(), 'actual ELF symbol evidence')
    require(not re.search(r'\bel2_native_\w+', symbols), 'native EL1/Stage2 adapter symbols leaked')
    functions = dict(re.findall(r'^[0-9a-f]+ <([^>]+)>:\n(.*?)(?=\n\n|\Z)', dis, re.M | re.S))
    smoke_names = [name for name in functions if name.startswith('e0_visual_smoke')]
    require(smoke_names, 'linked smoke API missing')
    for name in smoke_names + ['fb_e0_frame', 'dc_cvac_range', 'mmu_e0_snapshot']:
        require(name in functions, 'linked framebuffer helper missing: ' + name)
        code = functions[name]
        allowed = {'daif', 'par_el1'} if name == 'fb_e0_frame' else set()
        for register in re.findall(r'\bmsr\s+([^,\s]+)', code):
            require(register in allowed, f'{name} unexpected system-register write: {register}')
        require(not re.search(r'\b(?:hvc|eret|tlbi)\b', code), 'control transfer/TLBI in ' + name)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    parser.add_argument('--rel3', type=Path, required=True)
    parser.add_argument('--baseline-build', type=Path)
    parser.add_argument('--source', type=Path, help='live input checkout (defaults to recorded origin)')
    parser.add_argument('--selected-image', type=Path, help='read-only hash check of the exact file selected for boot')
    args = parser.parse_args()
    manifest = verify(args.bundle.resolve(), args.rel3.resolve(), args.baseline_build, args.source)
    if args.selected_image:
        require(file_sha(args.selected_image) == manifest['artifacts']['boot-rel3-fb-smoke.bin']['sha256'],
                'selected boot image does not match this build')
        print('SELECTED_IMAGE_MATCH: ' + str(args.selected_image))
    print('SMOKE_PACKAGE_PASS: live/source snapshot identity; frozen P3/die provenance; actual ELF/artifact hashes; '
          'exact REL3 kernel/initramfs; only smoke DT property; exact esplog removal; CFG; no native gate')
    print('PENDING PHYSICAL TEST: RAM pixel/cache checks do not prove J713 scanout visibility')


if __name__ == '__main__':
    main()
