# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.

"""Regression coverage for green CTest with blocked/missing reference tests."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import sys
from unittest import mock

sys.dont_write_bytecode = True

spec = importlib.util.spec_from_file_location(
    "reference_release", Path(__file__).resolve().parents[1] / "tools/check_reference_release.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


def raw_registration(missing=(), bad_label=(), bad_skip=(), extras=()):
    tests = []
    for name in gate.REQUIRED:
        if name in missing:
            continue
        labels = ["unrelated"] if name in bad_label else [gate.LANE_LABEL]
        properties = [dict(name="LABELS", value=labels)]
        if name in gate.PHASE2:
            properties.append(dict(name="SKIP_RETURN_CODE",
                                   value=0 if name in bad_skip else 77))
        tests.append(dict(name=name, properties=properties))
    for name in extras:
        tests.append(dict(name=name,
                          properties=[dict(name="LABELS", value=[gate.LANE_LABEL])]))
    return json.dumps(dict(tests=tests))


def registration(**kwargs):
    return gate.load_registration(raw_registration(**kwargs))


def report(results=None, extras=(), omit=()):
    results = results or {}
    body = "".join(
        '<testcase name="' + name + '" status="' + results.get(name, ("run", ""))[0] + '">' +
        results.get(name, ("run", ""))[1] + "</testcase>"
        for name in gate.REQUIRED if name not in omit)
    body += "".join('<testcase name="' + name + '" status="run"/>' for name in extras)
    return "<testsuite>" + body + "</testsuite>"


def classify(data, name):
    return next(row for row in data["tests"] if row["test"] == name)


FAILED = "<failure>Failed</failure>"


def failure(output):
    return FAILED + "<system-out>" + output + "</system-out>"


CAPACITY_FAILED = (
    "FAIL: 8K DeviceInput Consolidated strict encoder/bitstream gate failed: "
    "UNSUPPORTED_RESOURCE_BOUND: GPU entropy stream 2 entropy workspace allocation "
    "failed: requested 33178624 B; free 23789568 B of 3953131520 B; "
    "CUDA cudaErrorMemoryAllocation (out of memory)")
CAPACITY_NO_QUERY = (
    "UNSUPPORTED_RESOURCE_BOUND: GPU entropy stream 1 entropy workspace allocation "
    "failed: requested 1048576 B; device memory query failed; "
    "CUDA cudaErrorMemoryAllocation (out of memory)")


class ReleaseGateTests(unittest.TestCase):
    def test_all_pass_execution_without_independent_oracle_is_blocked(self):
        data = gate.inventory(report(), 0, registration())
        self.assertEqual(data["reference_lane"], "PASS")
        self.assertTrue(all(row["state"] == "PASS" for row in data["tests"]))
        self.assertEqual(data["gate_exit"], 77)
        self.assertEqual(data["release_gate"], "BLOCKED")
        self.assertFalse(data["independent_pixel_oracle"]["qualified"])
        self.assertEqual(data["counts"]["PASS"], len(gate.REQUIRED))

    def test_test_named_qualified_cannot_grant_release(self):
        xml = report(extras=("independent_oracle_qualified",))
        reg = registration(extras=("independent_oracle_qualified",))
        self.assertEqual(gate.inventory(xml, 0, reg)["gate_exit"], 77)

    def test_oracle_skip_is_classified_blocked(self):
        data = gate.inventory(report({"test_phase2a": ("notrun",
            '<skipped message="SKIP_RETURN_CODE=77"/>')}), 0, registration())
        row = classify(data, "test_phase2a")
        self.assertEqual(row["classification"], "REFERENCE_ORACLE_BLOCKED")
        self.assertEqual(row["gate_exit"], 77)
        self.assertEqual(data["release_gate"], "BLOCKED")

    def test_arbitrary_skip_is_not_oracle_evidence(self):
        data = gate.inventory(report({"test_phase2a": ("notrun",
            '<skipped message="DISABLED"/>')}), 0, registration())
        self.assertEqual(classify(data, "test_phase2a")["classification"],
                         "UNEXPECTED_SKIP")
        self.assertEqual(data["gate_exit"], 77)

    def test_missing_registration_cannot_be_coverage(self):
        data = gate.inventory(report(omit=("test_phase2c",)), 0,
                              registration(missing=("test_phase2c",)))
        self.assertEqual(classify(data, "test_phase2c")["classification"],
                         "REGISTRATION_MISSING")
        self.assertFalse(data["coverage"]["complete"])
        self.assertEqual(data["gate_exit"], 77)

    def test_changed_skip_property_is_registration_mismatch(self):
        data = gate.inventory(report(), 0, registration(bad_skip=("test_phase2b",)))
        self.assertEqual(classify(data, "test_phase2b")["classification"],
                         "REGISTRATION_MISMATCH")
        self.assertFalse(data["coverage"]["complete"])
        self.assertEqual(data["gate_exit"], 77)

    def test_missing_lane_label_is_registration_mismatch(self):
        data = gate.inventory(report(), 0, registration(bad_label=("test_phase2d",)))
        self.assertEqual(classify(data, "test_phase2d")["classification"],
                         "REGISTRATION_MISMATCH")

    def test_registered_without_result_is_not_run(self):
        data = gate.inventory(report(omit=("test_phase2b",)), 0, registration())
        self.assertEqual(classify(data, "test_phase2b")["classification"], "NOT_RUN")
        self.assertFalse(data["coverage"]["complete"])
        self.assertEqual(data["gate_exit"], 77)

    def test_unregistered_result_fails_closed(self):
        xml = report(extras=("stale_test_from_other_build",))
        data = gate.inventory(xml, 0, registration())
        self.assertEqual(data["gate_exit"], 1)
        self.assertEqual(data["release_gate"], "FAIL")

    def test_every_registered_lane_test_must_execute(self):
        reg = registration(extras=("test_reference_release_inventory",))
        complete = gate.inventory(report(extras=("test_reference_release_inventory",)),
                                  0, reg)
        self.assertTrue(complete["coverage"]["complete"])
        self.assertEqual(complete["gate_exit"], 77)
        absent = gate.inventory(report(), 0, reg)
        self.assertFalse(absent["coverage"]["complete"])

    def test_cpu_only_does_not_qualify_cuda(self):
        data = gate.inventory('<testsuite><testcase name="test_reference_oracle" status="run"/></testsuite>',
                              0, registration(missing=gate.PHASE2))
        self.assertEqual(data["gate_exit"], 77)
        self.assertEqual(sum(row["state"] == "BLOCKED" for row in data["tests"]),
                         len(gate.PHASE2))

    def test_resource_failure_overrides_blocked_pixels(self):
        data = gate.inventory(report({"test_phase2a": ("notrun",
                                      '<skipped message="SKIP_RETURN_CODE=77"/>'),
                                      "test_phase2c": ("run", "<failure>capacity</failure>")}),
                              8, registration())
        self.assertEqual(data["gate_exit"], 1)

    def test_capacity_bound_failure_is_classified_but_still_fails(self):
        data = gate.inventory(report({"test_phase2c": ("run", failure(CAPACITY_FAILED))}),
                              8, registration())
        row = classify(data, "test_phase2c")
        self.assertEqual(row["classification"], "STRICT_FAILURE_CAPACITY_BOUND")
        self.assertEqual(row["state"], "FAIL")
        self.assertEqual(row["gate_exit"], 1)
        self.assertEqual(row["capacity"]["requested_bytes"], 33178624)
        self.assertEqual(row["capacity"]["free_bytes"], 23789568)
        self.assertEqual(row["capacity"]["total_bytes"], 3953131520)
        self.assertEqual(row["capacity"]["cuda_status"], "cudaErrorMemoryAllocation")
        self.assertEqual(data["counts"]["FAIL"], 1)
        self.assertEqual(data["release_gate"], "FAIL")
        self.assertEqual(data["gate_exit"], 1)

    def test_capacity_message_without_device_query_is_classified(self):
        data = gate.inventory(report({"test_phase2c": ("run", failure(CAPACITY_NO_QUERY))}),
                              8, registration())
        row = classify(data, "test_phase2c")
        self.assertEqual(row["classification"], "STRICT_FAILURE_CAPACITY_BOUND")
        self.assertIsNone(row["capacity"]["free_bytes"])
        self.assertIsNone(row["capacity"]["total_bytes"])
        self.assertEqual(data["gate_exit"], 1)

    def test_capacity_marker_in_passing_output_grants_nothing(self):
        xml = report()
        xml = xml.replace('<testcase name="test_phase2c" status="run"></testcase>',
                          '<testcase name="test_phase2c" status="run">'
                          '<system-out>' + CAPACITY_FAILED + '</system-out></testcase>')
        data = gate.inventory(xml, 0, registration())
        row = classify(data, "test_phase2c")
        self.assertEqual(row["classification"], "PASS_EXECUTION_ONLY")
        self.assertEqual(data["gate_exit"], 77)
        self.assertEqual(data["release_gate"], "BLOCKED")

    def test_partial_capacity_message_stays_strict_failure(self):
        for text in ("UNSUPPORTED_RESOURCE_BOUND: workspace allocation failed",
                     "UNSUPPORTED_RESOURCE_BOUND: workspace allocation failed: "
                     "requested 33178624 B; CUDA cudaErrorMemoryAllocation (out of memory)",
                     "capacity: requested 33178624 B; free 23789568 B of 3953131520 B"):
            data = gate.inventory(report({"test_phase2c": ("run", failure(text))}), 8,
                                  registration())
            row = classify(data, "test_phase2c")
            self.assertEqual(row["classification"], "STRICT_FAILURE", text)
            self.assertNotIn("capacity", row)
            self.assertEqual(data["gate_exit"], 1)

    def test_runner_error_cannot_pass(self):
        self.assertEqual(gate.inventory(report(), 8, registration())["gate_exit"], 1)

    def test_duplicate_evidence_rejected(self):
        with self.assertRaises(ValueError):
            gate.inventory('<testsuite><testcase name="x"/><testcase name="x"/></testsuite>',
                           0, registration())

    def test_duplicate_registration_rejected(self):
        with self.assertRaises(ValueError):
            gate.load_registration(json.dumps(dict(tests=[
                dict(name="x"), dict(name="x")])))


def completed(returncode, stdout="", stderr=""):
    return subprocess.CompletedProcess([], returncode, stdout=stdout, stderr=stderr)


class RunnerTests(unittest.TestCase):
    """The error path must stay a complete schema-3 document and name its CTest."""

    def run_gate(self, argv, run):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "reference-release.json"
            with mock.patch.object(gate, "_run", run):
                code = gate.main(argv + ["--output", str(output)])
            return code, json.loads(output.read_text(encoding="utf-8"))

    def test_error_path_keeps_full_schema_and_diagnostic(self):
        def run(command):
            return completed(3, "ctest wrote the failure to stdout\n",
                             "ctest stderr detail\n")

        code, data = self.run_gate(["--build", "missing-build"], run)
        self.assertEqual(code, 1)
        for key in ("schema_version", "scope", "ctest_exit", "gate_exit",
                    "reference_lane", "independent_pixel_oracle", "release_gate",
                    "counts", "coverage", "reason", "tests"):
            self.assertIn(key, data)
        self.assertEqual(data["schema_version"], 3)
        self.assertEqual(data["reference_lane"], "FAIL")
        self.assertEqual(data["counts"], dict(PASS=0, FAIL=0, BLOCKED=0))
        self.assertFalse(data["coverage"]["complete"])
        self.assertEqual(data["coverage"]["required"], len(gate.REQUIRED))
        self.assertIsNone(data["ctest_exit"])
        self.assertIn("exit 3", data["reason"])
        self.assertIn("ctest wrote the failure to stdout", data["reason"])

    def test_missing_junit_report_carries_ctest_diagnostics(self):
        def run(command):
            if "--show-only=json-v1" in command:
                return completed(0, raw_registration(), "")
            return completed(2, "ctest-stdout-diagnostic\n", "ctest-stderr-diagnostic\n")

        code, data = self.run_gate(["--build", "present-build"], run)
        self.assertEqual(code, 1)
        self.assertEqual(data["ctest_exit"], 2)
        self.assertIn("no JUnit report", data["reason"])
        self.assertIn("exit 2", data["reason"])
        self.assertIn("ctest-stdout-diagnostic", data["reason"])

    def test_configured_ctest_is_used(self):
        seen = []

        def run(command):
            seen.append(list(command))
            return completed(3, "", "")

        self.run_gate(["--build", "missing-build", "--ctest", "/opt/other/ctest"], run)
        self.assertEqual([command[0] for command in seen], ["/opt/other/ctest"])

    def test_ctest_defaults_to_configured_environment(self):
        seen = []

        def run(command):
            seen.append(list(command))
            return completed(3, "", "")

        with mock.patch.dict(os.environ, {"CTEST_COMMAND": "/env/ctest"}):
            self.run_gate(["--build", "missing-build"], run)
        self.assertEqual([command[0] for command in seen], ["/env/ctest"])


if __name__ == "__main__":
    unittest.main()
