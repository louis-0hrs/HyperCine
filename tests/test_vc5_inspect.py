# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Synthetic only: no reference materials or external decoder execution."""
import importlib.util
import contextlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

TOOL = Path(__file__).resolve().parents[1] / 'tools' / 'vc5_inspect.py'
spec = importlib.util.spec_from_file_location('vc5_inspect', TOOL)
v = importlib.util.module_from_spec(spec)
spec.loader.exec_module(v)


def pair(tag, value):
    return struct.pack('>HH', tag & 65535, value)


def stream(extra=b'', chunk=None):
    return b'VC-5' + pair(12, 3) + pair(20, 48) + pair(21, 48) + extra + (pair(0x6000, 1) + b'abcd' if chunk is None else chunk)


def context():
    return {'claim': 'vc5', 'acceptance_claim': 'encoder', 'parts': [1, 3], 'build_parts': [1, 3], 'runtime_parts': [1, 3],
            'source': {'commit': 'a'*40, 'tree': 'b'*40, 'dirty': True, 'build_sha256': 'c'*64, 'executable_sha256': 'd'*64},
            'decoder': {'state': 'completed', 'qualified': True, 'executable_sha256': 'e'*64, 'version_id': 'synthetic-1', 'configuration_sha256': 'f'*64, 'warnings': 0, 'errors': 0, 'exit_code': 0, 'pixel_format': 'rg48', 'output_sha256': '1'*64, 'materials_available': True, 'inventory_complete': True, 'pixels_equal': True}}


class InspectorTests(unittest.TestCase):
    def refusal(self, data, reason, parts=(1, 3)):
        with self.assertRaisesRegex(v.Refusal, '^'+reason+'$'):
            v.inspect_bytes(data, parts)

    def test_synthetic_structure(self):
        r = v.inspect_bytes(stream(), [1])
        self.assertEqual((r['status'], r['segments'], r['chunks']), ('PASS', 4, 1))
        self.assertEqual(v.evidence(stream(), context())['verdict'], 'UNQUALIFIED')

    def test_pattern_width_context(self):
        r = v.inspect_bytes(stream(pair(106, 1)+pair(107, 1)+pair(108, 3)+pair(84, 1)+pair(102, 12)), [1, 3])
        self.assertEqual(r['chunks'], 1)
        self.refusal(stream(pair(106, 1)), 'HEADER_TAG', [1])

    def test_prescale_dual_unresolved(self):
        r = v.evidence(stream(pair(109, 0x2800)), context())
        self.assertEqual(r['syntax']['prescale']['table_b1'], [0, 0, 8])
        self.assertEqual(r['syntax']['prescale']['reference_layout'], [0, 2, 2])
        self.assertEqual(r['verdict'], 'UNQUALIFIED')
        self.assertIn('prescale', [x['claim'] for x in r['checks']])

    def test_framing(self):
        self.refusal(b'VC-', 'TRUNCATED_FRAMING')
        self.refusal(b'NOPE', 'UNKNOWN_FRAMING')
        self.refusal(stream()+b'x', 'TRUNCATED_SEGMENT')

    def test_header_completeness(self):
        self.refusal(b'VC-5'+pair(20, 1)+pair(0x6000, 0), 'MISSING_HEADER')
        self.refusal(b'VC-5', 'MISSING_HEADER')
        self.refusal(stream(chunk=b''), 'MISSING_BODY')

    def test_header_legality(self):
        self.refusal(stream(pair(999, 1)), 'HEADER_TAG')
        self.refusal(stream(pair(20, 1)), 'DUPLICATE_HEADER')
        self.refusal(stream(pair(101, 0)), 'HEADER_RANGE')
        self.refusal(stream(pair(101, 17)), 'PRECISION_RANGE')
        self.refusal(stream(pair(84, 5)), 'IMAGE_FORMAT_RANGE')
        self.refusal(b'VC-5'+pair(12, 5)+pair(20, 1)+pair(21, 1)+pair(0x6000, 0), 'CHANNEL_RANGE')

    def test_state_bounds(self):
        self.refusal(stream(pair(62, 3)), 'CHANNEL_STATE')
        self.refusal(stream(pair(48, 0)), 'SUBBAND_STATE')
        self.refusal(stream(pair(14, 2)+pair(48, 2)), 'SUBBAND_STATE')
        self.assertEqual(v.inspect_bytes(stream(pair(14, 2)+pair(48, 1)+pair(62, 2)), [1])['status'], 'PASS')

    def test_chunk_bounds(self):
        self.refusal(stream(chunk=pair(0x6000, 2)+b'abcd'), 'CHUNK_BOUNDS')
        self.refusal(stream(chunk=pair(0x6100, 0)), 'RESERVED_LARGE_CHUNK')
        self.refusal(stream()+pair(20, 1), 'BODY_TAG_UNSUPPORTED')
        self.assertEqual(v.inspect_bytes(stream(chunk=pair(-0x4004, 1)+b'abcd'), [1])['chunks'], 1)
        self.assertEqual(v.inspect_bytes(stream(chunk=pair(0x6001, 0)+bytes(65536*4)), [1])['chunks'], 1)

    def test_legacy_claim(self):
        c = context()
        data = bytes.fromhex('00010009')
        self.assertEqual(v.evidence(data, c)['verdict'], 'FAIL')
        for claim in ('legacy-cfhd', 'identify'):
            c['claim'] = claim
            self.assertEqual(v.evidence(data, c)['verdict'], 'NOT_APPLICABLE')
        c['claim'] = 'legacy-cfhd'
        self.assertEqual(v.evidence(stream(), c)['verdict'], 'FAIL')

    def test_missing_enabled_parts(self):
        for key in ('build_parts', 'runtime_parts'):
            c = context(); c[key] = [1]
            self.assertEqual(v.evidence(stream(), c)['verdict'], 'BLOCKED')

    def test_reference_missing(self):
        for key in ('materials_available', 'inventory_complete'):
            c = context(); c['decoder'][key] = False
            self.assertEqual(v.evidence(stream(), c)['verdict'], 'BLOCKED')
        for state in ('not_run', 'unavailable'):
            c = context(); c['decoder']['state'] = state
            self.assertEqual(v.evidence(stream(), c)['verdict'], 'BLOCKED')

    def test_warning_error_success_false_pass(self):
        for key, value in (('warnings', 1), ('errors', 1), ('exit_code', 1), ('pixels_equal', False)):
            c = context(); c['decoder'][key] = value
            self.assertEqual(v.evidence(stream(), c)['verdict'], 'FAIL')
        c = context(); c['decoder']['qualified'] = False
        self.assertEqual(v.evidence(stream(), c)['verdict'], 'UNQUALIFIED')
        self.assertEqual(next(x['reason'] for x in v.evidence(stream(), c)['checks'] if x['claim'] == 'reference_decode'), 'UNQUALIFIED_REFERENCE')

    def test_coverage_and_claims(self):
        c = context(); c['parts'] = list(v.PARTS); c['build_parts'] = c['parts']; c['runtime_parts'] = c['parts']
        for claim in ('encoder', 'decoder'):
            c['acceptance_claim'] = claim
            r = v.evidence(stream(), c)
            self.assertEqual(next(x['status'] for x in r['checks'] if x['claim'] == 'coverage'), 'UNQUALIFIED')
            self.assertEqual(len(r['coverage']), 7)
            self.assertTrue(all(x['status'] == 'MISSING_COVERAGE' for x in r['coverage']))
            self.assertEqual(r['procedure']['metadata'], 'MISSING_COVERAGE')
            self.assertEqual(r['procedure']['section'], '9.2' if claim == 'encoder' else '9.3')
            self.assertEqual(r['verdict'], 'UNQUALIFIED')

    def test_strict_context_types(self):
        changes = [('parts', [True]), ('parts', [1, 1]), ('parts', [3]), ('source', {}), ('acceptance_claim', 'both')]
        for key, value in changes:
            c = context(); c[key] = value
            with self.assertRaises(v.Refusal): v.evidence(stream(), c)
        for key, value in (('warnings', True), ('qualified', 1), ('exit_code', False), ('output_sha256', 'bad'), ('pixel_format', 'arbitrary')):
            c = context(); c['decoder'][key] = value
            with self.assertRaises(v.Refusal): v.evidence(stream(), c)

    def test_budgets(self):
        self.refusal(bytes(v.MAX_BYTES+1), 'INPUT_BUDGET')
        self.refusal(stream(chunk=pair(0x4000, 0)*v.MAX_SEGMENTS), 'SEGMENT_BUDGET')
        original = v.MAX_OUTPUT
        try:
            v.MAX_OUTPUT = 1
            with self.assertRaisesRegex(v.Refusal, 'OUTPUT_BUDGET'): v.evidence(stream(), context())
        finally: v.MAX_OUTPUT = original

    def cli(self, data, c):
        with tempfile.TemporaryDirectory() as directory:
            a = Path(directory)/'sample'; b = Path(directory)/'context'
            a.write_bytes(data); b.write_text(json.dumps(c) if not isinstance(c, str) else c)
            return subprocess.run([sys.executable, str(TOOL), '--input', str(a), '--context', str(b)], capture_output=True, timeout=5)

    def test_cli_exit_and_finite_output(self):
        for c, expected in ((context(), 3), (dict(context(), runtime_parts=[]), 2)):
            r = self.cli(stream(), c)
            self.assertEqual(r.returncode, expected)
            self.assertLess(len(r.stdout), v.MAX_OUTPUT)
            self.assertFalse(r.stderr)
        self.assertEqual(self.cli(b'bad!', context()).returncode, 1)
        self.assertEqual(self.cli(stream(), '{"claim":1,"claim":2}').returncode, 2)
        self.assertEqual(self.cli(stream(), '['*1000).returncode, 2)

    def test_privacy(self):
        sentinel = 'DO_NOT_ECHO_PRIVATE_SENTINEL'
        r = self.cli(sentinel.encode(), dict(context(), claim=sentinel))
        self.assertNotIn(sentinel.encode(), r.stdout+r.stderr)
        self.assertNotIn(str(TOOL.parent).encode(), r.stdout+r.stderr)
        self.assertEqual(r.returncode, 2)

    def test_refusal_classification(self):
        cases = [(stream()+pair(62, 1), 'UNQUALIFIED', 'BODY_TAG_UNSUPPORTED'),
                 (stream(pair(999, 1)), 'UNQUALIFIED', 'HEADER_TAG'),
                 (stream(pair(101, 17)), 'UNQUALIFIED', 'PRECISION_RANGE'),
                 (stream(pair(84, 5)), 'UNQUALIFIED', 'IMAGE_FORMAT_RANGE'),
                 (b'VC-5'+pair(12, 5), 'UNQUALIFIED', 'CHANNEL_RANGE'),
                 (b'VC-5'+bytes(v.MAX_BYTES), 'BLOCKED', 'INPUT_BUDGET'),
                 (stream(chunk=pair(0x4000, 0)*v.MAX_SEGMENTS), 'BLOCKED', 'SEGMENT_BUDGET')]
        for data, status, reason in cases:
            with self.subTest(reason=reason):
                r = v.evidence(data, context())
                self.assertEqual(r['syntax']['dialect'], 'vc5')
                self.assertEqual(r['syntax']['status'], status)
                self.assertEqual(r['verdict'], status)
                self.assertEqual(r['reason'], reason)
                self.assertEqual(r['claim_classes'][0]['status'], status)

    def test_known_dialect_failure(self):
        for data, reason in ((stream()+b'x', 'TRUNCATED_SEGMENT'),
                             (stream(chunk=pair(0x6000, 2)), 'CHUNK_BOUNDS'),
                             (stream(pair(20, 1)), 'DUPLICATE_HEADER')):
            r = v.evidence(data, context())
            self.assertEqual(r['syntax']['dialect'], 'vc5')
            self.assertEqual((r['verdict'], r['reason']), ('FAIL', reason))

    def test_cli_refusal_classification(self):
        r = self.cli(stream()+pair(62, 1), context())
        self.assertEqual(r.returncode, 3)
        report = json.loads(r.stdout)
        self.assertEqual(report['syntax']['dialect'], 'vc5')
        self.assertEqual(report['verdict'], 'UNQUALIFIED')
        r = self.cli(stream(chunk=pair(0x4000, 0)*v.MAX_SEGMENTS), context())
        self.assertEqual(r.returncode, 2)
        self.assertEqual(json.loads(r.stdout)['verdict'], 'BLOCKED')

    def test_cli_private_arguments(self):
        sentinel = 'PRIVATE_ARGUMENT_SENTINEL'
        private = '/private/'+sentinel+'/credential'
        arguments = [['--'+sentinel], ['--input'], ['--input', '--'+sentinel],
                     ['--input='+private], ['--input', 'sample', '--context', 'context', '--'+sentinel+'='+private],
                     ['--input', 'sample', '--context', 'context', '--'+sentinel+'='+('X'*16384)],
                     ['--inp='+private, '--context=context']]
        for args in arguments:
            r = subprocess.run([sys.executable, str(TOOL)]+args, capture_output=True, timeout=5)
            self.assertEqual(r.returncode, 2)
            self.assertNotIn(sentinel.encode(), r.stdout+r.stderr)
            self.assertNotIn(private.encode(), r.stdout+r.stderr)
            self.assertLess(len(r.stdout+r.stderr), 256)
            self.assertEqual(json.loads(r.stdout)['error'], 'INSPECTION_REFUSED')

    def test_sanitized_parser_in_process(self):
        sentinel = '--PRIVATE_ARGUMENT_SENTINEL'
        output, errors = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            try:
                code = v.main(['--input', 'sample', '--context', 'context', sentinel])
            except SystemExit as exc:
                code = exc.code
        self.assertEqual(code, 2)
        self.assertNotIn(sentinel, output.getvalue()+errors.getvalue())
        self.assertEqual(json.loads(output.getvalue())['error'], 'INSPECTION_REFUSED')
        self.assertLess(len(output.getvalue()+errors.getvalue()), 256)

    def test_cli_help(self):
        r = subprocess.run([sys.executable, str(TOOL), '--help'], capture_output=True, timeout=5)
        self.assertEqual(r.returncode, 0)
        self.assertIn(b'--input', r.stdout)
        self.assertIn(b'--context', r.stdout)
        self.assertFalse(r.stderr)
        self.assertLess(len(r.stdout), 2048)

    def test_bounded_read_request(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)/'input'; p.write_bytes(b'abc')
            real = os.fdopen
            requests = []
            class Spy:
                def __init__(self, fd, mode): self.file = real(fd, mode)
                def __enter__(self): return self
                def __exit__(self, *args): self.file.close()
                def fileno(self): return self.file.fileno()
                def read(self, size=-1):
                    requests.append(size)
                    if size != 4: raise AssertionError('unbounded read')
                    return self.file.read(size)
            with mock.patch.object(v.os, 'fdopen', Spy):
                self.assertEqual(v.read_bounded(p, 3), b'abc')
            self.assertEqual(requests, [4])

    def test_nonregular_and_bounded_file(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)/'input'; p.write_bytes(b'abc')
            with self.assertRaises(v.Refusal): v.read_bounded(p, 2)
            self.assertEqual(v.read_bounded(p, 3), b'abc')
            if hasattr(os, 'mkfifo'):
                fifo = Path(directory)/'fifo'; os.mkfifo(fifo)
                with self.assertRaises(v.Refusal): v.read_bounded(fifo, 3)


    def test_nonregular_descriptor_rejected_before_read(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)/'input'; p.write_bytes(b'abc')
            real_fdopen = os.fdopen
            reads = []
            class Spy:
                def __init__(self, fd, mode): self.file = real_fdopen(fd, mode)
                def __enter__(self): return self
                def __exit__(self, *args): self.file.close()
                def fileno(self): return self.file.fileno()
                def read(self, size=-1):
                    reads.append(size)
                    return self.file.read(size)
            with mock.patch.object(v.os, 'fdopen', Spy), mock.patch.object(v.os, 'fstat') as fstat:
                fstat.return_value.st_mode = 0
                with self.assertRaisesRegex(v.Refusal, '^REGULAR_FILE_REQUIRED$'):
                    v.read_bounded(p, 3)
                fstat.assert_called_once()
            self.assertEqual(reads, [])


if __name__ == '__main__':
    unittest.main()
