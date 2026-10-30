# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Synthetic negative controls; never modifies or requires the corpus."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('verifier',
    Path(__file__).resolve().parents[1]/'tools/verify_windows_sdk_corpus.py')
v = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(v)


class Integrity(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root/'file.bin').write_bytes(b'abc')
        self.row = chr(9).join(('file.bin', '3', v.sha(b'abc')))+chr(10)
        self.index(self.row)

    def index(self, value):
        (self.root/'TRANSFER_SHA256.tsv').write_text(value)

    def test_valid(self):
        self.assertEqual(len(v.verify_index(self.root, 1)), 1)

    def test_missing_file(self):
        (self.root/'file.bin').unlink()
        with self.assertRaises(ValueError):
            v.verify_index(self.root)

    def test_wrong_length(self):
        (self.root/'file.bin').write_bytes(b'abcd')
        with self.assertRaises(ValueError):
            v.verify_index(self.root)

    def test_same_length_wrong_hash(self):
        (self.root/'file.bin').write_bytes(b'abd')
        with self.assertRaises(ValueError):
            v.verify_index(self.root)

    def test_index_digest_pin(self):
        with self.assertRaises(ValueError):
            v.verify_index(self.root, 1, '0'*64)

    def test_wrong_count(self):
        with self.assertRaises(ValueError):
            v.verify_index(self.root, 2)

    def test_malformed_index(self):
        for value in ('bad row', chr(9).join(('file.bin', '3', 'bad'))):
            self.index(value)
            with self.assertRaises(ValueError):
                v.verify_index(self.root)

    def test_missing_index(self):
        (self.root/'TRANSFER_SHA256.tsv').unlink()
        with self.assertRaises(ValueError):
            v.verify_index(self.root)

    def test_duplicate(self):
        self.index(self.row*2)
        with self.assertRaises(ValueError):
            v.verify_index(self.root)

    def test_unindexed(self):
        (self.root/'extra').write_bytes(b'x')
        with self.assertRaises(ValueError):
            v.verify_index(self.root)

    def test_traversal(self):
        for name in ('../outside', '/outside', 'a//b', 'a/./b', 'a'+chr(92)+'b'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                v.safe_file(self.root, name)

    def test_symlink(self):
        try:
            (self.root/'link').symlink_to('file.bin')
        except (OSError, NotImplementedError):
            self.skipTest('symlink creation unavailable')
        with self.assertRaises(ValueError):
            v.safe_file(self.root, 'link')

    def test_packet_rejects_truncation(self):
        for data in (b'', bytes((0, 1, 0, 9)), bytes((0, 1, 0, 9))*6):
            with self.assertRaises(ValueError):
                v.parse_packet(data)

    def test_chunk_cannot_escape_parent(self):
        data = struct.pack('>hH', 1, 9)+struct.pack('>hH', -0x2200, 100)+bytes(16)
        with self.assertRaises(ValueError):
            v.parse_packet(data)

    def test_packet_order_is_not_manifest_trust(self):
        packet = {'tags': [(0, -69, 2), (4, -80, 1)],
                  'metadata': [('UFRM', ord('L'), '01000000')]}
        self.assertEqual(v.observe_order(packet, 1, 'low')['ufrm'], 1)
        for ordinal, profile in ((0, 'low'), (1, 'high')):
            with self.assertRaises(ValueError):
                v.observe_order(packet, ordinal, profile)

    def test_manifest_hash_is_checked(self):
        frame = {'input': 'file.bin', 'input_byte_length': 3, 'input_sha256': '0'*64}
        with self.assertRaises(ValueError):
            v.checked_frame(self.root, {'file.bin': (3, v.sha(b'abc'))}, frame, 'packet')


if __name__ == '__main__':
    unittest.main()
