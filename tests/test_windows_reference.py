#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
import copy
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import validate_windows_reference as v


class ReferenceGuardrails(unittest.TestCase):
    def setUp(self):
        self.manifest = v.load_manifest(v.CORPUS / 'manifest.json')
        self.frame = next(f for f in self.manifest['frames'] if f['width'] == 1920)

    def test_imports_and_oracles(self):
        for name, count in [('manifest.json', 16), ('phase3a_1080p_120_manifest.json', 120)]:
            frames = [f for f in v.load_manifest(v.CORPUS/name)['frames'] if f['width'] == 1920]
            self.assertEqual(len(frames), count)
            for frame in frames:
                v.validate_frame(frame)

    def test_sha_and_length_gate(self):
        for data, size in [(b'bad', 3), (b'ok', 3)]:
            with self.assertRaises(ValueError):
                v.verify_bytes(data, v.sha(b'yes'), size, 'input')

    def test_input_mismatch_never_enters_codec(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(v.subprocess, 'run') as run:
            path = Path(tmp)/'input'; path.write_bytes(b'bad')
            with self.assertRaises(ValueError):
                v.diagnose(Path('unused'), self.frame, path, Path(tmp))
            run.assert_not_called()

    def test_rejects_128_oracle(self):
        frame = next(f for f in self.manifest['frames'] if f['width'] == 128)
        with self.assertRaises(ValueError):
            v.validate_frame(frame)

    def test_quant_and_order_are_not_ignored(self):
        for field, value in [('quant', 999), ('subband', 99), ('width', 999)]:
            frame = copy.deepcopy(self.frame)
            frame['bands'][0][field] = value
            with self.assertRaises(ValueError):
                v.validate_frame(frame)

    def test_oracle_disagreement_is_rejected(self):
        frame = copy.deepcopy(self.frame)
        frame['bands'][0]['cuda_coeff_fnv64'] = '0'*16
        with self.assertRaises(ValueError):
            v.validate_frame(frame)

    def test_missing_binary_is_blocked(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = v.verify_artifacts(self.manifest, self.frame, Path(tmp))
            self.assertEqual(result['status'], 'BLOCKED')
            self.assertTrue(result['missing'])

    def test_corrupted_binary_is_not_missing_or_pass(self):
        with tempfile.TemporaryDirectory() as tmp:
            artifact = next(a for a in self.manifest['artifacts'] if 'tier-a-1080/frame_0000/sdk.cfhd' in a['path'])
            path = Path(tmp)/artifact['path']; path.parent.mkdir(parents=True); path.write_bytes(b'bad')
            with self.assertRaises(ValueError):
                v.verify_artifacts(self.manifest, self.frame, Path(tmp))

    def test_artifact_path_traversal(self):
        manifest = copy.deepcopy(self.manifest)
        manifest['artifacts'][0]['path'] = '../frames/tier-a-1080/frame_0000/sdk.cfhd'
        with tempfile.TemporaryDirectory() as tmp, self.assertRaises(ValueError):
            v.verify_artifacts(manifest, self.frame, Path(tmp))

    def test_signed_coordinate(self):
        expected = struct.pack('<4h', 0, -32768, 5, -7)
        actual = struct.pack('<4h', 0, -32768, -5, -7)
        self.assertEqual(v.first_i16_difference(expected, actual, 2),
                         {'x': 0, 'y': 1, 'expected': 5, 'actual': -5})
        self.assertIsNone(v.first_i16_difference(expected, expected, 2))
        self.assertEqual(v.first_i16_difference(expected, expected[:-2], 2), {'byte_length': [8, 6]})


if __name__ == '__main__':
    unittest.main()
