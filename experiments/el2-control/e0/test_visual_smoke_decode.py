#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Host-only protocol tests; synthetic v2 data is never physical evidence.

V1 fixtures are unmodified archived QEMU smoke records. Their provenance and
hashes accompany the bytes; neither version proves physical J713 scanout.
"""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


decoder = module('smoke_under_test', HERE / 'visual_smoke_decode.py')
collector = module('smoke_collector_under_test', HERE / 'collect_visual_smoke.py')
BUILD = '0123456789abcdef'


def synthetic_v2(**changes):
    """An invented, internally consistent protocol record, not a hardware dump."""
    hcr = (1 << 34) | (1 << 27) | (1 << 31)
    values = dict(frames=3, error=0, hcr_before=hcr, hcr_after=hcr,
                  ctr=4 << 16, ticks=100, framebuffer=0x804000000,
                  width=640, height=360, stride=2624, depth=32,
                  build=int(BUILD, 16), outcome=1, reason=0, stage=8, safety=15,
                  publication=1, publication_error=0, failed_frame=3,
                  diagnostic_flags=0x7c, failed_va=0, raw_par=0,
                  saved_par=0x12340000, restored_par=0x12340000,
                  daif_before=0x3c0, daif_after=0x3c0,
                  sctlr=1, tcr=(3 << 32) | (1 << 30) | (16 << 16) | (2 << 14) | 16,
                  ttbr0=0x800000000, ttbr1=0x800000000, mair=0xff4400,
                  current_el=8, context_changed=0, extent=2624 * 360,
                  cache_line=64, detail=0, decision=1, publication_step=2, reserved=0)
    values.update(changes)
    result = bytearray(256)
    struct.pack_into('<IHHII', result, 0, decoder.MAGIC, 2, 256, values['frames'], values['error'])
    struct.pack_into('<5Q', result, 16, *(values[key] for key in
                                      ('hcr_before', 'hcr_after', 'ctr', 'ticks', 'framebuffer')))
    struct.pack_into('<4IQ', result, 56, *(values[key] for key in
                                         ('width', 'height', 'stride', 'depth', 'build')))
    struct.pack_into('<5Ii2I', result, 80, *(values[key] for key in
                     (*decoder.V2_U32, 'publication_error', *decoder.V2_DIAG32)))
    struct.pack_into('<16QIIQ', result, 112, *(values[key] for key in
                      (*decoder.V2_U64, 'decision', 'publication_step', 'reserved')))
    return bytes(result)


def synthetic_translation(**changes):
    """Native-shaped owned-table metadata for protocol tests, not a device dump."""
    start = (0x804000000 + 90 * 2624) & ~16383
    count = (0x804000000 + 270 * 2624 - start + 16383) // 16384
    v = dict(build=int(BUILD, 16), va=start, par=(0x44 << 56) | start | 0xb00,
             root=0x800000000, d0=0x800004003, d1=0x800008003, d2=0x80000c003,
             d3=start | (3 << 53) | 0x607, pa=start, isar1=1 << 56, mmfr3=1, tcr2=0,
             mair_alias=0xff4400, last_va=start + (count - 1) * 16384, resolved_mair=0x44,
             pfr0=0, samples=count, page_shift=14, level=3, status=1, attribute=0, flags=7,
             publication=1, publication_error=0)
    v.update(changes)
    q = ('build', 'va', 'par', 'root', 'd0', 'd1', 'd2', 'd3', 'pa', 'isar1', 'mmfr3',
         'tcr2', 'mair_alias', 'last_va', 'resolved_mair', 'pfr0')
    i = ('samples', 'page_shift', 'level', 'status', 'attribute', 'flags', 'publication', 'publication_error')
    return struct.pack('<IHH16Q7Ii', 0x54583045, 1, 168, *(v[k] for k in q+i))


class DecoderTests(unittest.TestCase):
    def test_immutable_j713_attribute_refusal(self):
        path = HERE / 'fixtures/j713-attribute-refused-f9fe36a50e175b99.bin'
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(),
                         '2abb489bf6e5e39ceb004046e82b6e571766d0fbc1952021108a198965d30316')
        report = decoder.decode(path.read_bytes(), 'f9fe36a50e175b99')
        self.assertEqual((report['verdict'], report['reason'], report['frames']), ('REFUSED', 9, 0))
        self.assertEqual(report['raw_par'], 0x40000103e2324b00)
        self.assertNotIn('translation', report)  # Never invent a historical leaf or XS snapshot.
    def test_original_v1_fixtures(self):
        provenance = json.loads((HERE / 'fixtures/legacy-smoke-v1.json').read_text())
        for fixture in provenance['fixtures']:
            with self.subTest(depth=fixture['depth']):
                data = (HERE / 'fixtures' / fixture['file']).read_bytes()
                self.assertEqual(hashlib.sha256(data).hexdigest(), fixture['sha256'])
                report = decoder.decode(data, fixture['build_id'])
                self.assertEqual(report['verdict'], 'SMOKE_TRACE_VALID')
                self.assertEqual(report['depth'], fixture['depth'])
                self.assertFalse(report['normal_continuation'])
                self.assertFalse(report['stage2_test'])
                self.assertIn('PENDING', report['physical_visibility'])

    def test_v1_valid_failures_retain_diagnostics(self):
        source = bytearray((HERE / 'fixtures/legacy-smoke32-v1.bin').read_bytes())
        for error, verdict in ((1, 'REFUSED'), (2, 'REFUSED'), (3, 'FAIL')):
            data = bytearray(source)
            struct.pack_into('<II', data, 8, 1, error)
            report = decoder.decode(data, '496be05475106a9a')
            self.assertEqual(report['verdict'], verdict)
            self.assertEqual((report['frames'], report['error']), (1, error))
            self.assertEqual(report['framebuffer'], '0x40410080')
            self.assertTrue(report['errors'])

    def test_v2_complete(self):
        report = decoder.decode(synthetic_v2(), BUILD)
        self.assertEqual(report['verdict'], 'SMOKE_TRACE_VALID')
        self.assertTrue(report['normal_continuation'])
        self.assertEqual(report['errors'], [])
        self.assertEqual(report['size'], 256)
        self.assertFalse(report['stage2_test'])

    def test_safe_failure_is_not_success_but_may_continue(self):
        record = synthetic_v2(frames=1, error=2, outcome=2, reason=7, stage=6,
                              failed_frame=1, failed_va=0x804004000,
                              raw_par=0x9600000000000001, diagnostic_flags=0x7f)
        report = decoder.decode(record, BUILD)
        self.assertEqual(report['verdict'], 'REFUSED')
        self.assertEqual(report['decision'], 1)
        self.assertTrue(report['normal_continuation'])
        self.assertEqual(report['reason_name'], 'TRANSLATION')
        self.assertEqual(report['failed_va'], 0x804004000)
        self.assertEqual(report['raw_par'], 0x9600000000000001)
        self.assertEqual(report['stage'], 6)

    def test_smaller_canonical_low_va_regime_and_p3_priority(self):
        # QEMU's direct fixture uses a 36-bit low VA, while native uses 48.
        tcr = (3 << 32) | 28
        self.assertEqual(decoder.decode(synthetic_v2(tcr=tcr), BUILD)['verdict'],
                         'SMOKE_TRACE_VALID')
        outside_pa = synthetic_v2(tcr=(2 << 14) | 16)
        self.assertNotEqual(decoder.decode(outside_pa, BUILD)['verdict'], 'SMOKE_TRACE_VALID')
        for t0sz in (15, 33):
            self.assertNotEqual(decoder.decode(synthetic_v2(tcr=(3 << 32) | t0sz), BUILD)['verdict'],
                                'SMOKE_TRACE_VALID')
        for changes in ({'safety': 0}, {'publication': 0}):
            report = decoder.decode(synthetic_v2(decision=4, **changes), BUILD)
            self.assertEqual(report['verdict'], 'BLOCKED_P3')
            self.assertFalse(report['normal_continuation'])
        report = decoder.decode(synthetic_v2(error=3, outcome=3, reason=12, stage=7), BUILD)
        self.assertEqual(report['verdict'], 'BLOCKED_CONTEXT')
        self.assertFalse(report['normal_continuation'])

    def test_unsupported_renderer_regime_can_safely_refuse_before_at(self):
        report = decoder.decode(synthetic_v2(frames=0, error=2, outcome=2, reason=6,
                      stage=5, failed_frame=0, tcr=0, diagnostic_flags=12), BUILD)
        self.assertEqual(report['verdict'], 'REFUSED')
        self.assertTrue(report['normal_continuation'])
        self.assertEqual(report['reason_name'], 'REGIME')
        self.assertNotIn('PAR_TOUCHED', report['diagnostic_flag_names'])

    def test_contradictory_failure_diagnostics_are_retained_but_rejected(self):
        failure = dict(frames=1, error=2, outcome=2, reason=7, stage=6,
                       failed_frame=1, failed_va=0x804004000, raw_par=1,
                       diagnostic_flags=0x7f)
        for changes in ({'stage': 7}, {'raw_par': 0}, {'diagnostic_flags': 0x6d},
                        {'failed_frame': 3}, {'failed_va': 0x804004001}):
            with self.subTest(changes=changes):
                values = dict(failure, **changes)
                report = decoder.decode(synthetic_v2(**values), BUILD)
                self.assertEqual(report['verdict'], 'BLOCKED_EVIDENCE')
                self.assertFalse(report['normal_continuation'])
                self.assertEqual(report['raw_par'], values['raw_par'])
                self.assertEqual(report['reason'], 7)

    def test_identity_refusal_preserves_high_output_pa_bits(self):
        # This output PA differs only above bit 47. A low-VA bound cannot be
        # used to discard the high output bits from a captured failing PAR.
        va = 0x804004000
        high_output = (0x44 << 56) | (1 << 48) | va
        failure = dict(frames=1, error=2, outcome=2, reason=8, stage=6,
                       failed_frame=1, failed_va=va, raw_par=high_output,
                       diagnostic_flags=0x7f)
        report = decoder.decode(synthetic_v2(**failure), BUILD)
        self.assertEqual(report['verdict'], 'REFUSED')
        self.assertTrue(report['normal_continuation'])
        self.assertEqual(report['raw_par'], high_output)
        self.assertEqual(report['failed_va'], va)
        # An attribute-only refusal must describe an identity translation.
        failure.update(reason=9, raw_par=(0x88 << 56) | (1 << 48) | va)
        report = decoder.decode(synthetic_v2(**failure), BUILD)
        self.assertEqual(report['verdict'], 'BLOCKED_EVIDENCE')
        self.assertFalse(report['normal_continuation'])
        self.assertEqual(report['raw_par'], failure['raw_par'])

    def test_signed_publication_failure_preserves_first_error(self):
        report = decoder.decode(synthetic_v2(frames=0, error=2, outcome=2, reason=2, stage=1,
                      failed_frame=0, publication=2, publication_error=-3, decision=3,
                      diagnostic_flags=12, detail=4), BUILD)
        self.assertEqual(report['verdict'], 'BLOCKED_EVIDENCE')
        self.assertEqual((report['error'], report['reason'], report['stage']), (2, 2, 1))
        self.assertEqual(report['publication_error'], -3)
        self.assertFalse(report['normal_continuation'])

    def test_unsafe_cannot_continue_even_if_claimed(self):
        for changes in ({'safety': 14}, {'restored_par': 0}, {'daif_after': 0},
                        {'context_changed': 16}, {'diagnostic_flags': 0xfc},
                        {'hcr_after': 1}, {'current_el': 4}, {'diagnostic_flags': 0x78}):
            with self.subTest(changes=changes):
                report = decoder.decode(synthetic_v2(**changes), BUILD)
                self.assertEqual(report['verdict'], 'BLOCKED_CONTEXT')
                self.assertFalse(report['normal_continuation'])
                self.assertEqual(report['decision'], 1)

    def test_failure_decisions_must_agree_with_evidence(self):
        for changes, verdict in (({'decision': 2}, 'FAIL'),
                                  ({'decision': 3}, 'FAIL'),
                                  ({'decision': 4}, 'BLOCKED_P3'),
                                  ({'publication': 0, 'decision': 1}, 'BLOCKED_EVIDENCE'),
                                  ({'publication_error': -1}, 'BLOCKED_EVIDENCE'),
                                  ({'publication_step': 1}, 'BLOCKED_EVIDENCE')):
            with self.subTest(changes=changes):
                report = decoder.decode(synthetic_v2(**changes), BUILD)
                self.assertEqual(report['verdict'], verdict)
                self.assertFalse(report['normal_continuation'])

    def test_metadata_and_semantic_contradictions_never_succeed(self):
        cases = ({'frames': 2}, {'frames': 4}, {'error': 1}, {'outcome': 2}, {'reason': 1},
                 {'failed_frame': 0}, {'width': 399}, {'height': 0xffffffff}, {'stride': 4},
                 {'depth': 16}, {'framebuffer': 0}, {'framebuffer': (1 << 48) - 64},
                 {'ctr': 0}, {'extent': 1}, {'cache_line': 128}, {'sctlr': 0},
                 {'tcr': 0}, {'tcr': (1 << 59) | ((3 << 32) | (16 << 16) | (2 << 14) | 16)},
                 {'hcr_before': 0, 'hcr_after': 0}, {'diagnostic_flags': 0x7c | 256},
                 {'publication_step': 9}, {'daif_before': 0x1000, 'daif_after': 0x1000})
        for changes in cases:
            with self.subTest(changes=changes):
                report = decoder.decode(synthetic_v2(**changes), BUILD)
                self.assertNotEqual(report['verdict'], 'SMOKE_TRACE_VALID')
                self.assertTrue(report['errors'])

    def test_malformed_or_unsupported_encodings_raise(self):
        valid = synthetic_v2()
        cases = [b'', valid[:7], valid[:80], valid[:-1], valid + b'\0']
        for offset, value, fmt in ((0, 0, '<I'), (4, 3, '<H'), (6, 80, '<H'),
                                   (12, 4, '<I'), (80, 4, '<I'), (84, 16, '<I'),
                                   (88, 11, '<I'), (96, 3, '<I'), (240, 0, '<I'),
                                   (248, 1, '<Q'), (72, 0, '<Q')):
            data = bytearray(valid)
            struct.pack_into(fmt, data, offset, value)
            cases.append(data)
        for data in cases:
            with self.subTest(length=len(data)):
                with self.assertRaises(ValueError):
                    decoder.decode(data, BUILD)
        for identity in ('bad', '00000000000000000', 'g' * 16, None):
            with self.assertRaises(ValueError):
                decoder.decode(valid, identity)


class CollectorTests(unittest.TestCase):
    def fixture(self, record=None):
        p3 = bytearray(64)
        struct.pack_into('<I', p3, 0, 0x58505242)
        props = {'compatible': b'apple,j713\0apple,t8132\0',
                 '6boot,e0-visual-mode': b'smoke\0',
                 '6boot,e0-visual-result': record or synthetic_v2(),
                 '6boot,p3-result': bytes(p3)}
        manifest = {'build_id': BUILD, 'cmdline': 'root=fixture x6e3.fbsmoke=' + BUILD}
        return props, manifest

    def test_live_boot_binding_and_failure_retention(self):
        props, manifest = self.fixture()
        def evaluate(p=props, m=manifest, release='7.1.13-x6e3-rel3', cmdline=manifest['cmdline']):
            return collector.evaluate(p, m, release, cmdline)
        self.assertEqual(evaluate()['verdict'], 'SMOKE_TRACE_VALID')
        for name in props:
            bad = dict(props)
            bad.pop(name)
            self.assertEqual(evaluate(bad)['verdict'], 'FAIL')
        self.assertEqual(evaluate(release='wrong')['verdict'], 'FAIL')
        self.assertEqual(evaluate(cmdline=manifest['cmdline'] + ' x6e3.fbsmoke=other')['verdict'], 'FAIL')
        self.assertEqual(evaluate(m=dict(manifest, build_id='f' * 16))['verdict'], 'FAIL')
        failure = synthetic_v2(frames=0, failed_frame=0, error=2, outcome=2, reason=9,
                               stage=6, failed_va=0x804000000, raw_par=0x8800000804000000,
                               diagnostic_flags=0x6f)
        props['6boot,e0-visual-result'] = failure
        report = evaluate()
        self.assertEqual(report['verdict'], 'REFUSED')
        self.assertEqual(report['raw_par'], 0x8800000804000000)
        self.assertEqual(props['6boot,e0-visual-result'], failure)

    def test_reuses_native_mount_and_bounded_helpers(self):
        self.assertIs(collector.bounded, collector.native.bounded)
        self.assertIs(collector.destination_root, collector.native.destination_root)
        with tempfile.TemporaryDirectory() as tmp:
            lookalike = Path(tmp) / 'X63-R'
            lookalike.mkdir()
            with patch.object(collector.native.subprocess, 'check_output',
                              return_value=b'{"blockdevices":[]}'):
                with self.assertRaises(ValueError):
                    collector.destination_root(str(lookalike))
            source = Path(tmp) / 'oversized'
            source.write_bytes(b'12345')
            with self.assertRaises(ValueError):
                collector.bounded(source, 4)

    def test_visibility_requires_trace_and_explicit_observation(self):
        props, manifest = self.fixture(synthetic_v2(diagnostic_flags=0x17c))
        manifest['translation_required'] = True
        props['6boot,e0-translation-result'] = synthetic_translation()
        report = collector.evaluate(props, manifest, '7.1.13-x6e3-rel3', manifest['cmdline'])
        self.assertEqual(report['verdict'], 'SMOKE_TRACE_VALID')
        self.assertEqual(report['render_state'], 'RENDER_COMPLETE_UNOBSERVED')
        self.assertEqual(collector.evaluate(props, manifest, '7.1.13-x6e3-rel3',
            manifest['cmdline'], True)['render_state'], 'RENDER_COMPLETE_USER_OBSERVED')
        props.pop('6boot,e0-translation-result')
        self.assertEqual(collector.evaluate(props, manifest, '7.1.13-x6e3-rel3',
            manifest['cmdline'], True)['verdict'], 'FAIL')
        props, manifest = self.fixture()
        self.assertEqual(collector.evaluate(props, manifest, '7.1.13-x6e3-rel3',
            manifest['cmdline'] + ' root=wrong')['verdict'], 'FAIL')
        manifest['translation_required'] = True
        self.assertEqual(collector.evaluate(props, manifest, '7.1.13-x6e3-rel3',
            manifest['cmdline'])['verdict'], 'FAIL')

    def test_no_capture_written_when_mount_verification_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            manifest = Path(tmp) / 'manifest.json'
            manifest.write_text(json.dumps(self.fixture()[1]))
            with patch.object(collector.platform, 'system', return_value='Linux'), \
                 patch.object(collector.os, 'geteuid', return_value=0), \
                 patch.object(collector, 'destination_root', side_effect=ValueError('not mounted')):
                with self.assertRaises(ValueError):
                    collector.main(['--manifest', str(manifest), '--destination', tmp])
            self.assertEqual([p.name for p in Path(tmp).iterdir()], ['manifest.json'])

    def test_capture_retains_raw_refusal_and_always_uses_new_folder(self):
        # A temporary fake Linux DT and mounted destination exercise file output;
        # all live system reads and mount discovery are replaced for this test.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            mount = root / 'mounted-volume'
            mount.mkdir()
            chosen = root / 'fake-chosen'
            chosen.mkdir()
            record = synthetic_v2(frames=0, failed_frame=0, error=2, outcome=2,
                                  reason=2, stage=1, width=399, detail=4, diagnostic_flags=12)
            props, manifest = self.fixture(record)
            for name, value in props.items():
                if name.startswith('6boot,'):
                    (chosen / name).write_bytes(value)
            manifest_path = root / 'manifest.json'
            manifest_path.write_text(json.dumps(manifest))
            reads = {'/proc/sys/kernel/random/boot_id': b'12345678-1234-1234-1234-123456789abc',
                     '/proc/cmdline': manifest['cmdline'].encode(),
                     '/sys/firmware/devicetree/base/compatible': props['compatible'],
                     '/sys/firmware/fdt': b'host-only-fake-fdt', '/proc/version': b'host-only-fixture'}
            def path(value):
                if str(value) == '/sys/firmware/devicetree/base/chosen':
                    return chosen
                return Path(value)
            def read(source, limit=1024 * 1024):
                if str(source) in reads:
                    data = reads[str(source)]
                    self.assertLessEqual(len(data), limit)
                    return data
                return collector.native.bounded(source, limit)
            with patch.object(collector, 'Path', side_effect=path), \
                 patch.object(collector, 'bounded', side_effect=read), \
                 patch.object(collector, 'destination_root', return_value=mount), \
                 patch.object(collector.platform, 'system', return_value='Linux'), \
                 patch.object(collector.platform, 'release', return_value='7.1.13-x6e3-rel3'), \
                 patch.object(collector.platform, 'uname', return_value=('Linux', 'host-only-fixture')), \
                 patch.object(collector.os, 'geteuid', return_value=0), \
                 patch.object(collector.os, 'sync'), \
                 patch.object(collector.subprocess, 'run', return_value=
                              subprocess.CompletedProcess([], 0, b'host-only-kernel-log', b'')), \
                 patch('builtins.print'):
                for _ in range(2):
                    self.assertEqual(collector.main(['--manifest', str(manifest_path)]), 2)
            folders = list((mount / 'j713-diag/fb-smoke').iterdir())
            self.assertEqual(len(folders), 2)
            for folder in folders:
                self.assertEqual((folder / 'chosen/6boot,e0-visual-result').read_bytes(), record)
                summary = json.loads((folder / 'summary.json').read_text())
                self.assertEqual(summary['verdict'], 'REFUSED')
                self.assertEqual((summary['reason'], summary['error']), (2, 2))
                self.assertFalse(summary['stage2_test'])
                self.assertEqual(summary['capture_errors'], [])
                sums = (folder / 'SHA256SUMS').read_text()
                self.assertIn(hashlib.sha256(record).hexdigest() +
                              '  chosen/6boot,e0-visual-result', sums)

    def test_portable_bundle_works_in_isolated_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            for name in ('collect_visual_smoke.py', 'visual_smoke_decode.py'):
                shutil.copy2(HERE / name, folder / name)
            shutil.copy2(collector.HELPER, folder / 'native_collector.py')
            result = subprocess.run([sys.executable, '-I', '-B', str(folder / 'collect_visual_smoke.py'),
                                     '--help'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('already mounted', result.stdout)
            self.assertEqual((folder / 'native_collector.py').read_bytes(), collector.HELPER.read_bytes())


class PolicyAgreementTests(unittest.TestCase):
    """Independent wire mutations exercise the production C policy and decoder."""
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='e0vs-policy-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.cli = Path(cls.temporary.name) / 'record-test'
        root = HERE.parents[2]
        fdt = root / 'src/libfdt'
        command = ['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                   '-D_POSIX_C_SOURCE=200809L', '-include', 'stdlib.h',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                   '-I' + str(root / 'src'), '-I' + str(fdt), '-I' + str(HERE),
                   str(HERE / 'visual_smoke_record.c'), str(HERE / 'smoke_record_test.c')]
        command += [str(path) for path in sorted(fdt.glob('*.c'))]
        compiled = subprocess.run(command + ['-o', str(cls.cli)], capture_output=True, text=True)
        if compiled.returncode:
            raise RuntimeError(compiled.stdout + compiled.stderr)

    def decision(self, record, expected, translation=None):
        result = subprocess.run([str(self.cli), '--decision'], input=record + (translation or b''), capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        actual = int(result.stdout)
        self.assertEqual(actual, expected)
        # The final receipt carries the actual C decision, not a test's claim.
        receipt = bytearray(record)
        struct.pack_into('<I', receipt, 240, actual)
        report = decoder.decode(receipt, BUILD, translation)
        self.assertEqual(report['normal_continuation'], actual == 1, report)
        return report

    def test_missing_draw_proof_denies_handoff(self):
        for changes, expected in (({'stage': 0}, 3), ({'diagnostic_flags': 0}, 2),
                                  ({'diagnostic_flags': 12}, 3)):
            with self.subTest(changes=changes):
                self.decision(synthetic_v2(**changes), expected)

    def test_valid_and_contradictory_evidence(self):
        self.decision(synthetic_v2(), 1)
        refused = dict(frames=0, error=2, outcome=2, reason=2, stage=1,
                       failed_frame=0, width=399, detail=4, diagnostic_flags=12)
        self.assertEqual(self.decision(synthetic_v2(**refused), 1)['verdict'], 'REFUSED')
        for changes, expected in (({'safety': 0}, 2), ({'current_el': 4}, 2),
                ({'sctlr': 0}, 2), ({'context_changed': 1}, 2),
                ({'daif_before': 0x1000, 'daif_after': 0x1000}, 2),
                ({'publication': 2, 'publication_error': -3}, 3),
                ({'publication_step': 1}, 3), ({'failed_frame': 0}, 3),
                ({'cache_line': 128}, 3), ({'extent': 1}, 3),
                ({'width': 399}, 3), ({'tcr': 0}, 3),
                ({'failed_va': 0x804000000}, 3), ({'raw_par': 1}, 3),
                ({'diagnostic_flags': 0x7c | 256}, 3), ({'detail': 1}, 3)):
            with self.subTest(changes=changes):
                self.decision(synthetic_v2(**changes), expected)
        for changes in ({'stage': 6}, {'failed_frame': 2}, {'detail': 0}, {'extent': 1}):
            with self.subTest(refusal=changes):
                self.decision(synthetic_v2(**dict(refused, **changes)), 3)

    def test_wire_mutations_match_the_independent_policy(self):
        # decision at offset 240 is an output of the C policy, not an input.
        # Its on-wire corruption is covered by test_failure_decisions_must_agree_with_evidence.
        offsets = [(offset, '<I') for offset in (8, 12, 56, 60, 64, 68, 80, 84, 88,
                                                92, 96, 104, 108, 244)]
        offsets += [(offset, '<Q') for offset in range(112, 240, 8)]
        offsets += [(offset, '<Q') for offset in (16, 24, 32, 48, 248)]
        for offset, fmt in offsets:
            for value in (0, 1, (1 << (32 if fmt == '<I' else 64)) - 1):
                with self.subTest(offset=offset, value=value):
                    wire = bytearray(synthetic_v2())
                    struct.pack_into(fmt, wire, offset, value)
                    try:
                        report = decoder.decode(wire, BUILD)
                    except ValueError:
                        result = subprocess.run([str(self.cli), '--decision'], input=wire,
                                                capture_output=True, check=True)
                        self.assertNotEqual(int(result.stdout), 1)
                    else:
                        self.decision(wire, report['expected_decision'])

    def test_at_failure_requires_complete_and_consistent_evidence(self):
        fault = dict(frames=0, error=2, outcome=2, reason=7, stage=6, failed_frame=0,
                     failed_va=0x804004000, raw_par=1, diagnostic_flags=0x6f)
        self.decision(synthetic_v2(**fault), 1)
        for changes in ({'raw_par': 0}, {'failed_va': 0x804004001},
                        {'diagnostic_flags': 0x6c}, {'diagnostic_flags': 15}):
            with self.subTest(changes=changes):
                self.decision(synthetic_v2(**dict(fault, **changes)), 3)
        for reason, par in ((8, (0x44 << 56) | (1 << 48) | fault['failed_va']),
                            (9, (0x88 << 56) | fault['failed_va'])):
            self.decision(synthetic_v2(**dict(fault, reason=reason, raw_par=par)), 1)

    def test_attribute_classes_and_xs_boundaries(self):
        for actual, selected, xs, sh, expected in (
                (0x44, 0x44, 0, 2, 0), (0x40, 0x44, 1, 2, 0),
                (0x40, 0x40, 1, 2, 0), (0x44, 0x40, 1, 2, 0),
                (0xff, 0xff, 0, 3, 0), (0x40, 0x44, 0, 2, 2),
                (0x40, 0x44, 2, 2, 2), (0x44, 0xff, 1, 2, 3),
                (0xff, 0x44, 1, 2, 3), (0x44, 0x44, 1, 0, 3),
                (0x00, 0x00, 0, 2, 1), (0x04, 0x04, 0, 2, 1),
                (0x01, 0x01, 1, 2, 1), (0x05, 0x05, 1, 2, 1),
                (0xa0, 0xa0, 1, 2, 1), (0xf0, 0xf0, 1, 2, 1)):
            with self.subTest(actual=actual, selected=selected, xs=xs, sh=sh):
                p = subprocess.run([str(self.cli), '--attribute', hex((actual << 56) | (sh << 7)),
                                    str(selected), hex(xs << 56)], capture_output=True, check=True)
                self.assertEqual(int(p.stdout), expected)

    def test_native_translation_evidence_and_refusals(self):
        main = synthetic_v2(diagnostic_flags=0x17c)
        self.decision(main, 1, synthetic_translation())
        self.decision(main, 1, synthetic_translation(par=(0x40 << 56) | 0x804038000 | 0xb00))
        self.decision(main, 3)  # An E0VS receipt alone cannot authorize the new candidate.
        self.decision(synthetic_v2(), 3, synthetic_translation())  # Lost required flag cannot fall back to legacy.
        for changes in ({'root': 0}, {'d0': 1}, {'d1': 0x800008003 | (1 << 63)},
                        {'d1': 0x800008003 | (1 << 48)},
                        {'d3': 0x804038607 | 128}, {'d3': 0x804038207},
                        {'samples': 1}, {'last_va': 0}, {'page_shift': 12}, {'level': 0},
                        {'resolved_mair': 0xff}, {'tcr2': 1}, {'mair_alias': 0},
                        {'mmfr3': 2}, {'flags': 15}, {'publication': 0}, {'publication_error': -3},
                        {'pfr0': 1 << 12}, {'pfr0': 15 << 12}):
            with self.subTest(changes=changes):
                self.decision(main, 3, synthetic_translation(**changes))
        for bit in (59, 40, 39, 37, 7):
            tcr = (3 << 32) | (1 << 30) | (16 << 16) | (2 << 14) | 16 | (1 << bit)
            self.decision(synthetic_v2(diagnostic_flags=0x17c, tcr=tcr), 3, synthetic_translation())
        # L2 block decoding uses 25 output-offset bits, not the 14-bit page offset.
        self.decision(main, 1, synthetic_translation(level=2, d2=0x804000605, d3=0))
        va = 0x804038000
        for actual, selected, xs, attribute in ((0x40, 0x44, 0, 2), (0xff, 0x44, 1, 3),
                                               (0xff, 0xff, 1, 4), (0x00, 0x00, 0, 1)):
            par = (actual << 56) | va | 0xb00
            mair = 0xff0000 | (selected << 8)
            record = synthetic_v2(frames=0, error=2, outcome=2, reason=9, stage=6,
                                  failed_frame=0, diagnostic_flags=0x16f, failed_va=va, raw_par=par,
                                  mair=mair)
            t = synthetic_translation(par=par, resolved_mair=selected, mair_alias=mair,
                                      isar1=xs << 56, attribute=attribute, samples=1, last_va=va)
            self.assertEqual(self.decision(record, 1, t)['verdict'], 'REFUSED')
        for data in (b'', synthetic_translation()[:-1], synthetic_translation()+b'\0'):
            report = decoder.decode(main, BUILD, data)
            self.assertEqual(report['verdict'], 'BLOCKED_EVIDENCE')
            self.assertFalse(report['normal_continuation'])
        refusal = synthetic_v2(frames=0, error=2, outcome=2, reason=6, stage=5,
                                failed_frame=0, diagnostic_flags=0x16c, detail=3)
        t = synthetic_translation(samples=1, last_va=va, status=3, flags=6)
        self.decision(refusal, 1, t)
        self.decision(refusal, 3, synthetic_translation(samples=1, last_va=va, status=4, flags=6))
        self.decision(refusal, 3, synthetic_translation(samples=1, last_va=va, status=3, flags=6, par=1))


if __name__ == '__main__':
    unittest.main()
