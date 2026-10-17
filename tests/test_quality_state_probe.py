#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Exercise the actual oracle probe process, including typed integer boundaries."""
import json
from pathlib import Path
import subprocess
import sys
import unittest

PROBE = str(Path(sys.argv.pop(1)).resolve())


class ProbeArguments(unittest.TestCase):
    def invoke(self, replacements=None):
        args = ['6', '1920', '1080', '0', '0', '0', '0']
        for index, value in (replacements or {}).items():
            args[index] = value
        return subprocess.run([PROBE] + args, capture_output=True, text=True, timeout=10)

    def test_valid_profiles(self):
        for profile in range(1, 7):
            with self.subTest(profile=profile):
                result = self.invoke({0: str(profile)})
                self.assertEqual(result.returncode, 0, result.stderr)
                state = json.loads(result.stdout)
                self.assertEqual(len(state['quant']), 36)
                self.assertEqual(len(state['scale']), 36)

    def test_rejects_partial_empty_and_out_of_range_numbers(self):
        for index in range(7):
            for value in ['', 'junk', '1junk', '6.5', ' 6', '+6', '6 ', '9' * 80]:
                with self.subTest(index=index, value=value):
                    result = self.invoke({index: value})
                    self.assertEqual(result.returncode, 2)
                    self.assertEqual(result.stdout, '')
                    self.assertEqual(result.stderr, 'invalid integer argument\n')

    def test_typed_ranges(self):
        for index, invalid in [(0, ['2147483648', '-2147483649']),
                               (1, ['2147483648', '-2147483649']),
                               (2, ['2147483648', '-2147483649']),
                               (3, ['-1', '4294967296']),
                               (4, ['2147483648', '-2147483649']),
                               (5, ['-1', '18446744073709551616']),
                               (6, ['2147483648', '-2147483649'])]:
            for value in invalid:
                with self.subTest(index=index, value=value):
                    result = self.invoke({index: value})
                    self.assertEqual(result.returncode, 2)
                    self.assertEqual(result.stdout, '')

    def test_valid_frame_boundary_and_signed_limiter(self):
        result = self.invoke({3: '4294967295', 4: '-2147483648'})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['limiter'], 0)

    def test_semantically_invalid_numbers_remain_core_errors(self):
        for changes in [{0: '7'}, {1: '-2'}, {2: '0'}, {5: '18446744073709551615'}]:
            with self.subTest(changes=changes):
                result = self.invoke(changes)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stdout, '')


if __name__ == '__main__':
    unittest.main()
