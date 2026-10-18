#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Reject sequence corruption even when frame count and byte length agree."""
import hashlib
from pathlib import Path
import tempfile
import unittest

from validate_filmscan3_geometry import verify_sequence


class SequenceTests(unittest.TestCase):
    frames = (b'first frame!', b'next frame!!', b'last frame!!')

    def check_sequence(self, data):
        expected = [hashlib.sha256(frame).hexdigest() for frame in self.frames]
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'sequence.yuv'
            path.write_bytes(data)
            return verify_sequence(path, expected, len(self.frames[0]))

    def test_exact_sequence(self):
        self.assertEqual(self.check_sequence(b''.join(self.frames)),
                         [hashlib.sha256(frame).hexdigest() for frame in self.frames])

    def test_reordered_frames(self):
        with self.assertRaisesRegex(ValueError, 'frame 0'):
            self.check_sequence(b''.join(reversed(self.frames)))

    def test_duplicate_frame(self):
        with self.assertRaisesRegex(ValueError, 'frame 1'):
            self.check_sequence(self.frames[0] * 2 + self.frames[2])

    def test_corrupted_pixel(self):
        data = bytearray(b''.join(self.frames))
        data[-1] ^= 1
        with self.assertRaisesRegex(ValueError, 'frame 2'):
            self.check_sequence(data)

    def test_truncated_frame(self):
        with self.assertRaisesRegex(ValueError, 'truncated.*frame 2'):
            self.check_sequence(b''.join(self.frames)[:-1])

    def test_missing_frame(self):
        with self.assertRaisesRegex(ValueError, 'truncated.*frame 2'):
            self.check_sequence(b''.join(self.frames[:-1]))

    def test_extra_data(self):
        for suffix in (b'\0', self.frames[0]):
            with self.subTest(extra_bytes=len(suffix)):
                with self.assertRaisesRegex(ValueError, 'trailing'):
                    self.check_sequence(b''.join(self.frames) + suffix)


    def test_empty_or_invalid_sequence_contract_rejected(self):
        empty_sha = hashlib.sha256(b'').hexdigest()
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'sequence.yuv'
            path.write_bytes(b'')
            for hashes, size in (([], 12), ([empty_sha], 0),
                    ([empty_sha], False), ([empty_sha], -1), ([empty_sha], 1.0),
                    (['invalid'], 12), (None, 12)):
                with self.subTest(hashes=hashes, size=size), self.assertRaises(ValueError):
                    verify_sequence(path, hashes, size)

if __name__ == '__main__':
    unittest.main()
