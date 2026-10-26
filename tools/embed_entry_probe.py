#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Embed the entry-probe blob in stage 2 as a C array.

    embed_entry_probe.py <entry-probe.bin> <build/entry_probe_image.h>
"""
import hashlib
import sys

blob = open(sys.argv[1], 'rb').read()
sha = hashlib.sha256(blob).hexdigest()
rows = [', '.join(f'0x{b:02x}' for b in blob[i:i + 12]) for i in range(0, len(blob), 12)]
open(sys.argv[2], 'w').write(
    '/* Generated from entry-probe/ by tools/build-j713.sh. Do not edit. */\n'
    'static const u8 entry_probe_image[] __attribute__((aligned(16))) = {\n    '
    + ',\n    '.join(rows) + ',\n};\n'
    f'static const char entry_probe_image_sha256[] = "{sha}";\n')
print('entry probe', len(blob), 'bytes', sha)
