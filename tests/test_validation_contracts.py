# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Fault injection for the five portable macOS validation contracts."""
import copy
import base64
import json
from pathlib import Path
import subprocess
import sys
import struct
import tempfile
import unittest
import validate_windows_reference as windows
import validate_quality_state_corpus as quality
import compare_sdk_oracle as oracle
import validate_filmscan3 as filmscan
from validation_support import p10_samples


class FrameContract(unittest.TestCase):
    def test_frame_types_and_hashes(self):
        manifest = windows.load_manifest(windows.CORPUS / 'manifest.json')
        frame = next(f for f in manifest['frames'] if f['width'] == 1920)
        for field, value in [('width', 1920.0), ('normalized_equal', 'false'),
                             ('input_sha256', 'bad'), ('frame_index', True)]:
            with self.subTest(field=field):
                altered = copy.deepcopy(frame)
                altered[field] = value
                with self.assertRaises(ValueError):
                    windows.validate_frame(altered)
        altered = copy.deepcopy(frame)
        altered['bands'][0]['quant'] = True
        altered['quant'][8] = True
        with self.assertRaises(ValueError):
            windows.validate_frame(altered)


class QualityContract(unittest.TestCase):
    def test_typed_probe(self):
        valid = dict(limiter=0, overbitrate=0, quant=[1]*36, scale=[1]*36)
        quality.validate_probe(valid)
        for altered in ({}, dict(valid, extra=0), dict(valid, quant=None)):
            with self.assertRaises(ValueError):
                quality.validate_probe(altered)
        for field, value in [('limiter', False), ('overbitrate', -1),
                             ('quant', [True]*36), ('scale', [1]*35)]:
            with self.subTest(field=field):
                invalid = dict(valid, **{field: value})
                with self.assertRaises(ValueError):
                    quality.validate_probe(invalid)

    def test_sequence_order(self):
        frames = [dict(frame_index=i, width=1920, height=1080,
                       previous_sample_bytes=0, fs_rate_limiter=0,
                       quant=[1]*36, scale=[1]*36) for i in range(16)]
        quality.validate_frames(frames, '1080p-16f')
        for field, value in [('frame_index', True), ('width', 1920.0),
                             ('previous_sample_bytes', -1)]:
            invalid = copy.deepcopy(frames)
            invalid[1][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                quality.validate_frames(invalid, '1080p-16f')
        with self.assertRaises(ValueError):
            quality.validate_frames(list(reversed(frames)), '1080p-16f')


class OptimizedGateContract(unittest.TestCase):
    def test_external_gate_keeps_all_acceptance_checks_under_optimization(self):
        # A static guard supplements the optimized-process fault injection:
        # no production acceptance check may disappear under Python -O.
        import ast
        source = Path(windows.ROOT/'tests/validate_standalone.py').read_text()
        self.assertFalse(any(isinstance(n, ast.Assert) for n in ast.walk(ast.parse(source))))
        result = subprocess.run([sys.executable, '-O', '-c',
            'from validation_support import require; require(False, "bad pixels")'],
            cwd=windows.ROOT/'tests', capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('bad pixels', result.stderr)


    def test_real_external_gate_rejects_injected_failures_under_optimization(self):
        program = r"""
import json, sys, tempfile
from pathlib import Path
from unittest.mock import patch
import validate_standalone as gate
kind = sys.argv[1]
with tempfile.TemporaryDirectory() as tmp:
    output = Path(tmp)
    calls = 0
    def run(command, **kwargs):
        global calls
        path = Path(command[-1])
        if command[0] == 'encoder':
            calls += 1
            Path(command[2]).write_bytes(b'packet' + (bytes([calls]) if kind == 'repeat' else b''))
        else:
            # Correct byte count, but deliberately invalid/poor decoded samples.
            value = b'\x00\x04' if kind == 'range' else b'\x00\x00'
            path.write_bytes(value * (128*24*2))
    stream = dict(codec_name='wrong' if kind == 'format' else 'cfhd',
                  width=128, height=24, pix_fmt='yuv422p10le')
    with patch.object(sys, 'argv', ['validator', '--encoder', 'encoder', '--ffmpeg', 'ffmpeg',
         '--ffprobe', 'ffprobe', '--width', '128', '--height', '24', '--patterns', 'flat',
         '--output-dir', str(output)]), patch.object(gate.subprocess, 'run', side_effect=run), \
         patch.object(gate.subprocess, 'check_output', return_value=json.dumps(dict(streams=[stream])).encode()):
        gate.main()
"""
        for kind in ('repeat', 'format', 'range', 'quality'):
            result = subprocess.run([sys.executable, '-O', '-c', program, kind],
                                    cwd=windows.ROOT/'tests', capture_output=True, text=True)
            with self.subTest(kind=kind):
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('ValueError', result.stderr)


class OracleContract(unittest.TestCase):
    def test_p10_range_is_binding_even_for_identical_pixels(self):
        for value in (1024, 65535):
            invalid = struct.pack('<32768H', *([value]*32768))
            valid = struct.pack('<32768H', *([1023]*32768))
            for actual, expected in ((invalid, invalid), (invalid, valid), (valid, invalid)):
                with self.subTest(value=value), self.assertRaises(ValueError):
                    oracle.p10_metrics(actual, expected)

    def test_metric_refuses_truncated_planes(self):
        result = subprocess.run([sys.executable, '-O', '-c',
            'from compare_sdk_oracle import metric; metric([0], [0, 1023])'],
            cwd=windows.ROOT/'tests', capture_output=True)
        self.assertNotEqual(result.returncode, 0)

    def test_log_names_cannot_write_outside_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)/'output'; root.mkdir()
            lines = ['ORACLE_DATA_BEGIN ../escaped 1', 'ORACLE_DATA eA==',
                     'ORACLE_DATA_END ../escaped']
            with self.assertRaises(ValueError):
                oracle.extract_log(lines, root)
            self.assertFalse((root.parent/'escaped').exists())

    def test_valid_inventory_and_rejected_record_states(self):
        fixtures = windows.ROOT/'tests/fixtures/windows-oracle'
        lines = []
        for name in sorted(oracle.ORACLE_NAMES):
            raw = (fixtures/name).read_bytes()
            lines += ['ORACLE_DATA_BEGIN %s %d' % (name, len(raw)),
                      'ORACLE_DATA ' + base64.b64encode(raw).decode('ascii'),
                      'ORACLE_DATA_END ' + name]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            data = oracle.extract_log(lines, root)
            self.assertEqual(set(data), oracle.ORACLE_NAMES)
            for name in data:
                self.assertEqual((root/name).read_bytes(), (fixtures/name).read_bytes())
        cases = [lines + lines[:3], ['ORACLE_DATA eA=='],
                 ['ORACLE_DATA_BEGIN control.cfhd 16777217'],
                 ['ORACLE_DATA_BEGIN control.cfhd 1', 'ORACLE_DATA eA==eA=='],
                 ['ORACLE_DATA_BEGIN control.cfhd 1', 'ORACLE_DATA_BEGIN flat.sdk-p10 65536'],
                 ['ORACLE_DATA_END control.cfhd']]
        for invalid in cases:
            with tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                with self.assertRaises(ValueError):
                    oracle.extract_log(invalid, root)
                self.assertEqual(list(root.iterdir()), [])

    def test_valid_and_incomplete_log(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            lines = ['ORACLE_DATA_BEGIN flat.sdk-p10 65536',
                     'ORACLE_DATA_END flat.sdk-p10']
            with self.assertRaises(ValueError):
                oracle.extract_log(lines, root)
            with self.assertRaises(ValueError):
                oracle.extract_log(lines[:1], root)


class PixelStorageContract(unittest.TestCase):
    def test_little_endian_and_all_plane_boundaries(self):
        values = [0, 1, 255, 256, 511, 512, 1022, 1023]
        raw = struct.pack('<8H', *values)
        self.assertEqual(list(p10_samples(raw, 2, 2)), values)
        self.assertIsNone(filmscan.decoded_psnr(raw, raw, 2, 2))
        for index in range(8):
            bad = values[:]
            bad[index] = 1024
            invalid = struct.pack('<8H', *bad)
            for actual, reference in ((invalid, raw), (raw, invalid), (invalid, invalid)):
                with self.subTest(index=index), self.assertRaises(ValueError):
                    filmscan.decoded_psnr(actual, reference, 2, 2)

    def test_truncated_extra_and_empty_storage(self):
        raw = bytes(16)
        for invalid in (b'', raw[:-1], raw[:-2], raw + b'\x00\x00', raw + raw):
            for actual, reference in ((invalid, raw), (raw, invalid), (invalid, invalid)):
                with self.subTest(length=len(invalid)), self.assertRaises(ValueError):
                    filmscan.decoded_psnr(actual, reference, 2, 2)

    def test_quality_threshold_still_binding(self):
        black = bytes(16)
        white = struct.pack('<8H', *([1023]*8))
        with self.assertRaises(ValueError):
            filmscan.decoded_psnr(black, white, 2, 2)


class ProvenanceContract(unittest.TestCase):
    def test_command_omits_external_paths_and_values(self):
        from validation_support import portable_command
        args = ['--encoder', str(Path(tempfile.gettempdir())/'local-tool'),
                '--sequence', '--corpus-root', 'private-relative-corpus',
                '--frames', '0', '3', '--report=private-output.json']
        got = portable_command('validate_filmscan3.py', args)
        text = json.dumps(got)
        self.assertNotIn('local-tool', text)
        self.assertNotIn('private-', text)
        self.assertIn('3', got)
        self.assertIn('--sequence', got)
        self.assertIn('${HYPERCINE_GOLDEN_DIR}', text)


if __name__ == '__main__':
    unittest.main()
