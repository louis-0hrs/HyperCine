#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.

"""Regression coverage for the Windows SDK-oracle log replay filter."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "compare_sdk_oracle", Path(__file__).resolve().parent / "compare_sdk_oracle.py")
compare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare)


class ObservationFilter(unittest.TestCase):
    def test_retired_pass_marker_is_not_required(self):
        self.assertFalse(compare.is_observation_line(
            "SDK decode PASS: all three standalone packets"))

    def test_summary_line_is_recorded(self):
        self.assertTrue(compare.is_observation_line(
            "standalone_sdk_decode_3_packets quality_measured=1 "
            "quality_meets_threshold=1 distinctOutputsMax=1 "
            "BLOCKED_ORACLE_NOT_QUALIFIED exit=77"))

    def test_per_pattern_lines_are_recorded(self):
        self.assertTrue(compare.is_observation_line(
            "flat distinctYU64=1 distinctBGRA=1"))
        self.assertTrue(compare.is_observation_line(
            "invalid zero header PrepareToDecode status=1"))

    def test_strict_failure_lines_are_recorded(self):
        self.assertTrue(compare.is_observation_line(
            "gradient STRICT_FAILURE Cannot open fixture"))
        self.assertTrue(compare.is_observation_line(
            "Oracle harness error: boom"))

    def test_unrelated_lines_are_ignored(self):
        self.assertFalse(compare.is_observation_line("100% tests passed"))


if __name__ == '__main__':
    unittest.main()
