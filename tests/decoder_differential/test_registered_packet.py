# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Synthetic identity-gate tests; no decoder result is represented here."""
import unittest

import registered_packet as registered


class IdentityGateTest(unittest.TestCase):
    def test_wrong_packet_length_fails(self):
        with self.assertRaises(ValueError):
            registered.verify_inputs(b'x' * (registered.PACKET_BYTES - 1),
                                     b'\0' * registered.SOURCE_BYTES)

    def test_wrong_packet_digest_fails_at_registered_length(self):
        with self.assertRaises(ValueError):
            registered.verify_inputs(b'\0' * registered.PACKET_BYTES,
                                     b'\0' * registered.SOURCE_BYTES)

    def test_wrong_source_length_fails(self):
        with self.assertRaises(ValueError):
            registered.verify_inputs(b'\0' * registered.PACKET_BYTES,
                                     b'\0' * (registered.SOURCE_BYTES - 1))

    def test_packet_digest_constant_is_lowercase_sha256(self):
        self.assertEqual(len(registered.PACKET_SHA256), 64)
        self.assertEqual(registered.PACKET_SHA256, registered.PACKET_SHA256.lower())
        self.assertEqual(len(registered.SOURCE_SHA256), 64)


if __name__ == '__main__':
    unittest.main()
