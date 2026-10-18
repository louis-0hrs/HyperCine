# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Independent fault probes for five pending macOS validation fixes.

Run each issue in a separate process against its own pinned source snapshot.
These probes qualify validation helpers, not the blocked codec consumers.
"""
import copy
import importlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ISSUE, ROOT = sys.argv[1], Path(sys.argv[2]).resolve()
sys.argv[1:] = []


def module(directory, name):
    sys.path.insert(0, str(ROOT / directory))
    return importlib.import_module(name)


class Trace(unittest.TestCase):
    def test_state_inventory_types_and_binding(self):
        target = module('tests', 'validate_filmscan3')
        manifest = json.loads((ROOT / 'tests/reference_corpus/manifest.json').read_text())
        frame = next(f for f in manifest['frames'] if f['width'] == 1920)
        row = {key: copy.deepcopy(frame[key]) for key in
               ('frame_index', 'fs_rate_limiter', 'previous_sample_bytes', 'quant', 'scale')}
        row.update(sample_bytes=frame['sdk_byte_length'],
                   coefficient_fnv64=[b['sdk_coeff_fnv64'] for b in frame['bands']])
        target.validate_trace([row], [frame])
        bad = []
        for key in ('frame_index', 'fs_rate_limiter', 'previous_sample_bytes', 'sample_bytes'):
            for value in (True, False, float(row[key]), -1, '0'):
                changed = copy.deepcopy(row)
                changed[key] = value
                bad.append(changed)
        for key in ('quant', 'scale', 'coefficient_fnv64'):
            for value in ([], row[key][:-1], row[key] + row[key][:1]):
                changed = copy.deepcopy(row)
                changed[key] = value
                bad.append(changed)
        for key in row:
            changed = copy.deepcopy(row)
            del changed[key]
            bad.append(changed)
        bad.append(dict(row, unrecognized=1))
        changed = copy.deepcopy(row)
        changed['coefficient_fnv64'][0] = '0' * 16
        bad.append(changed)
        for index, changed in enumerate(bad):
            with self.subTest(case=index), self.assertRaises(ValueError):
                target.validate_trace([changed], [frame])
        for rows, expected in (([], []), ([], [frame]), ([row, row], [frame])):
            with self.subTest(inventory=len(rows)), self.assertRaises(ValueError):
                target.validate_trace(rows, expected)


class Pixels(unittest.TestCase):
    def test_both_pixel_sources_before_metrics(self):
        target = module('tests', 'validate_filmscan3_geometry')
        valid = struct.pack('<4H', 0, 256, 512, 1023)
        self.assertTrue(math.isinf(target.decoded_psnr(valid, valid)))
        nearby = struct.pack('<4H', 1, 257, 513, 1022)
        self.assertAlmostEqual(target.decoded_psnr(valid, nearby),
                               20 * math.log10(1023), places=10)
        invalid = [b'', b'\0', valid[:-2], valid + b'\0\0',
                   struct.pack('<4H', 0, 256, 512, 1024),
                   struct.pack('<4H', 65535, 256, 512, 1023)]
        for index, data in enumerate(invalid):
            for actual, expected in ((valid, data), (data, valid)):
                with self.subTest(case=index), self.assertRaises(ValueError):
                    target.decoded_psnr(actual, expected)
        with self.assertRaises(ValueError):
            target.decoded_psnr(bytes(8), b'\xff\x03' * 4)


class MemoryModel(unittest.TestCase):
    def test_faults_fail_with_python_optimization(self):
        target = module('tests/macos_contracts', 'metal_budget')
        original = target.budget
        # A damaged estimate must make the acceptance entry point fail under -O.
        def corrupt(width, height, padding=0):
            result = original(width, height, padding)
            if padding:
                result['resident_bytes'] -= 1
            return result
        with patch.object(target, 'budget', side_effect=corrupt):
            with self.assertRaisesRegex(ValueError, 'padding accounting'):
                target.main()
        # Independent algebra for the aligned 1080p allocation inventory.
        # Input + p10 color + four bands at each of three levels + two scratch.
        w, h = 1920, 1080
        expected = w * h * 4 + w * h * 4 + w * h * 2
        for pw in (w, w // 2, w // 2):
            ph = h
            for _ in range(3):
                pw //= 2
                ph //= 2
                expected += 4 * ((pw + 7) // 8 * 8) * ph * 2
        row = original(w, h)
        self.assertEqual(row['buffer_count'], 42)
        self.assertEqual(row['resident_bytes'], expected)
        self.assertEqual(row['status'], 'ESTIMATE_ONLY')


class QualityMode(unittest.TestCase):
    def test_real_cmake_options(self):
        for option in ('HYPERCINE_BUILD_STANDALONE', 'HYPERCINE_ENABLE_METAL',
                       'HYPERCINE_ENABLE_CUDA', 'HYPERCINE_REQUIRE_EXTERNAL_DECODE'):
            with self.subTest(option=option), tempfile.TemporaryDirectory() as temporary:
                result = subprocess.run(['cmake', '-S', str(ROOT), '-B', temporary,
                    '-DHYPERCINE_BUILD_QUALITY_STATE_ONLY=ON', '-D' + option + '=TRUE'],
                    capture_output=True, text=True, timeout=120)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('Quality-state-only', result.stderr)
        with tempfile.TemporaryDirectory() as temporary:
            result = subprocess.run(['cmake', '-S', str(ROOT), '-B', temporary,
                '-DHYPERCINE_BUILD_QUALITY_STATE_ONLY=ON', '-DHYPERCINE_BUILD_TESTS=OFF'],
                capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, 'tests-OFF configuration failed')
            inventory = subprocess.check_output(
                ['ctest', '--test-dir', temporary, '--show-only=json-v1'], timeout=30)
            self.assertEqual(json.loads(inventory)['tests'], [])


class DecoderPlanes(unittest.TestCase):
    def test_geometry_rejection_and_real_plane_hashes(self):
        import hashlib
        target = module('tests/macos_decode_portability', 'baseline')
        raw = struct.pack('<8H', 1, 2, 3, 4, 5, 6, 7, 8)
        result = target.summarize_frame(raw, 2, 2)
        self.assertEqual(result['sha256'], hashlib.sha256(raw).hexdigest())
        for plane, data in zip(result['planes'], (raw[:8], raw[8:12], raw[12:])):
            self.assertEqual(plane['sha256'], hashlib.sha256(data).hexdigest())
        for w, h in ((True, 2), (2, True), (2.0, 2), (2, 2.0),
                     ('2', 2), (2, None), (0, 2), (-2, 2), (3, 2)):
            with self.subTest(width=w, height=h), self.assertRaises(ValueError):
                target.summarize_frame(raw, w, h)
        for bad in (raw[:-1], raw + b'\0', struct.pack('<8H', 1024, 2, 3, 4, 5, 6, 7, 8)):
            with self.assertRaises(ValueError):
                target.summarize_frame(bad, 2, 2)


if __name__ == '__main__':
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(
        {'23': Trace, '44': Pixels, '45': MemoryModel, '46': QualityMode, '47': DecoderPlanes}[ISSUE])
    raise SystemExit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
