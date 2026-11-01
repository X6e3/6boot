#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Decode E0VS telemetry without asserting physical visibility or Stage2 proof.

Malformed, truncated, unsupported, or wrong-build E0VS records raise ValueError.
Missing or invalid E0XT evidence denies continuation while retaining diagnostics.
A well-formed failure returns its complete diagnostics and a failing verdict.
"""
import argparse
import json
from pathlib import Path
import re
import struct

MAGIC = 0x53563045
PAR_PA = (1 << 52) - (1 << 12)  # 64-bit PAR output address bits 51:12.
SIZES = {1: 80, 2: 256}
ERRORS = {0: 'NONE', 1: 'MODE_OR_CONTEXT', 2: 'RENDERER_REFUSAL', 3: 'HCR_CHANGED'}
OUTCOMES = {1: 'COMPLETE', 2: 'REFUSED', 3: 'FAILED'}
PUBLICATIONS = {0: 'PENDING', 1: 'COMPLETE', 2: 'FAILED'}
DECISIONS = {1: 'CONTINUE', 2: 'BLOCK_CONTEXT', 3: 'BLOCK_EVIDENCE', 4: 'BLOCK_P3'}
REASONS = dict(enumerate(('OK', 'ARGUMENT', 'GEOMETRY', 'RANGE', 'SHADOW', 'FOOTPRINT',
                          'REGIME', 'TRANSLATION', 'IDENTITY', 'ATTRIBUTE', 'PAR_RESTORE',
                          'DAIF_RESTORE', 'HCR_CHANGED', 'MODE', 'P3_FAILED', 'PUBLICATION')))
STAGES = dict(enumerate(('NONE', 'GEOMETRY', 'ADDRESS', 'SHADOW', 'FOOTPRINT', 'REGIME',
                         'TRANSLATE', 'RESTORE', 'DRAW', 'PUBLISH', 'P3')))
DIAGNOSTIC_FLAGS = {1: 'VA_VALID', 2: 'PAR_VALID', 4: 'SNAPSHOT_VALID', 8: 'REGIME_VALID',
                    16: 'DRAW_STARTED', 32: 'PAR_TOUCHED', 64: 'DAIF_TOUCHED', 128: 'UNSAFE',
                    256: 'TRANSLATION_REQUIRED'}
CONTEXT_BITS = {1: 'HCR', 2: 'PAR', 4: 'DAIF', 8: 'SCTLR', 16: 'TCR', 32: 'TTBR0',
                64: 'TTBR1', 128: 'MAIR', 256: 'CURRENT_EL', 512: 'TCR2'}
V2_U32 = ('outcome', 'reason', 'stage', 'safety', 'publication')
V2_DIAG32 = ('failed_frame', 'diagnostic_flags')
V2_U64 = ('failed_va', 'raw_par', 'saved_par', 'restored_par', 'daif_before', 'daif_after',
          'sctlr', 'tcr', 'ttbr0', 'ttbr1', 'mair', 'current_el', 'context_changed',
          'extent', 'cache_line', 'detail')


def _build_id(build):
    if not isinstance(build, str) or not re.fullmatch(r'(?:0x)?[0-9a-fA-F]{16}', build):
        raise ValueError('expected 64-bit hexadecimal build ID')
    return int(build, 16)


def _enum(value, values, label):
    if value not in values:
        raise ValueError('unsupported E0VS ' + label + ': ' + str(value))
    return values[value]


def _translation(report, data, check):
    if data is None or len(data) != 168:
        return check(False, 'required 168-byte E0XT record is present')
    magic, version, size = struct.unpack_from('<IHH', data)
    names = ('build', 'va', 'par', 'root', 'd0', 'd1', 'd2', 'd3', 'pa',
             'isar1', 'mmfr3', 'tcr2', 'mair_alias', 'last_va', 'resolved_mair', 'pfr0')
    t = dict(zip(names, struct.unpack_from('<16Q', data, 8)))
    t.update(zip(('samples', 'page_shift', 'level', 'status', 'attribute', 'flags',
                  'publication', 'publication_error'), struct.unpack_from('<7Ii', data, 136)))
    t.update(par_f=t['par'] & 1, par_attr=t['par'] >> 56, par_sh=(t['par'] >> 7) & 3,
             xs=(t['isar1'] >> 56) & 15, el3=(t['pfr0'] >> 12) & 15,
             tcr2_captured=bool(t['flags'] & 4))
    report['translation'] = t
    ok = check(magic == 0x54583045 and version == 1 and size == 168 and
               t['build'] == int(report['build_id'], 16), 'E0XT identity and build binding')
    ok &= check(t['publication'] == 1 and t['publication_error'] == 0 and
                t['status'] <= 7 and t['flags'] & ~7 == 0 and t['attribute'] <= 4,
                'E0XT publication and known status fields')
    if not t['samples']:
        zero = all(t[name] == 0 for name in names if name != 'build') and all(
            t[name] == 0 for name in ('page_shift', 'level', 'status', 'attribute', 'flags'))
        return bool(ok and check(not report['frames'] and not report['diagnostic_flags'] & 32 and zero,
                                 'no AT attempt has canonical empty E0XT evidence'))
    fb = int(report['framebuffer'], 16)
    height, width, stride = report['height'], report['width'], report['stride']
    metadata = height >= 180 and width >= 400 and stride >= width * 4 and stride * height <= 0xffffffff
    metadata &= fb < 1 << 48 and stride * height <= (1 << 48) - fb
    if not check(metadata and t['page_shift'] == 14 and report['diagnostic_flags'] & 96 == 96,
                 'E0XT sample uses bounded native 16 KiB framebuffer geometry'):
        return False
    start = (fb + ((height - 180) // 2) * stride) & ~16383
    end = fb + ((height - 180) // 2 + 180) * stride
    count = (end - start + 16383) // 16384
    ok &= check(0 < t['samples'] <= count and
                t['last_va'] == start + (t['samples'] - 1) * 16384 and
                t['va'] in (start, t['last_va']), 'E0XT sample bounds and progress')
    if report['reason'] in (7, 8):
        return bool(ok and check(t['va'] == report['failed_va'] and t['par'] == report['raw_par'] and
                                 not t['flags'] & 1, 'fault/identity refusal preserves raw AT evidence'))
    if t['status'] != 1:
        return bool(ok and check(report['reason'] == 6 and t['status'] and report['detail'] == t['status'] and
                                 not t['flags'] & 1 and not t['par_f'] and t['par'] & PAR_PA == t['va'],
                                 'unresolved walk remains a regime refusal'))
    root_mask = 0x0000ffffffffc000
    regime = (t['flags'] & 3 == 3 and t['mmfr3'] & 15 <= 1 and t['tcr2'] == 0 and
              bool(t['flags'] & 4) == (t['mmfr3'] & 15 == 1) and t['xs'] <= 1 and
              (not t['flags'] & 4 or (t['pfr0'] >> 12) & 15 == 0) and
              t['mair_alias'] == report['mair'] and t['root'] == report['ttbr0'] and
              t['root'] != 0 and t['root'] & ~root_mask == 0 and
              report['tcr'] & 63 == 16 and (report['tcr'] >> 14) & 3 == 2 and
              not report['tcr'] & sum(1 << bit for bit in (59, 40, 39, 37, 7)) and t['level'] in (2, 3))
    if not check(regime, 'E0XT owned root, aliases, feature gating and descriptor regime'):
        return False
    descriptors = [t['d0'], t['d1'], t['d2'], t['d3']]
    for desc in descriptors[:t['level']]:
        ok &= check(desc & 3 == 3 and desc & ~(root_mask | 3) == 0 and desc & root_mask != 0,
                    'E0XT table descriptor has supported address and controls')
    leaf = descriptors[t['level']]
    shift = 25 if t['level'] == 2 else 14
    offset = (1 << shift) - 1
    selected = (report['mair'] >> (((leaf >> 2) & 7) * 8)) & 255
    t.update(leaf=leaf, attr_index=(leaf >> 2) & 7, descriptor_sh=(leaf >> 8) & 3,
             descriptor_ap=(leaf >> 6) & 3)
    ok &= check(leaf & 3 == (1 if t['level'] == 2 else 3) and leaf & 1024 and not leaf & 128 and
                (leaf >> 8) & 3 == 2 and leaf & root_mask & offset == 0 and
                leaf & ~(root_mask | 0xfff | (3 << 53)) == 0 and
                (t['level'] != 2 or not t['d3']) and
                t['pa'] == (leaf & root_mask) | (t['va'] & offset) and t['pa'] == t['va'] and
                not t['par_f'] and t['par'] & PAR_PA == t['pa'] & ~0xfff and
                t['resolved_mair'] == selected, 'E0XT leaf permissions, output PA and selected MAIR')
    actual = t['par_attr']
    if (actual == 0x40 or selected == 0x40) and t['xs'] != 1:
        expected = 2
    elif actual not in (0x40, 0x44, 0xff) or selected not in (0x40, 0x44, 0xff):
        expected = 1
    elif (actual == 0xff) != (selected == 0xff) or (actual != 0xff and t['par_sh'] != 2):
        expected = 3
    else:
        expected = 4 if selected == 0xff else 0  # WB is forbidden for native /vram scanout.
    ok &= check(t['attribute'] == expected, 'E0XT effective attribute matches feature/MAIR policy')
    if report['reason'] == 9:
        return bool(ok and check(expected != 0 and t['va'] == report['failed_va'] and
                                 t['par'] == report['raw_par'], 'E0XT explains the attribute refusal'))
    ok &= check(expected == 0, 'E0XT authorizes only native Normal-NC scanout')
    if report['outcome'] == 1:
        ok &= check(t['va'] == start and t['samples'] == count, 'complete scanout covers the full footprint')
    return bool(ok)


def decode(data, build, translation=None):
    expected = _build_id(build)
    if len(data) < 8:
        raise ValueError('truncated E0VS header')
    magic, version, size = struct.unpack_from('<IHH', data)
    if magic != MAGIC:
        raise ValueError('E0VS magic')
    if version not in SIZES:
        raise ValueError('unsupported E0VS version: ' + str(version))
    if size != SIZES[version] or len(data) != size:
        raise ValueError('E0VS version/size/length')
    frames, error = struct.unpack_from('<II', data, 8)
    before, after, ctr, ticks, fb = struct.unpack_from('<5Q', data, 16)
    width, height, stride, depth = struct.unpack_from('<4I', data, 56)
    actual, = struct.unpack_from('<Q', data, 72)
    if actual != expected:
        raise ValueError('E0VS build identity')
    error_name = _enum(error, ERRORS, 'legacy error')
    report = {'verdict': 'FAIL', 'physical_visibility': 'PENDING HUMAN OBSERVATION',
              'stage2_test': False, 'version': version, 'size': size,
              'build_id': f'{actual:016x}', 'frames': frames, 'error': error,
              'error_name': error_name, 'hcr': hex(before), 'hcr_before': hex(before),
              'hcr_after': hex(after), 'ctr': hex(ctr), 'ticks': ticks,
              'framebuffer': hex(fb), 'width': width, 'height': height,
              'stride': stride, 'depth': depth, 'checks': [], 'errors': [],
              'normal_continuation': False}

    def check(ok, name):
        report['checks'].append({'check': name, 'pass': bool(ok)})
        if not ok:
            report['errors'].append(name)
        return bool(ok)

    line = 4 << ((ctr >> 16) & 15)
    extent = stride * height
    context_ok = before == after and not (before & 1)
    metadata_ok = (line >= 64 and fb != 0 and fb % line == 0 and
                   width >= 400 and height >= 180 and stride % 4 == 0 and
                   stride >= width * 4 and (depth & 255) in (30, 32) and
                   extent <= 0xffffffff and fb < (1 << 48) and extent <= (1 << 48) - fb)
    if metadata_ok:
        x0, y0 = (width - 400) // 2, (height - 180) // 2
        end = fb + (y0 + 179) * stride + (x0 + 400) * 4
        metadata_ok = (end + line - 1) // line * line <= fb + extent
    bounded_frames = check(frames <= 3, 'bounded frame count')
    check(context_ok, 'HCR preserved with VM off')
    check(metadata_ok, 'framebuffer and complete cache-line bounds')
    if version == 1:
        # The historical protocol cannot attest PAR/DAIF/host restoration or
        # publication policy; keep those unknown even for a valid old trace.
        report['continuation_evidence'] = 'UNAVAILABLE IN V1'
        check(frames == 3, 'three smoke frames completed')
        check(error == 0, 'legacy smoke error is zero')
        if not report['errors']:
            report['verdict'] = 'SMOKE_TRACE_VALID'
        elif not context_ok:
            report['verdict'] = 'BLOCKED_CONTEXT'
        elif error in (1, 2):
            report['verdict'] = 'REFUSED'
        return report

    fields = dict(zip(V2_U32, struct.unpack_from('<5I', data, 80)))
    fields['publication_error'], = struct.unpack_from('<i', data, 100)
    fields.update(zip(V2_DIAG32, struct.unpack_from('<2I', data, 104)))
    fields.update(zip(V2_U64, struct.unpack_from('<16Q', data, 112)))
    fields['decision'], fields['publication_step'], fields['reserved'] = struct.unpack_from('<IIQ', data, 240)
    for name, values in (('outcome', OUTCOMES), ('reason', REASONS), ('stage', STAGES),
                         ('publication', PUBLICATIONS), ('decision', DECISIONS)):
        fields[name + '_name'] = _enum(fields[name], values, name)
    if fields['reserved']:
        raise ValueError('unsupported E0VS reserved data')
    # Keep every scalar including the first failure, even when policy checks fail.
    report.update(fields)
    report['diagnostic_flag_names'] = [name for bit, name in DIAGNOSTIC_FLAGS.items()
                                       if fields['diagnostic_flags'] & bit]
    report['context_changed_names'] = [name for bit, name in CONTEXT_BITS.items()
                                       if fields['context_changed'] & bit]
    flags_ok = check(fields['diagnostic_flags'] & ~511 == 0, 'known diagnostic flag bits')
    check(fields['context_changed'] & ~1023 == 0, 'known context-change bits')
    snapshot_ok = bool(fields['diagnostic_flags'] & 4)
    check(snapshot_ok, 'caller context snapshot captured')
    check(fields['safety'] & ~15 == 0, 'known restoration safety bits')
    check(fields['safety'] == 15, 'all restoration safety bits confirmed')
    par_ok = fields['saved_par'] == fields['restored_par']
    daif_ok = fields['daif_before'] == fields['daif_after']
    host_ok = (snapshot_ok and fields['current_el'] == 8 and
               fields['context_changed'] == 0 and not fields['diagnostic_flags'] & 128)
    check(par_ok, 'PAR restored')
    check(daif_ok, 'DAIF restored')
    check(host_ok, 'EL2 host context unchanged')
    check(not fields['safety'] & 1 or context_ok, 'HCR safety assertion agrees with evidence')
    check(not fields['safety'] & 2 or par_ok, 'PAR safety assertion agrees with evidence')
    check(not fields['safety'] & 4 or daif_ok, 'DAIF safety assertion agrees with evidence')
    check(not fields['safety'] & 8 or host_ok, 'host safety assertion agrees with evidence')
    extent_ok = check(fields['extent'] == extent, 'captured framebuffer extent agrees')
    line_ok = check(fields['cache_line'] == line, 'captured cache line agrees with CTR')
    entry_profile = (before & ((1 << 34) | (1 << 27) | (1 << 31)) ==
                     ((1 << 34) | (1 << 27) | (1 << 31)) and bool(fields['sctlr'] & 1))
    t0sz = fields['tcr'] & 63
    ips = (fields['tcr'] >> 32) & 7
    pa_bits = (32, 36, 40, 42, 44, 48)
    native_regime = (entry_profile and ((fields['tcr'] >> 14) & 3) in (0, 2) and
                     ips < len(pa_bits) and 16 <= t0sz <= 32 and
                     fb + extent <= (1 << pa_bits[ips]) and
                     fb + extent <= (1 << (64 - t0sz)) and not fields['tcr'] & (1 << 59))
    regime_asserted = bool(fields['diagnostic_flags'] & 8)
    # A renderer can refuse an unsupported TCR before AT or drawing while the
    # caller's established host profile remains unchanged and safe to resume.
    # REGIME_VALID records the observed entry profile; COMPLETE additionally
    # needs the supported descriptor/translation regime checked below.
    check(not regime_asserted or entry_profile, 'regime observation agrees with host entry profile')
    published = (fields['publication'] == 1 and fields['publication_error'] == 0 and
                 fields['publication_step'] == 2)
    check(fields['publication_step'] in (1, 2), 'known publication step')
    check(published, 'E0VS publication completed successfully')
    check(fields['publication'] != 2 or fields['publication_error'] != 0,
          'failed publication carries an error')
    safe = (fields['safety'] == 15 and context_ok and par_ok and daif_ok and host_ok and
            entry_profile and regime_asserted and fields['daif_before'] & ~0x3c0 == 0 and error != 3)
    complete = fields['outcome'] == 1
    outcome_ok = check((error == 0 and frames == 3 and fields['reason'] == 0) if complete else
                       (error != 0 and fields['reason'] != 0), 'outcome agrees with first failure')
    frame_ok = check(fields['failed_frame'] == 3 if complete else
                     fields['failed_frame'] <= min(frames, 3),
                     'failed-frame sentinel agrees with first failure')
    failure_flags = fields['diagnostic_flags'] & 3
    diagnostic_ok = check(failure_flags in (0, 3), 'failure VA and PAR validity flags agree')
    if not complete:
        diagnostic_ok &= check(not frames or fields['diagnostic_flags'] & 16,
                               'completed frames retain draw evidence')
        reason_stages = {1: (1,), 2: (1,), 3: (2,), 4: (3,), 5: (4,), 6: (5,),
                         7: (6,), 8: (6,), 9: (6,), 10: (7,), 11: (7,),
                         12: (5, 7), 13: (5,), 14: (10,), 15: (9,)}
        diagnostic_ok &= check(fields['stage'] in reason_stages.get(fields['reason'], ()),
                               'first failure reason agrees with its stage')
        if fields['reason'] == 2:
            diagnostic_ok &= check(1 <= fields['detail'] <= 11,
                                   'geometry refusal identifies its first predicate')
    if failure_flags:
        diagnostic_ok &= check(fields['reason'] in (7, 8, 9) and fields['stage'] == 6,
                               'failed AT evidence identifies the translation stage')
        diagnostic_ok &= check(fields['diagnostic_flags'] & 96 == 96,
                               'failed AT retains PAR and DAIF operation evidence')
        page = 16384 if (fields['tcr'] >> 14) & 3 == 2 else 4096
        diagnostic_ok &= check(fields['failed_va'] % page == 0, 'failed VA uses the actual granule')
        if fields['reason'] == 7:
            diagnostic_ok &= check(bool(fields['raw_par'] & 1), 'translation refusal has PAR.F')
        elif fields['reason'] == 8:
            diagnostic_ok &= check(not fields['raw_par'] & 1 and
                                   fields['raw_par'] & PAR_PA != fields['failed_va'],
                                   'identity refusal has a different translated address')
        elif fields['reason'] == 9:
            diagnostic_ok &= check(not fields['raw_par'] & 1 and
                                   fields['raw_par'] & PAR_PA == fields['failed_va'] and
                                   (bool(fields['diagnostic_flags'] & 256) or
                                    fields['raw_par'] >> 56 not in (0x44, 0xff)),
                                   'attribute refusal has an unsupported identity PAR')
    else:
        diagnostic_ok &= check(fields['failed_va'] == fields['raw_par'] == 0,
                               'absent AT failure evidence is zero')
        diagnostic_ok &= check(fields['reason'] not in (7, 8, 9),
                               'translation refusal requires VA and PAR evidence')
    if complete:
        diagnostic_ok &= check(fields['stage'] == 8 and fields['diagnostic_flags'] & ~256 == 0x7c,
                               'complete smoke has only successful draw diagnostics')
        check(native_regime and regime_asserted, 'supported native translation regime')
        check(fields['diagnostic_flags'] & 0x7c == 0x7c,
              'complete trace includes snapshot, regime, draw and restoration observations')
        check(fields['daif_before'] & ~0x3c0 == 0, 'architectural DAIF encoding')
        diagnostic_ok &= check(fields['detail'] == 0, 'complete smoke has no failure predicate')
    translation_ok = _translation(report, translation, check) if fields['diagnostic_flags'] & 256 else True
    if not fields['diagnostic_flags'] & 256 and translation is not None:
        translation_ok = check(False, 'companion record requires its E0VS flag')
    evidence_ok = (bounded_frames and flags_ok and extent_ok and line_ok and outcome_ok and translation_ok and
                   frame_ok and diagnostic_ok and
                   (not complete or (metadata_ok and native_regime and regime_asserted)))
    expected_decision = 2 if not safe else 3 if not published or not evidence_ok else 1
    report['expected_decision'] = expected_decision
    # P3 is a separate outer veto. A stored CONTINUE must satisfy the complete
    # evidence policy, not merely report unchanged HCR/PAR/DAIF values.
    decision_ok = fields['decision'] in (4, expected_decision)
    check(decision_ok, 'continuation decision agrees with restoration/publication evidence')
    check(fields['decision'] == 1, 'native policy permits normal continuation')
    check(frames == 3, 'three smoke frames completed')
    check(error == 0, 'legacy smoke error is zero')
    check(complete, 'smoke outcome COMPLETE')
    report['normal_continuation'] = expected_decision == fields['decision'] == 1
    report['render_state'] = {1: 'RENDER_COMPLETE_UNOBSERVED', 2: 'RENDERER_REFUSED',
                              3: 'RENDER_FAILED'}[fields['outcome']]
    if fields['decision'] == 4:
        report['verdict'] = 'BLOCKED_P3'
    elif not safe:
        report['verdict'] = 'BLOCKED_CONTEXT'
    elif not published or not evidence_ok:
        report['verdict'] = 'BLOCKED_EVIDENCE'
    elif not report['errors']:
        report['verdict'] = 'SMOKE_TRACE_VALID'
    elif fields['outcome'] == 2 and outcome_ok and frame_ok and diagnostic_ok and decision_ok:
        report['verdict'] = 'REFUSED'
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('record', type=Path)
    parser.add_argument('--build', required=True)
    parser.add_argument('--translation', type=Path, help='companion 168-byte E0XT record')
    args = parser.parse_args(argv)
    report = decode(args.record.read_bytes(), args.build,
                    args.translation.read_bytes() if args.translation else None)
    print(json.dumps(report, indent=2))
    return 0 if report['verdict'] == 'SMOKE_TRACE_VALID' else 2


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except ValueError as exc:
        raise SystemExit('FAIL: ' + str(exc))
