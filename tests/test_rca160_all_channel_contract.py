# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.

"""The #160 all-channel readiness claims must stay true of the frozen tree.

These tests do not decode anything. They pin the source-level facts the
2026-10-28 report rests on, so a rebase that silently repairs one of them shows
up as a red test instead of a stale handoff.
"""
import importlib.util
from pathlib import Path
import subprocess
import sys
import unittest

sys.dont_write_bytecode = True

TOOL = Path(__file__).resolve().parents[1] / "tools/rca160_all_channel_contract.py"
spec = importlib.util.spec_from_file_location("rca160_all_channel_contract", TOOL)
contract = importlib.util.module_from_spec(spec)
spec.loader.exec_module(contract)


class ReleasedPredicateTests(unittest.TestCase):
    def test_released_predicate_is_the_equality_form(self):
        report = contract.validate()
        self.assertEqual(report["macro"]["define_line"], 230)
        self.assertEqual(
            report["macro"]["expression"],
            "((wavelet)->band_valid_flags == (wavelet)->band_started_flags)")

    def test_strict_mask_form_survives_only_as_a_comment(self):
        report = contract.validate()
        self.assertEqual(report["macro"]["strict_form_commented_at"], 229)

    def test_flag_space_is_exhausted_not_sampled(self):
        report = contract.validate()
        space = report["flag_space"]
        self.assertEqual(space["pairs_examined"], 256)
        self.assertEqual(space["released_accepted"], 16)
        self.assertEqual(space["released_accepted_without_lowpass"], 8)
        self.assertIn("0xE/0xE", space["released_without_lowpass_pairs"])

    def test_strict_predicate_accepts_no_band0_absent_pair(self):
        report = contract.validate()
        self.assertEqual(report["flag_space"]["strict_accepted_without_lowpass"], 0)


class GuardClassificationTests(unittest.TestCase):
    def test_all_transform_bands_valid_is_hollow(self):
        report = contract.validate()
        guard = report["guards"]["AllTransformBandsValid"]
        self.assertEqual(guard["status"], "hollow")
        self.assertEqual(guard["context"], "if DEBUG || 1")

    def test_lowpass_check_is_assert_only_and_never_compiled(self):
        report = contract.validate()
        guard = report["guards"]["AllLowpassBandsValid"]
        self.assertEqual(guard["status"], "assert-only")
        self.assertEqual(guard["context"], "if DEBUG")
        self.assertEqual(report["guards"]["_DEBUG_defined_in_build_files"], [])


class PublicationOrderTests(unittest.TestCase):
    def test_valid_flags_are_published_under_the_lock(self):
        report = contract.validate()
        writers = report["writers"]
        self.assertEqual(len(writers["valid_flags_locks"]), 1)

    def test_started_flags_are_published_without_a_lock(self):
        report = contract.validate()
        writers = report["writers"]
        self.assertEqual(writers["started_flags_locks"], [])
        self.assertEqual(writers["delayed_thread_start"], "1")

    def test_admission_decides_under_the_lock_and_rechecks_after_it(self):
        report = contract.validate()
        admission = report["admission"]
        self.assertLess(admission["decision_line"], admission["unlock_line"])
        self.assertLess(admission["unlock_line"], admission["recheck_after_unlock_line"])

    def test_publication_follows_the_inverse_transform_in_both_branches(self):
        report = contract.validate()
        publication = report["publication"]
        calls = publication["inverse_transform_lines"]
        publishes = publication["publish_lines"]
        self.assertEqual(len(calls), 2)
        for index, call in enumerate(calls):
            following = [n for n in publishes if n > call]
            self.assertTrue(following, "no publication follows the transform at %d" % call)
            if index + 1 < len(calls):
                later_guard = min((n for n in publication["readiness_guards"] if n > call),
                                  default=None)
                if later_guard is not None:
                    self.assertLess(following[0], later_guard)


class ScopeTests(unittest.TestCase):
    def test_temporal_and_highpass_branches_remain_ungated(self):
        report = contract.validate()
        ungated = report["ungated"]
        self.assertFalse(ungated["temporal_runtime_band0_check"])
        self.assertEqual(len(ungated["frame_publish_lines"]), 2)

    def test_frozen_tree_is_untouched(self):
        report = contract.validate()
        self.assertEqual(report["upstream_tree"], "clean")


class SelfTestTests(unittest.TestCase):
    def test_every_mutation_is_rejected_for_its_stated_reason(self):
        """The checker must disagree with the prose when the tree moves.

        Run as a subprocess because the self-test spawns one per mutation; its
        own output names each case, so a wrong-reason rejection is visible
        rather than hidden behind an aggregate exit code.
        """
        result = subprocess.run([sys.executable, str(TOOL), "--self-test"],
                                capture_output=True, text=True, timeout=600)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("RCA160-SELFTEST: PASS", result.stdout)
        self.assertNotIn("WRONG-REASON", result.stdout)
        self.assertNotIn("SURVIVED", result.stdout)


if __name__ == "__main__":
    unittest.main()
