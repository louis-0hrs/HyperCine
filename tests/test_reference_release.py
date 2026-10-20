# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.

"""Regression coverage for green CTest with blocked/missing reference tests."""
import importlib.util
from pathlib import Path
import unittest
import sys

sys.dont_write_bytecode = True

spec = importlib.util.spec_from_file_location(
    "reference_release", Path(__file__).resolve().parents[1] / "tools/check_reference_release.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


def report(overrides=None):
    overrides = overrides or {}
    return "<testsuite>" + "".join(
        '<testcase name="' + name + '" status="run">' + overrides.get(name, "") +
        "</testcase>" for name in gate.REQUIRED) + "</testsuite>"


class ReleaseGateTests(unittest.TestCase):
    def test_all_pass_execution_without_independent_oracle_is_blocked(self):
        data = gate.inventory(report(), 0)
        self.assertEqual(data["reference_lane"], "PASS")
        self.assertTrue(all(row["state"] == "PASS" for row in data["tests"]))
        self.assertEqual(data["gate_exit"], 77)
        self.assertEqual(data["release_gate"], "BLOCKED")
        self.assertFalse(data["independent_pixel_oracle"]["qualified"])

    def test_test_named_qualified_cannot_grant_release(self):
        xml = report().replace("</testsuite>",
            '<testcase name="independent_oracle_qualified" status="run"/></testsuite>')
        self.assertEqual(gate.inventory(xml, 0)["gate_exit"], 77)

    def test_green_ctest_with_skips_is_blocked(self):
        data = gate.inventory(report({"test_phase2a": '<skipped message="SKIP_RETURN_CODE=77"/>'}), 0)
        self.assertEqual(data["gate_exit"], 77)
        self.assertEqual(data["release_gate"], "BLOCKED")

    def test_cpu_only_does_not_qualify_cuda(self):
        data = gate.inventory('<testsuite><testcase name="test_reference_oracle" status="run"/></testsuite>', 0)
        self.assertEqual(data["gate_exit"], 77)
        self.assertEqual(sum(row["state"] == "BLOCKED" for row in data["tests"]), 4)

    def test_resource_failure_overrides_blocked_pixels(self):
        data = gate.inventory(report({"test_phase2a": "<skipped/>",
                                      "test_phase2c": "<failure>capacity</failure>"}), 8)
        self.assertEqual(data["gate_exit"], 1)

    def test_runner_error_cannot_pass(self):
        self.assertEqual(gate.inventory(report(), 8)["gate_exit"], 1)

    def test_not_run_is_blocked(self):
        self.assertEqual(gate.inventory(report().replace('status="run"', 'status="notrun"'), 0)["gate_exit"], 77)

    def test_duplicate_evidence_rejected(self):
        with self.assertRaises(ValueError):
            gate.inventory('<testsuite><testcase name="x"/><testcase name="x"/></testsuite>', 0)


if __name__ == "__main__":
    unittest.main()
