#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build/package an opt-in smoke image from an unchanged source snapshot. Never installs it."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-build', type=Path, required=True)
    parser.add_argument('--rel3-bundle', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    out = args.out.resolve()
    baseline, rel3 = args.baseline_build.resolve(), args.rel3_bundle.resolve()
    if out == repo or repo in out.parents:
        parser.error('output must be outside the source checkout')
    if any(out == origin or out in origin.parents or origin in out.parents for origin in [baseline, rel3]):
        parser.error('output must be separate from immutable input packages')
    out.mkdir(parents=True, exist_ok=False)
    logs = out / 'logs'
    logs.mkdir()
    spec = importlib.util.spec_from_file_location('smoke_verify', Path(__file__).with_name('verify_visual_smoke.py'))
    verify = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verify)
    try:
        old, frozen = verify.baseline_inputs(baseline)
        rel3_manifest, parts = verify.rel3_inputs(rel3)
        inputs = verify.source_hashes(repo)
        source = out / 'source'
        shutil.copytree(repo, source, ignore=shutil.ignore_patterns(*verify.IGNORED))
        verify.require(verify.source_hashes(source) == inputs, 'snapshot differs from live inputs')
        verify.require(verify.source_hashes(repo) == inputs, 'live inputs changed during snapshot')
        (source / 'build').mkdir()
        for name, fact in frozen.items():
            shutil.copy2(fact['origin'], source / 'build' / name)
        build = verify.build_identity(inputs, frozen)
        (source / 'build/e0_visual_build.h').write_text(
            '#define E0_VISUAL_BUILD_ID UINT64_C(0x' + build + ')\n')
        provenance = {'schema': verify.SCHEMA, 'build_id': build, 'source_origin': str(repo),
                      'source': inputs, 'source_transformations': [], 'baseline_build': str(baseline),
                      'baseline_build_id': old['build_id'],
                      'baseline_manifest_sha256': verify.BASELINE_MANIFEST_SHA,
                      'baseline_source_changes': verify.source_changes(old, inputs),
                      'complete_baseline_source_comparison': verify.complete_source_provenance(baseline, inputs),
                      'frozen_inputs': frozen, 'rel3_bundle': str(rel3), 'cfg': verify.CFG,
                      'translation_required': True, 'trace_formats': {'E0VS': 256, 'E0XT': 168}}
        (out / 'source-manifest.json').write_text(json.dumps(provenance, indent=2) + '\n')
        shutil.copy2(baseline / 'source-manifest.json', out / 'frozen-baseline-source-manifest.json')
        command = verify.BUILD_COMMAND
        env = os.environ.copy()
        env['M1N1_VERSION_TAG'] = '6boot-fb-smoke-' + build
        (out / 'command.json').write_text(json.dumps(
            {'command': command, 'cwd': str(source), 'M1N1_VERSION_TAG': env['M1N1_VERSION_TAG']}, indent=2) + '\n')
        reproduce = [sys.executable, '-I', '-B', str(repo / 'experiments/el2-control/e0/build_visual_smoke.py'),
                     '--baseline-build', str(baseline), '--rel3-bundle', str(rel3), '--out', str(out) + '-reproduced']
        check = [sys.executable, '-I', '-B', str(repo / 'experiments/el2-control/e0/verify_visual_smoke.py'),
                 str(out), '--rel3', str(rel3), '--baseline-build', str(baseline), '--source', str(repo)]
        (out / 'reproduce.sh').write_text('#!/bin/sh\nset -eu\n' + shlex.join(reproduce) + '\n'
                                         + '# Verify this package:\n' + shlex.join(check) + '\n')
        with (logs / 'toolchain-versions.txt').open('w') as log:
            for tool in ['gmake', 'clang', 'aarch64-elf-ld', 'aarch64-elf-objcopy',
                         'aarch64-elf-objdump', 'aarch64-elf-nm', 'cargo', 'rustc', 'fdtput']:
                log.write(tool + ' --version\n')
                log.flush()
                subprocess.run([tool, '--version'], stdout=log, stderr=subprocess.STDOUT, check=True)
        with (logs / 'build.log').open('w') as log:
            log.write(shlex.join(command) + '\n')
            log.flush()
            result = subprocess.run(command, cwd=source, env=env, stdout=log, stderr=subprocess.STDOUT)
        verify.require(result.returncode == 0, 'native smoke build failed; inspect ' + str(logs / 'build.log'))
        verify.require(verify.source_hashes(source) == inputs, 'build modified source snapshot')
        verify.require(verify.source_hashes(repo) == inputs, 'live source changed during build')
        tree = out / 'j713-rel3-fb-smoke.dtb'
        tree.write_bytes(parts['j713-rel3.dtb'])
        with (logs / 'dt.log').open('w') as log:
            subprocess.run(['fdtput', '-t', 's', str(tree), '/chosen', '6boot,e0-visual-mode', 'smoke'],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        verify.verify_smoke_dt(parts['j713-rel3.dtb'], tree.read_bytes())
        stage = out / 'm1n1-fb-smoke.bin'
        shutil.copy2(source / 'build/m1n1.bin', stage)
        cmdline = verify.smoke_cmdline(rel3_manifest['cmdline_base'], build)
        image = out / 'boot-rel3-fb-smoke.bin'
        image.write_bytes(stage.read_bytes() + b'chosen.bootargs=' + cmdline.encode() + b'\n'
                          + tree.read_bytes() + parts['Image.gz'] + parts['initramfs-p2t.bin'])
        tools = verify.TOOLS
        elf = source / 'build/m1n1-raw.elf'
        for name, tool_command in [('stage.dis', [tools['objdump'], '-d', str(elf)]),
                                   ('stage.nm', [tools['nm'], str(elf)])]:
            with (logs / name).open('w') as log:
                subprocess.run(tool_command, stdout=log, stderr=subprocess.STDOUT, check=True)
        artifacts = [out / name for name in sorted(verify.ARTIFACTS)]
        manifest = {**provenance, 'scope': 'FRAMEBUFFER ONLY; NO STAGE2 / NO EL1 WORLD EXECUTION',
                    'physical_status': 'PENDING PHYSICAL TEST', 'build_command': command,
                    'tools': tools, 'cmdline_original': rel3_manifest['cmdline_base'], 'cmdline': cmdline,
                    'removed_bootargs': ['x6e3.esplog'], 'rel3_kernel_sha256': verify.REL3_SHA['Image.gz'],
                    'rel3_initramfs_sha256': verify.REL3_SHA['initramfs-p2t.bin'],
                    'original_rel3_sha256': verify.REL3_SHA['boot-rel3-original.bin'],
                    'artifacts': {str(path.relative_to(out)): {'sha256': verify.file_sha(path), 'bytes': path.stat().st_size}
                                  for path in artifacts}}
        (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        verify.verify(out, rel3, baseline, repo)
        print(json.dumps({'bundle': str(out), 'build_id': build,
                          'image_sha256': verify.file_sha(image), 'status': 'SMOKE_PACKAGE_PASS',
                          'physical_status': 'PENDING PHYSICAL TEST'}, indent=2))
    except Exception as error:
        (out / 'failure.json').write_text(json.dumps({'error': str(error), 'type': type(error).__name__,
                                                     'logs': str(logs)}, indent=2) + '\n')
        raise


if __name__ == '__main__':
    main()
