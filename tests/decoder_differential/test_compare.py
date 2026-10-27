# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
import math
import struct
import tempfile
import unittest
from collections import Counter
from pathlib import Path
from compare import (align_bgra_orientation, avi_sequence, compare, compare_bgra, digest,
                     flip_vertical, manifest_root, select_filter, summarize, unpack,
                     unpack_bgra, verified_packet)
import compare as differential


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

    def test_bgra_bounds(self):
        raw = bytes(2*2*4)
        self.assertEqual(unpack_bgra(raw, 2, 2), memoryview(raw))
        for bad in (raw[:-1], raw + b'0', bytes(8000*2*4)):
            with self.assertRaises(ValueError):
                unpack_bgra(bad, 2, 2)

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
        # An 8-bit BGRA channel uses peak 255, not the p10 peak.
        self.assertAlmostEqual(summarize(Counter({1: 1}), peak=255)['psnr_db'],
                               10*math.log10(255**2))

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
            self.assertEqual(plane['detail'], 'full')
            for bins in plane['groups'].values():
                self.assertEqual(sum(m['samples'] for m in bins.values()), plane['samples'])
                self.assertEqual(sum(m['mismatches'] for m in bins.values()), plane['mismatches'])

    def test_basic_detail_is_exhaustive_and_reports_omission(self):
        w, h = 8, 4
        a = [512]*(w*h*2)
        b = a.copy()
        a[0] = 700
        original = differential.FULL_DETAIL_SAMPLES
        differential.FULL_DETAIL_SAMPLES = 8
        try:
            planes = compare(struct.pack('<%dH' % len(a), *a), struct.pack('<%dH' % len(b), *b), w, h)
        finally:
            differential.FULL_DETAIL_SAMPLES = original
        for plane in planes.values():
            self.assertEqual(plane['detail'], 'basic')
            self.assertEqual(plane['groups_omitted'], ['reference_code_bin', 'spatial_8x8'])
            self.assertNotIn('reference_code_bin', plane['groups'])
            for bins in plane['groups'].values():
                self.assertEqual(sum(m['samples'] for m in bins.values()), plane['samples'])
        self.assertEqual(planes['Y']['first_32_differences'][0],
                         dict(x=0, y=0, gopro=700, ffmpeg=512, delta=188))

    def test_bgra_channel_metrics(self):
        w, h = 2, 2
        a = bytearray(w*h*4)
        b = bytearray(w*h*4)
        a[0::4] = bytes([0, 255, 10, 10])
        a[1::4] = bytes([1, 2, 3, 4])
        a[2::4] = bytes([5, 6, 7, 8])
        a[3::4] = bytes([255]*4)
        b[3::4] = bytes([255]*4)
        channels = compare_bgra(bytes(a), bytes(b), w, h)
        self.assertEqual(set(channels), {'B', 'G', 'R', 'A'})
        self.assertEqual(channels['B']['first_32_differences'][0],
                         dict(x=1, y=0, gopro=255, ffmpeg=0, delta=255))
        self.assertEqual(channels['G']['max_abs'], 4)
        self.assertEqual(channels['R']['mismatches'], 4)
        self.assertEqual(channels['R']['max_abs'], 8)
        self.assertEqual(channels['A']['mismatches'], 0)
        self.assertEqual(channels['B']['peak'], 255)

    def test_bgra_orientation_detection(self):
        top = bytes([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16])
        bottom_up = bytes([9, 10, 11, 12, 13, 14, 15, 16, 1, 2, 3, 4, 5, 6, 7, 8])
        self.assertEqual(flip_vertical(top, 2, 2), bottom_up)
        aligned, info = align_bgra_orientation(bottom_up, top, 2, 2)
        self.assertEqual(aligned, top)
        self.assertEqual(info['selected'], 'flipped_to_top_down')
        same, info = align_bgra_orientation(top, top, 2, 2)
        self.assertEqual(same, top)
        self.assertEqual(info['selected'], 'top_down')

    def test_first_differences_bounded_and_exact(self):
        a, b = struct.pack('<128H', *([1]*128)), bytes(256)
        planes = compare(a, b, 8, 8)
        self.assertEqual(len(planes['Y']['first_32_differences']), 32)
        self.assertEqual(planes['Y']['first_32_differences'][-1]['y'], 3)
        self.assertFalse(compare(a, a, 8, 8)['Y']['first_32_differences'])

    def test_sequence_avi_header_and_chunks(self):
        packets = [b'one', b'two2', b'three']
        wrapped = avi_sequence(packets, 16, 8)
        self.assertEqual(wrapped.count(b'00dc'), 3)
        self.assertIn(struct.pack('<14I', 40000, 0, 0, 0, 3, 0, 1, 5, 16, 8, 0, 0, 0, 0), wrapped)
        self.assertIn(b'CFHD', wrapped)

    def test_select_filter(self):
        self.assertEqual(select_filter([0, 17, 119]),
                         "select='eq(n\\,0)+eq(n\\,17)+eq(n\\,119)',format=yuv422p10le")

    def test_tiered_manifest_discovery(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'quality'/'1080p-16f').mkdir(parents=True)
            (root/'quality'/'1080p-16f'/'manifest.json').write_text('{}', encoding='utf-8')
            self.assertEqual(manifest_root(root, 'quality', '1080p-16f'),
                             root/'quality'/'1080p-16f')
            (root/'legacy').mkdir()
            (root/'legacy'/'manifest.json').write_text('{}', encoding='utf-8')
            self.assertEqual(manifest_root(root, 'legacy', '1080p-16f'), root/'legacy')
            with self.assertRaises(FileNotFoundError):
                manifest_root(root, 'quality', '7680x4320-1f')


if __name__ == '__main__':
    unittest.main()
