#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Capture this boot's framebuffer smoke evidence to a NEW X63-R folder.

Linux, Python 3 standard library. Uses only an already mounted X63-R; never
mounts, installs, accesses an ESP, changes boot selection, or reboots.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import subprocess
import uuid


def _module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


HERE = Path(__file__).resolve().parent
# A portable package includes an unchanged copy of tools/collect_native.py.
HELPER = HERE / 'native_collector.py'
if not HELPER.is_file():
    HELPER = HERE.parent / 'tools' / 'collect_native.py'
native = _module('e0_smoke_native_collector', HELPER)
smoke = _module('e0_smoke_decoder', HERE / 'visual_smoke_decode.py')
bounded = native.bounded
destination_root = native.destination_root


def evaluate(props, manifest, release, cmdline, observed_aba=False):
    """Keep decoder diagnostics and independently bind them to this Linux boot."""
    checks = []
    errors = []

    def check(ok, name):
        checks.append({'check': name, 'pass': bool(ok)})
        if not ok:
            errors.append(name)

    report = {'verdict': 'FAIL', 'physical_visibility': 'PENDING HUMAN OBSERVATION',
              'stage2_test': False, 'normal_continuation': False}
    try:
        report = smoke.decode(props['6boot,e0-visual-result'], manifest['build_id'],
                              props.get('6boot,e0-translation-result'))
    except (KeyError, ValueError, TypeError) as exc:
        errors.append('missing/invalid E0VS evidence: ' + str(exc))
    check(props.get('6boot,e0-visual-mode') == b'smoke\0', 'live DT smoke mode')
    check(all(value in props.get('compatible', b'').split(b'\0')
              for value in (b'apple,j713', b'apple,t8132')), 'live DT identifies J713/T8132')
    check(release.strip() == '7.1.13-x6e3-rel3', 'REL3 kernel release')
    build = manifest.get('build_id', '')
    wanted = manifest.get('cmdline', '').split()
    tokens = cmdline.split()
    check(bool(wanted) and tokens == wanted, 'exact packaged bootargs; no duplicate overrides')
    if manifest.get('translation_required'):
        check(bool(report.get('diagnostic_flags', 0) & 256), 'candidate requires live E0XT evidence')
    check([token for token in tokens if token.startswith('x6e3.fbsmoke=')] ==
          ['x6e3.fbsmoke=' + build], 'unambiguous smoke build bootarg')
    p3 = props.get('6boot,p3-result', b'')
    check(len(p3) == 64 and int.from_bytes(p3[:4], 'little') == 0x58505242 and
          int.from_bytes(p3[12:16], 'little') == 0, 'legacy P3 result present and successful')
    report['decoder_verdict'] = report['verdict']
    report['checks'] = report.get('checks', []) + checks
    report['errors'] = report.get('errors', []) + errors
    if errors:
        report['verdict'] = 'FAIL'
        report['normal_continuation'] = False
    if observed_aba and report['verdict'] == 'SMOKE_TRACE_VALID':
        report['physical_visibility'] = 'USER REPORTED A/B/A OBSERVATION'
        report['render_state'] = 'RENDER_COMPLETE_USER_OBSERVED'
    elif observed_aba:
        report['errors'].append('A/B/A observation conflicts with an invalid or refused trace')
        report['verdict'] = 'FAIL'
        report['normal_continuation'] = False
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--destination', help='existing mounted X63-R root (optional discovery)')
    parser.add_argument('--manifest', type=Path, default=HERE / 'manifest.json')
    parser.add_argument('--observed-aba', action='store_true',
                        help='explicit human report of this boot\'s A/B/A; never inferred from frames')
    args = parser.parse_args(argv)
    if platform.system() != 'Linux':
        parser.error('run on J713 Linux after the experimental boot')
    if os.geteuid() != 0:
        parser.error('run with sudo to capture the current boot evidence and kernel log')
    manifest = json.loads(args.manifest.read_text())
    mount = destination_root(args.destination)
    bootid = bounded('/proc/sys/kernel/random/boot_id', 128).decode('ascii').strip()
    if not bootid or len(bootid) > 64 or any(c not in '0123456789abcdef-' for c in bootid):
        raise ValueError('invalid Linux boot ID')
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    out = mount / 'j713-diag' / 'fb-smoke' / (stamp + '-' + bootid[:8] + '-' + uuid.uuid4().hex[:6])
    out.mkdir(parents=True, exist_ok=False)
    capture_errors = []
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')

    def capture(source, name, limit=1024 * 1024):
        try:
            data = bounded(source, limit)
            (out / name).write_bytes(data)
            return data
        except (OSError, ValueError) as exc:
            capture_errors.append(str(exc))
            return b''

    chosen = Path('/sys/firmware/devicetree/base/chosen')
    if not chosen.is_dir():
        chosen = Path('/proc/device-tree/chosen')
    props = {}
    (out / 'chosen').mkdir()
    if chosen.is_dir():
        for prop in sorted(chosen.iterdir()):
            if prop.is_file() and (prop.name.startswith('6boot,') or prop.name == 'bootargs'):
                props[prop.name] = capture(prop, 'chosen/' + prop.name)
    else:
        capture_errors.append('No chosen device-tree node')
    cmdline = capture('/proc/cmdline', 'cmdline.txt', 65536).decode(errors='replace')
    props['compatible'] = capture('/sys/firmware/devicetree/base/compatible', 'compatible.bin', 4096)
    capture('/sys/firmware/fdt', 'firmware-fdt.bin', 2 * 1024 * 1024)
    capture('/proc/version', 'version.txt', 65536)
    release = platform.release()
    (out / 'kernel-release.txt').write_text(release + '\n')
    (out / 'uname.txt').write_text(' '.join(platform.uname()) + '\n')
    (out / 'boot-id.txt').write_text(bootid + '\n')
    try:
        log = subprocess.run(['dmesg', '--color=never'], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, timeout=15)
        (out / 'dmesg.txt').write_bytes(log.stdout[:8 * 1024 * 1024])
        (out / 'dmesg-stderr.txt').write_bytes(log.stderr[:65536])
        if log.returncode or len(log.stdout) > 8 * 1024 * 1024 or len(log.stderr) > 65536:
            capture_errors.append('dmesg failed or truncated')
    except (OSError, subprocess.TimeoutExpired) as exc:
        capture_errors.append(str(exc))
    report = evaluate(props, manifest, release, cmdline, args.observed_aba)
    report.update(capture_errors=capture_errors, boot_id=bootid,
                  evidence_scope='User-run Linux capture; no emulator result substitutes for physical observation.')
    (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    lines = ['Verdict: ' + report['verdict'], 'Boot ID: ' + bootid,
             'Physical visibility: ' + report['physical_visibility'], 'Stage2 test: false',
             'Errors:'] + report['errors'] + ['Capture errors:'] + capture_errors
    (out / 'summary.txt').write_text('\n'.join(lines) + '\n')
    sums = [hashlib.sha256(path.read_bytes()).hexdigest() + '  ' + str(path.relative_to(out))
            for path in sorted(out.rglob('*')) if path.is_file()]
    (out / 'SHA256SUMS').write_text('\n'.join(sums) + '\n')
    fd = os.open(out / 'SHA256SUMS', os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)
    os.sync()
    print(report['verdict'] + ': ' + str(out))
    return 0 if report['verdict'] == 'SMOKE_TRACE_VALID' and not capture_errors else 2


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        raise SystemExit('Capture stopped: ' + str(exc))
