#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Tests for ci/check_runner_capability.py — one accepted and one rejected shape
per capability label, so a mislabelled pool host cannot pass silently.
"""

from __future__ import annotations

import os
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import check_runner_capability as crc  # noqa: E402


def all_tools(name: str) -> str:
    return f"/usr/bin/{name}"


class Accepted(unittest.TestCase):
    def test_linux_cpu(self) -> None:
        self.assertEqual([], crc.check("hc-linux-x64-cpu", "Linux", "x86_64", all_tools))

    def test_linux_cuda(self) -> None:
        self.assertEqual(
            [], crc.check("hc-linux-x64-cuda12.8", "Linux", "x86_64", all_tools)
        )

    def test_windows_msvc(self) -> None:
        self.assertEqual([], crc.check("hc-windows-x64-msvc", "Windows", "AMD64", all_tools))

    def test_macos_arm64(self) -> None:
        self.assertEqual([], crc.check("hc-macos-arm64", "Darwin", "arm64", all_tools))


class Rejected(unittest.TestCase):
    def test_linux_label_on_macos_host(self) -> None:
        problems = crc.check("hc-linux-x64-cpu", "Darwin", "arm64", all_tools)
        self.assertEqual(2, len(problems))

    def test_macos_label_on_intel_host(self) -> None:
        problems = crc.check("hc-macos-arm64", "Darwin", "x86_64", all_tools)
        self.assertEqual(1, len(problems))
        self.assertIn("machine mismatch", problems[0])

    def test_windows_label_on_linux_host(self) -> None:
        problems = crc.check("hc-windows-x64-msvc", "Linux", "x86_64", all_tools)
        self.assertEqual(1, len(problems))
        self.assertIn("system mismatch", problems[0])

    def test_missing_tool_is_a_failure(self) -> None:
        problems = crc.check("hc-linux-x64-cpu", "Linux", "x86_64", lambda name: None)
        self.assertEqual(4, len(problems))
        self.assertTrue(all("missing required tool" in p for p in problems))

    def test_unknown_capability(self) -> None:
        problems = crc.check("self-hosted", "Linux", "x86_64", all_tools)
        self.assertEqual(1, len(problems))
        self.assertIn("unknown capability label", problems[0])


class Cli(unittest.TestCase):
    def test_cli_exit_codes(self) -> None:
        with patch.object(crc.platform, "system", return_value="Linux"), \
             patch.object(crc.platform, "machine", return_value="x86_64"), \
             patch.object(crc.shutil, "which", side_effect=all_tools):
            self.assertEqual(0, crc.main(["--capability", "hc-linux-x64-cpu"]))
        self.assertEqual(2, crc.main(["--capability", "hc-nope"]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
