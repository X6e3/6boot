#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regression checks for smoke input provenance, exact bootargs and package rejection."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile

spec = importlib.util.spec_from_file_location('smoke_verify', Path(__file__).with_name('verify_visual_smoke.py'))
v = importlib.util.module_from_spec(spec)
spec.loader.exec_module(v)


def rejected(action, expected):
    try:
        action()
    except (ValueError, FileNotFoundError) as error:
        v.require(expected in str(error), 'unexpected rejection: ' + str(error))
    else:
        raise AssertionError('accepted invalid input: ' + expected)


def link_file(origin, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    target.symlink_to(origin)


def independent_file(path, data):
    if path.is_symlink():
        path.unlink()
    path.write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-build', type=Path, required=True)
    parser.add_argument('--rel3', type=Path, required=True)
    parser.add_argument('--bundle', type=Path, help='also test a completed native smoke package')
    args = parser.parse_args()
    baseline, rel3 = args.baseline_build.resolve(), args.rel3.resolve()
    original, frozen = v.baseline_inputs(baseline)
    rel3_manifest, parts = v.rel3_inputs(rel3)
    checks = ['frozen-baseline-and-header-origins', 'pinned-rel3-components']
    changed_source = dict(original['source'])
    changed_source['Makefile'] = '0' * 64
    v.require(v.source_changes(original, changed_source)['Makefile'] == {
        'baseline_sha256': original['source']['Makefile'], 'current_sha256': '0' * 64},
        'current edits must be recorded instead of rejected as baseline drift')
    checks.append('authorized-current-source-delta-recorded')
    build_id = '0123456789abcdef'
    command = v.smoke_cmdline(rel3_manifest['cmdline_base'], build_id)
    before = rel3_manifest['cmdline_base'].split(' ')
    v.require(command.split(' ')[:-1] == [token for token in before if token != 'x6e3.esplog'],
              'bootarg values/order changed')
    v.require('root=UUID=f4b9e2cd-2ba7-466d-8e45-531c176cb05f' in command.split(' '),
              'root UUID changed')
    sample = 'a x6e3.esplog=1 x6e3.esplog x6e3.esplog.extra root=UUID=original z'
    v.require(v.smoke_cmdline(sample, build_id) ==
              'a x6e3.esplog=1 x6e3.esplog.extra root=UUID=original z x6e3.fbsmoke=' + build_id,
              'esplog removal matched other tokens')
    rejected(lambda: v.smoke_cmdline('a x6e3.esplog x6e3.esplog z', build_id), 'one exact esplog')
    checks.append('exact-esplog-token-and-root-order')
    with tempfile.TemporaryDirectory(prefix='6boot-e0-build-test-') as temporary:
        root = Path(temporary)
        frozen_copy = root / 'frozen'
        required = {'source-manifest.json', 'probe.bin'}
        for layout in ['source', 'control-source']:
            required.update(layout + '/' + name for name in original['source'])
            required.update(layout + '/build/' + name for name in frozen)
            required.add(layout + '/data/die_blocks.h')
        for name in required:
            link_file(baseline / name, frozen_copy / name)
        header = frozen_copy / 'control-source/build/entry_probe_image.h'
        independent_file(header, header.read_bytes() + b'\n/* changed */\n')
        rejected(lambda: v.baseline_inputs(frozen_copy), 'frozen entry_probe_image.h hash')
        checks.append('changed-frozen-p3-header-rejected')
        tree = root / 'smoke.dtb'
        tree.write_bytes(parts['j713-rel3.dtb'])
        subprocess.run(['fdtput', '-t', 's', str(tree), '/chosen', '6boot,e0-visual-mode', 'smoke'], check=True)
        v.verify_smoke_dt(parts['j713-rel3.dtb'], tree.read_bytes())
        checks.append('smoke-property-only-accepted')
        subprocess.run(['fdtput', '-c', str(tree), '/e0-unexpected-empty-node'], check=True)
        rejected(lambda: v.verify_smoke_dt(parts['j713-rel3.dtb'], tree.read_bytes()), 'DT changes beyond')
        checks.append('extra-empty-dt-node-rejected')
        if args.bundle:
            bundle = args.bundle.resolve()
            manifest = v.verify(bundle, rel3, baseline)
            checks.append('completed-package-accepted')
            for case in ['source', 'native-cfg', 'missing-artifact', 'artifact-bytes', 'live-source']:
                fixture = root / case
                fixture.mkdir()
                for path in bundle.iterdir():
                    if path.name not in {'manifest.json', 'source-manifest.json'}:
                        (fixture / path.name).symlink_to(path, target_is_directory=path.is_dir())
                altered = json.loads(json.dumps(manifest))
                source_manifest = json.loads((bundle / 'source-manifest.json').read_text())
                source_origin = None
                if case == 'source':
                    altered['source']['Makefile'] = '0' * 64
                    expected = 'source snapshot hashes'
                elif case == 'native-cfg':
                    altered['cfg'].append('SIXBOOT_EL2_NATIVE')
                    source_manifest['cfg'] = altered['cfg']
                    expected = 'smoke CFG'
                elif case == 'missing-artifact':
                    del altered['artifacts']['source/build/m1n1-raw.elf']
                    expected = 'complete artifact hash inventory'
                elif case == 'artifact-bytes':
                    independent_file(fixture / 'm1n1-fb-smoke.bin', b'changed artifact')
                    expected = 'artifact hash/length'
                else:
                    source_origin = root / 'changed-live-source'
                    source_origin.mkdir()
                    (source_origin / 'Makefile').write_text('changed live source\n')
                    expected = 'snapshot differs from live source'
                (fixture / 'manifest.json').write_text(json.dumps(altered))
                source_manifest_raw = (json.dumps(source_manifest, indent=2) + '\n').encode()
                (fixture / 'source-manifest.json').write_bytes(source_manifest_raw)
                # Preserve the artifact fact for the original source manifest unless
                # the tested CFG change intentionally updates this provenance file.
                if case == 'native-cfg':
                    altered['artifacts']['source-manifest.json'] = {
                        'sha256': v.sha(source_manifest_raw), 'bytes': len(source_manifest_raw)}
                    (fixture / 'manifest.json').write_text(json.dumps(altered))
                rejected(lambda: v.verify(fixture, rel3, baseline, source_origin), expected)
                checks.append(case + '-package-rejected')
    print(json.dumps({'status': 'BUILD_TOOLING_TEST_PASS', 'checks': checks, 'count': len(checks)}, indent=2))


if __name__ == '__main__':
    main()
