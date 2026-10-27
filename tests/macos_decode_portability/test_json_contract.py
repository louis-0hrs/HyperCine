# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Exercise manifest parsing through the real baseline entry point."""
import contextlib
import copy
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import baseline


class JsonContract(unittest.TestCase):
    def setUp(self):
        self.report = dict(schema=1, fixture_sha256='fixture',
                           stream={'width': 1920}, packets=[],
                           output={'frame_count': 24})

    def verify(self, text, accepted=False):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'baseline.json'
            path.write_text(text, encoding='utf-8')
            stderr = io.StringIO()
            with patch.object(sys, 'argv', ['baseline.py', '--ffmpeg', 'mock',
                    '--ffprobe', 'mock', '--verify', str(path)]), \
                    patch.object(baseline, 'capture', return_value=self.report), \
                    contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(stderr):
                if accepted:
                    baseline.main()
                else:
                    with self.assertRaises(SystemExit) as error:
                        baseline.main()
                    self.assertEqual(error.exception.code, 1)
                    self.assertEqual(stderr.getvalue(),
                        'FAIL: oracle unavailable, invalid fixture, decode error or baseline mismatch\n')

    def test_valid_reordered_object(self):
        self.verify(json.dumps(dict(reversed(list(self.report.items())))), True)

    def test_numeric_types_cannot_alias(self):
        for key, field, value in [('schema', None, True), ('schema', None, 1.0),
                ('stream', 'width', 1920.0), ('output', 'frame_count', 24.0)]:
            report = copy.deepcopy(self.report)
            if field is None:
                report[key] = value
            else:
                report[key][field] = value
            with self.subTest(key=key, field=field, value=value):
                self.verify(json.dumps(report))

    def test_duplicate_keys_at_each_depth(self):
        raw = json.dumps(self.report)
        for old, new in [('"schema": 1', '"schema": 9, "schema": 1'),
                ('"width": 1920', '"width": 1, "width": 1920'),
                ('"frame_count": 24', '"frame_count": 0, "frame_count": 24')]:
            with self.subTest(field=old):
                self.verify(raw.replace(old, new))

    def test_wrong_root_and_nonstandard_constants(self):
        for raw in ['[]', 'null', 'true', '1', '"text"',
                    json.dumps(self.report)[:-1] + ', "ignored": NaN}',
                    json.dumps(self.report)[:-1] + ', "ignored": Infinity}',
                    json.dumps(self.report)[:-1] + ', "ignored": -Infinity}']:
            with self.subTest(raw=raw):
                self.verify(raw)

    def test_deeply_nested_baseline_fails_without_traceback(self):
        # JSON decoders do not share one recursion threshold across Python versions.
        # Damage a compared field so rejection also holds when parsing succeeds.
        nested = '[' * 2048 + '0' + ']' * 2048
        raw = json.dumps(self.report).replace('"frame_count": 24',
                                              '"frame_count": ' + nested)
        self.verify(raw)

    def test_json_recursion_failure_is_sanitized(self):
        # Exercise exhaustion deterministically, independent of the parser limit.
        raw = json.dumps(self.report)
        with patch.object(baseline.json, 'loads',
                          side_effect=RecursionError('SYNTHETIC_PRIVATE_EVIDENCE')):
            self.verify(raw)

    def test_malformed_tool_json_fails_without_traceback(self):
        import subprocess
        for raw in [b'[]', b'{"streams":[],"streams":[]}',
                    b'{"ignored":NaN}', b'\xff']:
            with self.subTest(raw=raw), \
                    patch.object(sys, 'argv', ['baseline.py', '--ffmpeg', 'mock', '--ffprobe', 'mock']), \
                    patch.object(baseline.subprocess, 'run', return_value=
                        subprocess.CompletedProcess([], 0, stdout=raw, stderr=b'')), \
                    contextlib.redirect_stderr(io.StringIO()) as stderr:
                with self.assertRaises(SystemExit) as error:
                    baseline.main()
                self.assertEqual(error.exception.code, 1)
                self.assertNotIn('Traceback', stderr.getvalue())


if __name__ == '__main__':
    unittest.main()
