# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
import struct
import unittest
from validate_filmscan3_geometry import decoded_psnr

class ReferencePixels(unittest.TestCase):
    def test_reference_range_is_binding(self):
        valid=struct.pack('<8H',*([1023]*8))
        invalid=struct.pack('<8H',*([1024]*8))
        with self.assertRaises(ValueError):
            decoded_psnr(valid,invalid)

    def test_both_planes_and_storage(self):
        valid=bytes(16)
        for actual, reference in [(b'',b''),(valid,valid[:-2]),(valid,valid+b'xx')]:
            with self.subTest(length=len(reference)), self.assertRaises(ValueError):
                decoded_psnr(actual,reference)

if __name__ == '__main__':
    unittest.main()
