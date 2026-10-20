# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.

"""Regression coverage for green CTest with blocked/missing reference tests."""
import importlib.util
import json
from pathlib import Path
import unittest
import sys

sys.dont_write_bytecode = True

spec = importlib.util.spec_from_file_location(
    "reference_release", Path(__file__).resolve().parents[1] / "tools/check_reference_release.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


def registration(missing=(), bad_label=(), bad_skip=(), extras=()):
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
    return gate.load_registration(json.dumps(dict(tests=tests)))


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


if __name__ == "__main__":
    unittest.main()
