#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Execute only the own E0 ELF on diskless QEMU virt, never J713."""
import argparse
import json
import os
from pathlib import Path
import resource
import subprocess
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, required=True)
p.add_argument('--qemu', required=True)
p.add_argument('--lib', default='')
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=False)
cmd = [a.qemu, '-machine', 'virt,virtualization=on,secure=off,gic-version=3',
       '-cpu', 'max', '-accel', 'tcg', '-smp', '1', '-m', '128M', '-nodefaults',
       '-display', 'none', '-serial', 'none', '-monitor', 'none', '-nic', 'none',
       '-semihosting-config', 'enable=on,target=native', '-device',
       'loader,file='+str((a.build/'e0.elf').resolve())+',cpu-num=0']
(a.out/'command.json').write_text(json.dumps(cmd, indent=2)+'\n')
env = os.environ.copy()
if a.lib: env['DYLD_LIBRARY_PATH'] = str(Path(a.lib).resolve())
def limit(): resource.setrlimit(resource.RLIMIT_FSIZE, (4*1024*1024,4*1024*1024))
with (a.out/'serial.log').open('wb') as log:
    try:
        r = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, env=env,
                           timeout=15, preexec_fn=limit)
    except subprocess.TimeoutExpired:
        raise SystemExit('FAIL: E0 QEMU timeout, log retained')
text = (a.out/'serial.log').read_text(errors='replace')
print(text, end='')
if (r.returncode or text.count('E0_CASE_PASS') != 5 or text.count('E0_NEGATIVE_PASS') != 2
    or 'E0_QEMU_PASS' not in text or 'E0_CASE_FAIL' in text or 'UNHANDLED' in text):
    raise SystemExit('FAIL: E0 emulator run; inspect retained serial.log')
print('PASS: QEMU Stage-2 CPU access experiment only; J713 PENDING PHYSICAL TEST')
