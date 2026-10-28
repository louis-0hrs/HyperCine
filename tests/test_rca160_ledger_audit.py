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

# The ledgers live in a separate evidence branch. When they are absent this
# module must not report a pass it did not perform: the guard skips each class
# with a reason instead of silently passing on an empty audit.
EVIDENCE_PRESENT = ((audit.ALL_CHANNELS / "calls.json").is_file()
                    and (audit.LEGACY / "calls.json").is_file())
NEEDS_EVIDENCE = unittest.skipUnless(
    EVIDENCE_PRESENT,
    "recorded RCA-160 ledgers are not present in this checkout; nothing was audited")


@NEEDS_EVIDENCE
class InventoryTests(unittest.TestCase):
    def test_the_matrix_has_every_pre_registered_cell(self):
        report = audit.validate()
        self.assertEqual(report["inventory"]["calls"], 28)
        self.assertEqual(report["inventory"]["cells"], 28)

    def test_the_packet_is_the_recorded_capture(self):
        report = audit.validate()
        self.assertEqual(report["inventory"]["packet_sha256"], audit.PACKET_SHA)


@NEEDS_EVIDENCE
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


@NEEDS_EVIDENCE
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


@NEEDS_EVIDENCE
class LegacyTests(unittest.TestCase):
    def test_96_call_totals_are_recomputed(self):
        report = audit.validate()
        legacy = report["legacy_96"]
        self.assertEqual(legacy["calls"], 96)
        self.assertEqual(legacy["differing_outputs"], 20)
        self.assertEqual(legacy["late_producer"], 19)
        self.assertEqual(legacy["signalled_waits"], 32)
        self.assertEqual(legacy["cells"], 12)


@NEEDS_EVIDENCE
class HonestyTests(unittest.TestCase):
    def test_the_verdict_does_not_claim_a_replay(self):
        report = audit.validate()
        self.assertEqual(report["verdict"], "RECORDED_LEDGER_AUDIT_PASS")
        self.assertIs(report["replay"], False)


@NEEDS_EVIDENCE
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
