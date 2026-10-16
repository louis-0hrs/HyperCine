# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
import math
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import json

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from compare_cfhd_decoders import metrics, unpack_plane, planes_p10, checked_files, sha256


class MetricsTests(unittest.TestCase):
    def test_hand_calculated_signed_errors(self):
        result = metrics([0, 5, 4, 9], [1, 3, 4, 8], 2)
        self.assertEqual(result['exact_samples'], 1)
        self.assertEqual(result['exact_fraction'], .25)
        self.assertEqual(result['max_abs_error'], 2)
        self.assertEqual(result['mean_abs_error'], 1)
        self.assertEqual(result['bias'], .5)
        self.assertAlmostEqual(result['rmse'], math.sqrt(1.5))
        self.assertAlmostEqual(result['psnr_db'], 10*math.log10(1023**2/1.5))
        self.assertEqual(result['error_histogram'], {'-1': 1, '1': 1, '2': 1})
        self.assertEqual(result['first_32_differences'][1],
                         dict(x=1, y=0, actual=5, reference=3, difference=2))

    def test_exact_and_border_interior_accounting(self):
        result = metrics([12]*9, [12]*9, 3)
        self.assertIsNone(result['psnr_db'])
        self.assertTrue(result['psnr_infinite'])
        self.assertEqual(result['bias'], 0)
        reference = [0]*9
        result = metrics([1]*9, reference, 3)
        self.assertEqual((result['border_mismatches'], result['interior_mismatches']), (8, 1))
        self.assertEqual((result['affected_rows'], result['affected_columns']), (3, 3))
        self.assertEqual(len(metrics([1]*40, [0]*40, 8)['first_32_differences']), 32)

    def test_endian_stride_orientation_and_unsigned_extremes(self):
        for endian, prefix in [('little', '<'), ('big', '>')]:
            raw = struct.pack(prefix+'6H', 0, 1023, 65535, 512, 64, 65535)
            self.assertEqual(unpack_plane(raw, 2, 2, 6, endian), [0, 1023, 512, 64])
            self.assertEqual(unpack_plane(raw, 2, 2, 6, endian, True), [512, 64, 0, 1023])

    def test_planar_chroma_order(self):
        raw = struct.pack('<8H', 1, 2, 3, 4, 100, 101, 900, 901)
        planes = planes_p10(raw, 2, 2)
        self.assertEqual(planes['Y'], ([1, 2, 3, 4], 2))
        self.assertEqual(planes['Cb'], ([100, 101], 1))
        self.assertEqual(planes['Cr'], ([900, 901], 1))

    def test_reject_incomplete_or_out_of_range(self):
        for a, b, w in [([], [], 1), ([0], [0, 0], 1), ([0]*3, [0]*3, 2),
                        ([-1], [0], 1), ([1024], [0], 1), ([0], [0], 0)]:
            with self.assertRaises(ValueError):
                metrics(a, b, w)
        for raw, w, h, stride, endian in [(b'\0'*4, 2, 2, 4, 'little'),
                                         (b'\0'*4, 2, 1, 3, 'little'),
                                         (b'\0'*4, 2, 1, 4, 'native'),
                                         (b'\xff\xff', 1, 1, 2, 'little')]:
            with self.assertRaises(ValueError):
                unpack_plane(raw, w, h, stride, endian)
        with self.assertRaises(ValueError):
            planes_p10(b'\0'*16, 3, 2)
        with self.assertRaises(ValueError):
            planes_p10(b'\0'*17, 2, 2)

    def test_fixture_integrity_and_manifest_names(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root/'sample').write_bytes(b'abc')
            record = dict(file='sample', bytes=3, sha256=sha256(b'abc'))
            def write(records):
                (root/'manifest.json').write_text(json.dumps(dict(files=records)))
            write([record])
            self.assertEqual(checked_files(root)[1]['sample'], b'abc')
            (root/'sample').write_bytes(b'abd')
            with self.assertRaises(ValueError):
                checked_files(root)
            for name in ('../sample', '/sample'):
                write([dict(record, file=name)])
                with self.assertRaises(ValueError):
                    checked_files(root)
            (root/'sample').write_bytes(b'abc')
            write([record, record])
            with self.assertRaises(ValueError):
                checked_files(root)


if __name__ == '__main__':
    unittest.main()
