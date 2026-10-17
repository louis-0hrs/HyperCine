# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
import unittest
from unittest.mock import patch
import tempfile
from pathlib import Path
import measure


class MeasurementTests(unittest.TestCase):
    def test_metric_known_delta_and_size(self):
        result = measure.metrics(bytes([0, 10, 255, 99]), bytes([0, 11, 252, 99]))
        self.assertEqual(result['changed_bytes'], 2)
        self.assertEqual(result['max_abs_delta'], 3)
        self.assertAlmostEqual(result['psnr_db'], 44.15140352195873)
        self.assertIsNone(measure.metrics(b'x', b'x')['psnr_db'])
        for a, b in ((b'', b''), (b'a', b'ab')):
            with self.assertRaises(ValueError):
                measure.metrics(a, b)

    def test_incomplete_worker_fails_closed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            def worker(command):
                if command[1] == 'generate':
                    Path(command[3]).write_bytes(b'packet')
                    Path(command[4]).write_bytes(bytes(128*128*2))
                    return b''
                return b'{}\n'
            with patch.object(measure, 'run', side_effect=worker):
                with self.assertRaises((ValueError, KeyError)):
                    measure.measure(Path('worker'), '128', 2, root)

    def test_no_alternate_or_encoder_comparison_does_not_pass(self):
        with tempfile.TemporaryDirectory() as temp:
            def worker(command):
                if command[1] == 'generate':
                    Path(command[3]).write_bytes(b'packet')
                    Path(command[4]).write_bytes(bytes(128*128*2))
                    return b''
                return b'\n'.join(measure.json.dumps(dict(ordinal=i, decoder_status=7,
                     output_valid=False)).encode() for i in range(1, 6))
            with patch.object(measure, 'run', side_effect=worker):
                report = measure.measure(Path('worker'), '128', 2, Path(temp))
            self.assertEqual(report['alternate_decoder']['status'], 'UNAVAILABLE')
            self.assertIsNone(report['encoder_packets_equal'])
            self.assertEqual(report['result']['classification'], 'REFERENCE_DECODE_ERROR')
            self.assertTrue(report['result']['release_blocked'])


if __name__ == '__main__':
    unittest.main()
