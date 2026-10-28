# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.

"""The two recorded RCA-160 ledgers must close without trusting their summary.

These tests recompute the published counts and the per-channel matrix from the
per-call records. They do not decode anything and they do not call the
submission's verifier; the point is an independent arithmetic path.
"""
import importlib.util
from pathlib import Path
import subprocess
import sys
import unittest

sys.dont_write_bytecode = True

TOOL = Path(__file__).resolve().parents[1] / "tools/rca160_ledger_audit.py"
spec = importlib.util.spec_from_file_location("rca160_ledger_audit", TOOL)
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)

# The sanitized 28-call and 96-call records are committed on main by #299.
# A checkout missing either required ledger MUST fail this test, never silently
# skip every assertion and return success to CTest.
class EvidencePresenceTests(unittest.TestCase):
    def test_required_recorded_ledgers_are_present(self):
        for base in (audit.ALL_CHANNELS, audit.LEGACY):
            for name in ("plan.json", "calls.json", "summary.json"):
                with self.subTest(record=str(base.name + "/" + name)):
                    self.assertTrue((base / name).is_file(),
                                    "missing mandatory recorded ledger: " + base.name + "/" + name)

    def test_missing_recorded_ledger_is_a_strict_failure(self):
        import os
        import tempfile
        with tempfile.TemporaryDirectory(prefix="rca160-missing-evidence-") as root:
            environment = dict(os.environ, RCA160_EVIDENCE=root)
            result = subprocess.run([sys.executable, str(TOOL)], env=environment,
                                    text=True, capture_output=True, timeout=30)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("required recorded ledger missing or unreadable", result.stderr)
            self.assertNotIn("RECORDED_LEDGER_AUDIT_PASS", result.stdout)


class InventoryTests(unittest.TestCase):
    def test_the_matrix_has_every_pre_registered_cell(self):
        report = audit.validate()
        self.assertEqual(report["inventory"]["calls"], 28)
        self.assertEqual(report["inventory"]["cells"], 28)

    def test_the_packet_is_the_recorded_capture(self):
        report = audit.validate()
        self.assertEqual(report["inventory"]["packet_sha256"], audit.PACKET_SHA)


class MatrixTests(unittest.TestCase):
    def test_each_inducing_arm_corrupts_only_its_own_channel(self):
        report = audit.validate()
        for key, entries in report["matrix"].items():
            arm = key.split("/")[1]
            if arm not in ("induceY", "induceV", "induceU"):
                continue
            selected = {"induceY": [0], "induceV": [1], "induceU": [2]}[arm]
            for entry in entries:
                self.assertEqual(entry["phase1"], selected, key)

    def test_serial_and_publish_arms_stay_clean(self):
        report = audit.validate()
        for key, entries in report["matrix"].items():
            if key.split("/")[1] not in ("serial", "publishAll"):
                continue
            for entry in entries:
                self.assertEqual(entry["phase1"], [], key)
                self.assertEqual(entry["pixel_equal"], True, key)

    def test_recovery_holds_per_resolution_and_per_channel(self):
        report = audit.validate()
        recovery = report["recovery"]
        self.assertEqual(recovery["full/publishAll"], 2)
        self.assertEqual(recovery["half/publishAll"], 2)
        self.assertEqual(recovery["full/induceAll"], 2)
        self.assertEqual(recovery["half/induceAll"], 2)


class WitnessTests(unittest.TestCase):
    def test_first_differing_coefficient_is_reproduced(self):
        report = audit.validate()
        row = report["first_difference"]["1"]
        self.assertEqual(row["phase_0_expected"], 1063)
        self.assertEqual(row["phase_0_actual"], 0)
        self.assertEqual(row["phase_1_expected"], 1066)
        self.assertEqual(row["phase_1_actual"], 6)

    def test_every_consumer_names_its_own_publisher(self):
        report = audit.validate()
        self.assertGreater(report["producer_identity"]["pairs_checked"], 0)


class LegacyTests(unittest.TestCase):
    def test_96_call_totals_are_recomputed(self):
        report = audit.validate()
        legacy = report["legacy_96"]
        self.assertEqual(legacy["calls"], 96)
        self.assertEqual(legacy["differing_outputs"], 20)
        self.assertEqual(legacy["late_producer"], 19)
        self.assertEqual(legacy["signalled_waits"], 32)
        self.assertEqual(legacy["cells"], 12)


class HonestyTests(unittest.TestCase):
    def test_the_verdict_does_not_claim_a_replay(self):
        report = audit.validate()
        self.assertEqual(report["verdict"], "RECORDED_LEDGER_AUDIT_PASS")
        self.assertIs(report["replay"], False)


class SelfTestTests(unittest.TestCase):
    def test_every_mutation_is_rejected_for_its_stated_reason(self):
        result = subprocess.run([sys.executable, str(TOOL), "--self-test"],
                                capture_output=True, text=True, timeout=900)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("RCA160-LEDGER-SELFTEST: PASS", result.stdout)
        self.assertNotIn("WRONG-REASON", result.stdout)
        self.assertNotIn("SURVIVED", result.stdout)


if __name__ == "__main__":
    unittest.main()
