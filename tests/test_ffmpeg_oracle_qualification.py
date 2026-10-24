#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Contract tests for the external-oracle qualification harness.

No FFmpeg and no SDK are needed: these pin the fail-closed decisions that the
harness makes before any external decoder runs.
"""
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import qualify_ffmpeg_oracle as harness

FIXTURES = Path(__file__).parent / 'fixtures' / 'standalone'


class QualificationContractTest(unittest.TestCase):
    def test_measured_builds_are_accepted(self):
        for token in ('n8.0-140fd653aed8', '9.0.2-essentials_build-www.gyan.dev'):
            self.assertEqual(harness.require_qualified_build(token), token)

    def test_unmeasured_builds_are_blocked(self):
        for token in ('9.1.0', 'n7.1', '7.0.2'):
            with self.assertRaises(harness.Blocked):
                harness.require_qualified_build(token)

    def test_adapter_rejects_out_of_contract_output(self):
        harness.validate_output(bytes(harness.OUTPUT_BYTES))
        with self.assertRaises(harness.Failed):
            harness.validate_output(bytes(harness.OUTPUT_BYTES - 1))
        samples = [0] * (harness.OUTPUT_BYTES // 2)
        samples[0] = harness.PEAK + 1
        with self.assertRaises(harness.Failed):
            harness.validate_output(struct.pack('<{}H'.format(len(samples)), *samples))

    def test_self_check_passes(self):
        harness.self_check_rejects_corrupt_output()

    def test_missing_tool_is_blocked(self):
        with self.assertRaises(harness.Blocked):
            harness.find_tool(str(FIXTURES / 'does-not-exist'), 'ffmpeg')
        with self.assertRaises(harness.Blocked):
            harness.find_tool(None, 'hypercine-no-such-tool')

    def test_bound_fixture_is_accepted(self):
        declared = harness.load_manifest(FIXTURES)
        packet = harness.load_bound(FIXTURES, declared, 'flat.cfhd')
        self.assertEqual(harness.sha256(packet), declared['flat.cfhd']['sha256'])

    def test_tampered_fixture_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix='hypercine-oracle-contract-') as temporary:
            copy = Path(temporary) / 'fixtures'
            shutil.copytree(FIXTURES, copy)
            declared = harness.load_manifest(copy)
            path = copy / 'canonical.p10'
            data = bytearray(path.read_bytes())
            data[0] ^= 1
            path.write_bytes(data)
            with self.assertRaises(harness.Failed):
                harness.load_bound(copy, declared, 'canonical.p10')
            (copy / 'flat.cfhd').unlink()
            with self.assertRaises(harness.Failed):
                harness.load_bound(copy, declared, 'flat.cfhd')


if __name__ == '__main__':
    unittest.main()
