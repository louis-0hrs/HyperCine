#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Exercise actual source replay and reject corrupt evidence/absent output."""
import contextlib
import copy
import hashlib
import io
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock
from validate_rgb444_rg48 import checked_artifact, replay

PROBE = None


class ReplayTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='hypercine-rg48-test-')
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        source = struct.pack('<3H', 0, 15, 65535) + bytes([0xee])*10
        source += struct.pack('<3H', 16, 31, 32768) + bytes([0xee])*10
        self.case = {'case_id': 'SYNTHETIC_SOURCE_ONLY', 'width': 1, 'height': 2,
                     'source_pitch': 16, 'artifacts': {}, 'canonical_p12_le_planes': []}
        for name, raw in [('source.bin', source), ('sdk.cfhd', b'synthetic packet'),
                          ('sdk.normalized.cfhd', b'synthetic normalized'),
                          ('decoded.bin', b'synthetic decoded')]:
            (self.root/name).write_bytes(raw)
            self.case['artifacts'][name] = {'path': name, 'byte_length': len(raw),
                                            'sha256': hashlib.sha256(raw).hexdigest()}
        for channel, values in [('red', (0, 1)), ('green', (0, 1)), ('blue', (4095, 2048))]:
            self.case['canonical_p12_le_planes'].append({
                'channel': channel, 'byte_length': 4,
                'sha256': hashlib.sha256(struct.pack('<2H', *values)).hexdigest()})
        self.pins = {'manifests': [], 'cases': [self.case]}

    def run_replay(self):
        with contextlib.redirect_stdout(io.StringIO()):
            return replay(self.root, PROBE, self.pins)

    def test_actual_probe_known_source_and_padding(self):
        result = self.run_replay()
        self.assertEqual(result['samples_compared'], 6)
        self.assertEqual(result['artifacts_verified'], 4)
        self.assertEqual(result['status'], 'SOURCE_IMPORT_PASS')

    def test_every_corrupt_artifact_rejected_before_probe(self):
        for name in self.case['artifacts']:
            path = self.root/name
            original = path.read_bytes()
            for corrupted in (original[:-1], bytes([original[0] ^ 1]) + original[1:]):
                with self.subTest(name=name, length=len(corrupted)):
                    path.write_bytes(corrupted)
                    with mock.patch('validate_rgb444_rg48.subprocess.run') as run:
                        with self.assertRaises(ValueError):
                            self.run_replay()
                        run.assert_not_called()
            path.write_bytes(original)

    def test_success_without_new_output_rejected(self):
        # Both repeat invocations have to create a fresh file.
        self.run_replay()
        with mock.patch('validate_rgb444_rg48.subprocess.run'):
            with self.assertRaises(FileNotFoundError):
                self.run_replay()

    def test_wrong_output_extent_or_hash_rejected(self):
        for raw in (b'', bytes(12)):
            def fake_run(command, **kwargs):
                Path(command[-1]).write_bytes(raw)
            with self.subTest(length=len(raw)), mock.patch(
                    'validate_rgb444_rg48.subprocess.run', side_effect=fake_run):
                with self.assertRaises(ValueError):
                    self.run_replay()

    def test_relative_artifact_domain(self):
        for path in ('../outside', '/outside', 'C:/outside', 'folder\\outside'):
            record = dict(self.case['artifacts']['source.bin'], path=path)
            with self.subTest(path=path), self.assertRaises(ValueError):
                checked_artifact(self.root, record)

    def test_invalid_geometry_and_empty_replay(self):
        for key, value in [('width', True), ('height', 0), ('source_pitch', 15),
                           ('width', 4097), ('source_pitch', 16*1024*1024)]:
            pins = copy.deepcopy(self.pins)
            pins['cases'][0][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                replay(self.root, PROBE, pins)
        with self.assertRaises(ValueError):
            replay(self.root, PROBE, {'manifests': [], 'cases': []})


if __name__ == '__main__':
    if len(sys.argv) != 3 or sys.argv[1] != '--probe':
        raise SystemExit('expected --probe executable')
    PROBE = Path(sys.argv[2]).resolve()
    unittest.main(argv=[sys.argv[0]])
