#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Portable self-tests for the derivative affinity protocol helpers."""
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import affinity_1080


class ArmMaskTests(unittest.TestCase):
    def test_lowest_active_bits_are_selected(self):
        self.assertEqual(affinity_1080.arm_masks(0xff),
                         {'A': None, 'B': 0x0f, 'C': 0x03, 'D': 0x01})

    def test_non_contiguous_system_mask_is_respected(self):
        self.assertEqual(affinity_1080.arm_masks(0x55),
                         {'A': None, 'B': 0x55, 'C': 0x05, 'D': 0x01})

    def test_missing_logical_processors_fail_closed(self):
        with self.assertRaises(ValueError):
            affinity_1080.arm_masks(0x03)


class FirstDifferenceTests(unittest.TestCase):
    def test_no_difference_is_none(self):
        self.assertIsNone(affinity_1080.first_difference(b'ABCD', b'ABCD'))

    def test_channel_and_coordinate_mapping_matches_1920_geometry(self):
        reference = bytes(3 * 7680)
        actual = bytearray(reference)
        offset = 2 * 7680 + 5 * 4 + 2
        actual[offset] = 7
        difference = affinity_1080.first_difference(reference, bytes(actual))
        self.assertEqual(difference, dict(byte_offset=offset, x=5, y=2,
                                          channel='R', reference=0, actual=7))


if __name__ == '__main__':
    unittest.main()
