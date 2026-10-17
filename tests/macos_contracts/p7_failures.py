#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Fault-inject the existing P7.A oracle; never substitute a codec decoder."""
import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('p7_baseline', sys.argv.pop(1))
baseline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(baseline)


class BaselineFailures(unittest.TestCase):
    def test_each_plane_rejects_out_of_range_and_endian_swap(self):
        for index in range(8):
            for value in [1024, 65535]:
                values = [1] * 8
                values[index] = value
                with self.subTest(index=index, value=value):
                    with self.assertRaises(ValueError):
                        baseline.summarize_frame(struct.pack('<8H', *values), 2, 2)
        # A LE 10-bit value 4 becomes 1024 on byte swap.
        with self.assertRaises(ValueError):
            baseline.summarize_frame(struct.pack('>8H', *([4] * 8)), 2, 2)

    def test_all_truncated_lengths_and_extra_frame(self):
        raw = struct.pack('<8H', *range(8))
        for length in range(len(raw)):
            with self.assertRaises(ValueError):
                baseline.summarize_frame(raw[:length], 2, 2)
        with self.assertRaises(ValueError):
            baseline.summarize_frame(raw + raw, 2, 2)

    def test_every_verified_component_is_binding(self):
        expected = dict(schema=1, fixture_sha256='fixture', stream={'width': 2},
                        packets=[{'data_hash': 'packet'}], output={'frame_count': 1})
        for key in expected:
            for missing in [False, True]:
                changed = copy.deepcopy(expected)
                if missing:
                    del changed[key]
                else:
                    changed[key] = None
                with self.subTest(key=key, missing=missing):
                    self.verify(expected, changed, should_pass=False)
        self.verify(expected, expected, should_pass=True)

    def verify(self, expected, actual, should_pass):
        with tempfile.TemporaryDirectory() as tmp:
            manifest = Path(tmp) / 'baseline.json'
            manifest.write_text(json.dumps(expected))
            argv = ['baseline.py', '--ffmpeg', 'test-ffmpeg', '--ffprobe',
                    'test-ffprobe', '--verify', str(manifest)]
            with patch.object(sys, 'argv', argv), patch.object(baseline, 'capture', return_value=actual), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                if should_pass:
                    baseline.main()
                else:
                    with self.assertRaises(SystemExit) as error:
                        baseline.main()
                    self.assertEqual(error.exception.code, 1)

    def test_unavailable_oracle_is_failure(self):
        for failure in [FileNotFoundError(), subprocess.TimeoutExpired('test', 1),
                        subprocess.CalledProcessError(1, 'test'), ValueError('invalid stream')]:
            output = io.StringIO()
            with self.subTest(failure=type(failure).__name__), \
                    patch.object(sys, 'argv', ['baseline.py', '--ffmpeg', 'missing', '--ffprobe', 'missing']), \
                    patch.object(baseline, 'capture', side_effect=failure), \
                    contextlib.redirect_stderr(output):
                with self.assertRaises(SystemExit) as error:
                    baseline.main()
                self.assertEqual(error.exception.code, 1)
                self.assertIn('FAIL:', output.getvalue())


if __name__ == '__main__':
    unittest.main()
