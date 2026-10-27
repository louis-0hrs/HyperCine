# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.

"""The recorded RCA-159 witness must survive recomputation and stay sanitized."""
from copy import deepcopy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import sys

sys.dont_write_bytecode = True

spec = importlib.util.spec_from_file_location(
    "rca159_witness_contract", Path(__file__).resolve().parents[1] / "tools/rca159_witness_contract.py")
contract = importlib.util.module_from_spec(spec)
spec.loader.exec_module(contract)


def evidence():
    return contract.load(contract.EVIDENCE)


def protocol():
    return contract.load(contract.PROTOCOL)


class RepositoryRecordTests(unittest.TestCase):
    def test_committed_records_validate(self):
        result = contract.validate()
        self.assertEqual(result["controls"], 4)
        self.assertAlmostEqual(result["witness_p_two_sided"], 2.1e-06)

    def test_published_p_value_is_recomputed_not_trusted(self):
        document = evidence()
        document["witness"]["contingency"]["counts"] = [[3, 1], [0, 56]]
        with self.assertRaises(contract.ContractError):
            contract.validate_evidence(document)

    def test_arm_totals_must_close(self):
        document = evidence()
        document["rate_arms"]["arms"][0]["modal"] += 1
        with self.assertRaises(contract.ContractError):
            contract.validate_evidence(document)

    def test_only_the_uninstrumented_arm_may_be_quoted_as_a_rate(self):
        document = evidence()
        document["rate_arms"]["arms"][2]["quotable_as_rate"] = True
        with self.assertRaises(contract.ContractError):
            contract.validate_evidence(document)

    def test_withdrawn_claim_must_still_be_contradicted(self):
        document = evidence()
        for frame in document["withdrawn_hypotheses"][0]["contradicting_observations"]["frames"]:
            frame["early_accept_events"] = 1
            frame["planes_moved"] = ["B", "G", "R"]
            frame["channel_changed_percent"] = {"B": 100.0, "G": 100.0, "R": 100.0}
        with self.assertRaises(contract.ContractError):
            contract.validate_evidence(document)

    def test_hash_domains_must_close(self):
        document = evidence()
        document["hash_domains"]["entries"][1]["bytes"] -= 1
        with self.assertRaises(contract.ContractError):
            contract.validate_evidence(document)

    def test_private_output_may_not_claim_a_rehash(self):
        document = evidence()
        document["hash_domains"]["recomputed_here"] = True
        with self.assertRaises(contract.ContractError):
            contract.validate_evidence(document)

    def test_rate_contrast_must_match_the_arms_it_summarises(self):
        document = evidence()
        document["rate_arms"]["arms"][2]["divergent"] = 9
        document["rate_arms"]["arms"][2]["modal"] = 51
        with self.assertRaises(contract.ContractError):
            contract.validate_evidence(document)

    def test_rate_contrast_rows_must_name_their_arms_in_order(self):
        document = evidence()
        document["rate_arms"]["traced_vs_uninstrumented"]["rows"] = [
            "uninstrumented", "buffered_recorder"]
        with self.assertRaises(contract.ContractError):
            contract.validate_evidence(document)

    def test_witness_population_must_equal_the_contingency_total(self):
        for population in (1, 99):
            document = evidence()
            document["witness"]["population"]["fresh_processes"] = population
            with self.assertRaises(contract.ContractError):
                contract.validate_evidence(document)

    def test_sentinel_byte_value_must_fit_in_one_byte(self):
        document = evidence()
        document["hash_domains"]["entries"][1]["sentinel_byte_value"] = 999
        with self.assertRaises(contract.ContractError):
            contract.validate_evidence(document)

    def test_unsanitized_host_path_is_refused(self):
        document = evidence()
        # Built from parts so the fixture itself carries no literal share path.
        document["provenance"]["instrumentation"] = "C:" + chr(92) + "work" + chr(92) + "golden"
        with self.assertRaises(contract.ContractError):
            contract.validate_evidence(document)

    def test_unsanitized_host_path_is_refused_as_a_json_key(self):
        document = evidence()
        document["provenance"]["C:" + chr(92) + "work" + chr(92) + "golden"] = "recorded"
        with self.assertRaises(contract.ContractError):
            contract.validate_evidence(document)

    def test_non_claims_must_be_explicitly_false(self):
        document = evidence()
        document["non_claims"]["pixel_oracle_qualified"] = "no"
        with self.assertRaises(contract.ContractError):
            contract.validate_evidence(document)


class PreregistrationTests(unittest.TestCase):
    def test_preregistration_must_predate_execution(self):
        document = protocol()
        document["executed"] = True
        with self.assertRaises(contract.ContractError):
            contract.validate_protocol(document, evidence())

    def test_all_four_controls_are_required(self):
        document = protocol()
        document["controls"] = document["controls"][:3]
        with self.assertRaises(contract.ContractError):
            contract.validate_protocol(document, evidence())

    def test_tracked_upstream_change_may_not_be_permitted(self):
        document = protocol()
        document["patch_requirement"]["tracked_upstream_change_permitted"] = True
        with self.assertRaises(contract.ContractError):
            contract.validate_protocol(document, evidence())

    def test_both_admission_predicates_are_required(self):
        document = protocol()
        document["patch_requirement"]["checked_at"] = ["worker_dequeue"]
        with self.assertRaises(contract.ContractError):
            contract.validate_protocol(document, evidence())

    def test_guarded_arm_must_actually_carry_the_guard(self):
        document = protocol()
        document["arms"][1]["guard_enabled"] = False
        with self.assertRaises(contract.ContractError):
            contract.validate_protocol(document, evidence())

    def test_runner_fields_must_express_each_arm(self):
        for field in ("run.arms.A.divergent", "run.arms.B.output_sha256",
                      "run.arms.B.early_reader_observed"):
            document = protocol()
            document["runner_required_fields"].remove(field)
            with self.assertRaises(contract.ContractError):
                contract.validate_protocol(document, evidence())

    def test_runner_fields_must_express_each_control(self):
        for field in ("run.controls.C1.outcome", "run.controls.C2.executed",
                      "run.controls.C4.deadline_violated"):
            document = protocol()
            document["runner_required_fields"].remove(field)
            with self.assertRaises(contract.ContractError):
                contract.validate_protocol(document, evidence())

    def test_a_single_aggregate_control_execution_flag_is_refused(self):
        document = protocol()
        document["runner_required_fields"].append("run.controls.executed")
        with self.assertRaises(contract.ContractError):
            contract.validate_protocol(document, evidence())

    def test_control_outcomes_must_keep_skipped_visible(self):
        document = protocol()
        document["control_outcomes"].remove("SKIPPED")
        with self.assertRaises(contract.ContractError):
            contract.validate_protocol(document, evidence())

    def test_cli_fails_closed_on_a_tampered_copy(self):
        document = evidence()
        document["witness"]["contingency"]["counts"] = [[4, 0], [1, 55]]
        with tempfile.TemporaryDirectory() as directory:
            tampered = Path(directory) / "evidence.json"
            tampered.write_text(json.dumps(document), encoding="utf-8")
            self.assertEqual(contract.main(["--evidence", str(tampered)]), 1)
        self.assertEqual(contract.main([]), 0)


if __name__ == "__main__":
    unittest.main()
