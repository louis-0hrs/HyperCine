# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
import struct
import unittest
from baseline import summarize_frame


class PlaneContract(unittest.TestCase):
    def test_plane_order_endian_pitch_and_endpoints(self):
        raw = struct.pack('<8H', 1, 1023, 3, 4, 5, 6, 7, 8)
        planes = summarize_frame(raw, 2, 2)['planes']
        self.assertEqual([(p['name'], p['pitch_bytes'], p['first'], p['last'])
                          for p in planes], [('Y', 4, 1, 4), ('U', 2, 5, 6), ('V', 2, 7, 8)])

    def test_truncation_extra_data_geometry_and_precision(self):
        for raw, w, h in [(b'\0' * 15, 2, 2), (b'\0' * 17, 2, 2),
                          (b'\0' * 12, 3, 1), (b'', 0, 1),
                          (struct.pack('<8H', 1024, 0, 0, 0, 0, 0, 0, 0), 2, 2)]:
            with self.subTest(width=w, height=h, length=len(raw)):
                with self.assertRaises(ValueError):
                    summarize_frame(raw, w, h)


if __name__ == '__main__':
    unittest.main()
