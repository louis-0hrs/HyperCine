# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
import math
import struct
import tempfile
import unittest
from collections import Counter
from pathlib import Path
from compare import compare, summarize, unpack, verified_packet, digest


class MetricsTest(unittest.TestCase):
    def test_packet_provenance_rejects_changed_bytes(self):
        frame = dict(width=32, height=16, frame_index=3, sdk_sha256=digest(b'original'))
        relative = 'frames/low-sequence-32x16/frame_0003/sdk.cfhd'
        manifest = dict(profile=dict(quality='low'), artifacts=[dict(path=relative, byte_length=8, sha256=frame['sdk_sha256'])])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root/relative
            path.parent.mkdir(parents=True)
            path.write_bytes(b'original')
            self.assertEqual(verified_packet(root, manifest, frame), b'original')
            path.write_bytes(b'modified')
            with self.assertRaises(ValueError):
                verified_packet(root, manifest, frame)

    def test_little_endian_and_bounds(self):
        raw = struct.pack('<8H', 0, 1, 256, 1023, 64, 512, 255, 1000)
        self.assertEqual(list(unpack(raw, 2, 2)), [0, 1, 256, 1023, 64, 512, 255, 1000])
        for invalid in (raw[:-1], raw + b'00', struct.pack('<8H', *([1024]*8))):
            with self.assertRaises(ValueError):
                unpack(invalid, 2, 2)
        for width, height in ((0, 1), (3, 1), (2, -1), (8000, 1)):
            with self.assertRaises(ValueError):
                unpack(b'', width, height)

    def test_known_signed_errors_and_psnr(self):
        m = summarize(Counter({-3: 1, 0: 2, 1: 1}))
        self.assertEqual(m['samples'], 4)
        self.assertEqual(m['mismatches'], 2)
        self.assertEqual(m['mean_abs'], 1)
        self.assertEqual(m['bias'], -.5)
        self.assertEqual(m['p95_abs'], 3)
        self.assertEqual(m['signed_histogram']['-3'], 1)
        self.assertAlmostEqual(m['rmse'], math.sqrt(2.5))
        self.assertAlmostEqual(m['psnr_db'], 10*math.log10(1023**2/2.5))
        self.assertIsNone(summarize(Counter({0: 10}))['psnr_db'])
        self.assertEqual(summarize(Counter({0: 95, 100: 5}))['p95_abs'], 0)

    def test_plane_coordinates_and_group_accounting(self):
        w, h = 32, 20
        a = [512]*(w*h*2)
        b = a.copy()
        a[10*w+9] = 515
        a[w*h+3*(w//2)+7] = 0
        a[w*h+w*h//2+19*(w//2)+15] = 1023
        planes = compare(struct.pack('<%dH' % len(a), *a), struct.pack('<%dH' % len(b), *b), w, h)
        self.assertEqual(planes['Y']['first_32_differences'][0], dict(x=9, y=10, gopro=515, ffmpeg=512, delta=3))
        self.assertEqual(planes['Cb']['max_abs'], 512)
        self.assertEqual(planes['Cr']['first_32_differences'][0]['delta'], 511)
        self.assertEqual(planes['Y']['groups']['border']['interior']['mismatches'], 1)
        for plane in planes.values():
            for bins in plane['groups'].values():
                self.assertEqual(sum(m['samples'] for m in bins.values()), plane['samples'])
                self.assertEqual(sum(m['mismatches'] for m in bins.values()), plane['mismatches'])

    def test_first_differences_bounded_and_exact(self):
        a, b = struct.pack('<128H', *([1]*128)), bytes(256)
        planes = compare(a, b, 8, 8)
        self.assertEqual(len(planes['Y']['first_32_differences']), 32)
        self.assertEqual(planes['Y']['first_32_differences'][-1]['y'], 3)
        self.assertFalse(compare(a, a, 8, 8)['Y']['first_32_differences'])


if __name__ == '__main__':
    unittest.main()
