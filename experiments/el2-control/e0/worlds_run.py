#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Diskless QEMU A/B/C/D prototype verification. Never loads an image on J713."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, required=True); p.add_argument('--out', type=Path, required=True)
p.add_argument('--qemu', required=True); p.add_argument('--lib', default='')
a = p.parse_args(); a.out.mkdir(parents=True, exist_ok=False)
spec = importlib.util.spec_from_file_location('decoder', Path(__file__).with_name('worlds_decode.py'))
decoder = importlib.util.module_from_spec(spec); spec.loader.exec_module(decoder)
symbols = {}
for line in subprocess.check_output(['aarch64-elf-nm', '-n', str(a.build/'e0.elf')], text=True).splitlines():
    f = line.split()
    if len(f) == 3: symbols[f[2]] = int(f[0], 16)
expect = {'build_id': re.search(r'0x([0-9a-f]{16})', (a.build/'worlds_build.h').read_text())[1],
          'pcs': {key: 0x10000000+symbols[name]-symbols['e0_guest_vectors'] for key, name in
                  [('yield','ew_after_yield'), ('permission','ew_permission_pc'), ('foreign','ew_foreign_pc'), ('done','ew_after_done')]}}
(a.out/'expect.json').write_text(json.dumps(expect, indent=2)+'\n')
cmd = [a.qemu, '-machine', 'virt,virtualization=on,secure=off,gic-version=3', '-cpu', 'max',
       '-accel', 'tcg', '-smp', '1', '-m', '128M', '-nodefaults', '-display', 'none',
       '-serial', 'none', '-monitor', 'none', '-nic', 'none', '-semihosting-config', 'enable=on,target=native',
       '-device', 'loader,file='+str((a.build/'e0.elf').resolve())+',cpu-num=0']
(a.out/'command.json').write_text(json.dumps(cmd, indent=2)+'\n')
env = os.environ.copy()
if a.lib: env['DYLD_LIBRARY_PATH'] = str(Path(a.lib).resolve())
with (a.out/'serial.log').open('wb') as log:
    r = subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=20)
serial = (a.out/'serial.log').read_text(errors='replace')
records = re.findall(r'^E0W1 ([0-9a-f]+)$', serial, re.M)
reports = []
for i, raw in enumerate(records):
    blob = bytes.fromhex(raw); (a.out/f'trace-{i}.bin').write_bytes(blob)
    report = decoder.decode(blob, expect); reports.append(report)
    (a.out/f'trace-{i}.json').write_text(json.dumps(report, indent=2)+'\n')
    print('E0W_TRACE_PASS', i, 'mode', report['header']['mode'], 'events', len(report['events']))
assert r.returncode == 0 and len(reports) == 5 and serial.count('E0W_CASE_PASS') == 5 and 'E0W_QEMU_PASS' in serial, serial[-2000:]
assert [r['header']['mode'] for r in reports] == [5, 6, 7, 6, 7]
print('E0W_QEMU_PASS: prepare VM0, single/dual/repeated domains, independent trace policy; J713 NOT TESTED')
