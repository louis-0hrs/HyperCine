# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Synthetic protocol controls; these do not assert original SDK equality."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import validate_spatial_attachment_144 as replay
import test_scalar_inverse_replay as old_contract

PROBE = None


class Protocol(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.case = {'sdk_channel': 0, 'inputs': [{'stage': {'width': 16, 'height': 4}}] * 4}
        self.files = []
        for band in range(4):
            path = self.root / f'band{band}'
            path.write_bytes((b'\xa0\x0f' if band == 0 else b'\0\0') * 64)
            self.files.append((path, replay.original.digest(path)))
        expected = self.root / 'expected'
        expected.write_bytes(b'\xe8\x03' * 256)  # Analytic DC: 4000 -> 1000.
        self.files.append((expected, replay.original.digest(expected)))

    def test_actual_worker_matrix_and_identity_controls(self):
        for workers in replay.WORKERS:
            for channel in range(3):
                self.case['sdk_channel'] = channel
                for mode in ('normal', 'wrong-band', 'wrong-channel'):
                    with self.subTest(workers=workers, channel=channel, mode=mode):
                        status = replay.execute(PROBE, self.case, self.files,
                            self.root / f'{workers}-{channel}-{mode}', workers, mode)
                        self.assertEqual(status, 0 if mode == 'normal' else 2)

    def test_wrong_expected_bytes_fail(self):
        path = self.files[4][0]
        path.write_bytes(b'\xe9\x03' * 256)
        self.files[4] = (path, replay.original.digest(path))
        with self.assertRaises(ValueError):
            replay.execute(PROBE, self.case, self.files, self.root / 'out', 1, 'normal')

    def test_short_input_fails(self):
        self.files[0][0].write_bytes(b'\0\0')
        with self.assertRaises(ValueError):
            replay.execute(PROBE, self.case, self.files, self.root / 'out', 1, 'normal')
        self.assertFalse((self.root / 'out').exists())

    def test_existing_output_is_unchanged(self):
        out = self.root / 'out'
        out.mkdir()
        sentinel = out / 'stage.i16le'
        sentinel.write_bytes(b'preserve')
        with self.assertRaises(ValueError):
            replay.execute(PROBE, self.case, self.files, out, 1, 'normal')
        self.assertEqual(sentinel.read_bytes(), b'preserve')

    def test_accidental_failure_is_not_identity_rejection(self):
        for status, stdout in ((1, b'S3_IDENTITY_REJECTED no_output_access=1 drained=1'), (2, b'')):
            with mock.patch.object(replay.subprocess, 'run',
                    return_value=subprocess.CompletedProcess([], status, stdout, b'')):
                with self.assertRaises(ValueError):
                    replay.execute(PROBE, self.case, self.files, self.root / 'out', 1, 'wrong-band')

    def test_missing_originals_fail_before_probe(self):
        with mock.patch.object(replay.subprocess, 'run') as run:
            with self.assertRaises(OSError):
                replay.replay(self.root, PROBE)
            run.assert_not_called()

    def test_sidecars_are_rechecked_after_execution(self):
        pins = old_contract.Contracts().synthetic_originals(self.root)
        path = self.root / 'synthetic-pins.json'
        path.write_text(replay.json.dumps(pins))
        cases = replay.original.originals(self.root, pins)
        sha_sidecar = cases[0][1][0][0].with_name(cases[0][1][0][0].name + '.sha256')
        def corrupt_sidecar(*args):
            sha_sidecar.write_text('changed')
            return 0
        with mock.patch.object(replay.original, 'PINS', path), \
                mock.patch.object(replay, 'execute', side_effect=corrupt_sidecar):
            with self.assertRaises(ValueError):
                replay.replay(self.root, PROBE)

    def test_invalid_worker_or_geometry_is_fatal(self):
        for width, workers in ((16, 3), (65536, 1)):
            result = subprocess.run([str(PROBE), '--replay', str(width), '4',
                *[str(p) for p, _ in self.files[:4]], '0', str(self.root / 'out'),
                str(workers), 'normal'], capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 1)
            self.assertFalse((self.root / 'out').exists())


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--probe', type=Path, required=True)
    args, remaining = p.parse_known_args()
    PROBE = args.probe.resolve()
    unittest.main(argv=[__file__] + remaining)
