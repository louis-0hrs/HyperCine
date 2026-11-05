#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Negative controls for ci/workflow_event_matrix.py.

A routing gate that only ever prints OK proves nothing. Each test below takes a
deep copy of the real workflows, applies exactly one mutation that would make
the routing unsafe, and requires the verifier to report the specific rule that
covers it. The baseline copy must stay clean, so a rule that fires on
everything is caught too.

Run standalone (no CI needed):  python3 ci/test_workflow_event_matrix.py
Under unittest:                 python3 -m unittest ci.test_workflow_event_matrix
"""

from __future__ import annotations

import copy
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import workflow_event_matrix as wem  # noqa: E402

BASE = wem.load_workflows()


def docs() -> dict:
    return copy.deepcopy(BASE)


_ORIGINAL_SCENARIOS = wem.scenarios


def with_pool_enabled() -> list[wem.Event]:
    """Scenarios with Mac-only private relay activation enabled."""
    events = _ORIGINAL_SCENARIOS()
    for event in events:
        event.vars = {"HYPERCINE_CI_LOCAL_MAC": "1"}
    return events


def rules(findings: list[wem.Finding]) -> set[str]:
    return {finding.rule for finding in findings}


def standalone(d: dict) -> dict:
    return d["ci-standalone.yml"]["jobs"]["standalone"]


def privacy(d: dict) -> dict:
    return d["privacy-guard.yml"]["jobs"]


def cuda(d: dict) -> dict:
    return d["ci-cuda-build.yml"]["jobs"]["cuda-build"]


class Baseline(unittest.TestCase):
    def test_real_workflows_are_clean(self) -> None:
        findings = wem.check_contract(BASE) + wem.check_local_evidence(BASE)
        self.assertEqual([], [f"{f.rule}: {f.detail}" for f in findings])

    def test_every_workflow_is_parsed(self) -> None:
        self.assertIn("privacy-guard.yml", BASE)
        self.assertEqual(
            {
                "ci-cpu.yml",
                "ci-cuda-build.yml",
                "ci-standalone.yml",
                "ci-workflow-routing.yml",
                "privacy-guard.yml",
                "python-compat.yml",
                "quality-state.yml",
            },
            set(BASE),
        )


class NonMacDefaultRoutingPreserved(unittest.TestCase):
    """Non-Mac defaults retain all original labels; Mac needs explicit approval."""

    def test_default_labels_match_the_hosted_baseline(self) -> None:
        expected = {
            "ci-cpu.yml:cpu-only-windows-msvc": ["windows-latest"],
            "ci-cuda-build.yml:cuda-build": ["ubuntu-24.04"],
            "ci-cuda-build.yml:inventory-gate-selftest": ["ubuntu-24.04"],
            "ci-standalone.yml:standalone [Windows MSVC x64 Release]": ["windows-2022"],
            "ci-standalone.yml:standalone [Linux x64 Release]": ["ubuntu-24.04"],
            "ci-standalone.yml:standalone [Linux x64 ASan UBSan]": ["ubuntu-24.04"],
            "privacy-guard.yml:windows-installed-hooks": ["windows-2022"],
            "privacy-guard.yml:publication-privacy": ["ubuntu-24.04"],
            "python-compat.yml:rca160-python": ["ubuntu-24.04"],
            "quality-state.yml:quality-state [Windows MSVC x64]": ["windows-2022"],
            "quality-state.yml:quality-state [Linux x64]": ["ubuntu-24.04"],
        }
        event = wem.scenarios()[0]
        actual = {
            f"{item.workflow}:{item.job_id}"
            + (f" [{item.matrix_name}]" if item.matrix_name else ""): item.labels
            for item in wem.plan(BASE, event)
        }
        self.assertEqual(expected, actual)

    def test_routing_contract_job_starts_only_on_workflow_changes(self) -> None:
        """The gate is path-filtered, and stays hosted when it does start."""
        changed = wem.Event(
            name="pull_request",
            ref="refs/pull/43/merge",
            ref_name="infra/x",
            ref_type="branch",
            changed=[".github/workflows/ci-standalone.yml"],
            pr_number=43,
        )
        items = wem._by(wem.plan(BASE, changed), "ci-workflow-routing.yml", "routing-contract")
        self.assertEqual(1, len(items))
        self.assertEqual(["ubuntu-24.04"], items[0].labels)

        unrelated = wem.Event(
            name="pull_request",
            ref="refs/pull/44/merge",
            ref_name="feature/y",
            ref_type="branch",
            changed=["src/core/pipeline.cpp"],
            pr_number=44,
        )
        self.assertEqual([], wem._by(wem.plan(BASE, unrelated), "ci-workflow-routing.yml", "routing-contract"))

    def test_no_job_ever_requests_the_pool_by_default(self) -> None:
        for event in wem.scenarios():
            for item in wem.plan(BASE, event):
                self.assertFalse(wem._is_self_hosted(item.labels), item.describe())


class RequiredPrivacyScope(unittest.TestCase):
    def test_r1_push_scope_removed_from_privacy(self) -> None:
        d = docs()
        del d["privacy-guard.yml"]["on"]["push"]
        self.assertEqual({"R1"}, rules(wem.check_contract(d)))

    def test_r1_pr_scope_removed_from_privacy(self) -> None:
        d = docs()
        del d["privacy-guard.yml"]["on"]["pull_request"]
        self.assertEqual({"R1"}, rules(wem.check_contract(d)))

    def test_r2_privacy_moved_to_self_hosted(self) -> None:
        d = docs()
        privacy(d)["publication-privacy"]["runs-on"] = ["self-hosted", "hc-linux-x64-cpu"]
        self.assertIn("R2", rules(wem.check_contract(d)))

    def test_r2_privacy_label_typo_is_not_a_hosted_runner(self) -> None:
        d = docs()
        privacy(d)["publication-privacy"]["runs-on"] = "ubuntu-24.04-large"
        found = rules(wem.check_contract(d))
        self.assertIn("R2", found)


class SelfHostedRouting(unittest.TestCase):
    def test_r12_bare_self_hosted_label(self) -> None:
        d = docs()
        standalone(d)["runs-on"] = "self-hosted"
        self.assertIn("R12", rules(wem.check_local_evidence(d)))

    def test_r4_capability_label_without_explicit_self_hosted(self) -> None:
        d = docs()
        standalone(d)["runs-on"] = "hc-linux-x64-cpu"
        self.assertIn("R4", rules(wem.check_contract(d)))

    def test_r4_unknown_capability_label(self) -> None:
        d = docs()
        standalone(d)["runs-on"] = ["self-hosted", "hc-linux-x64-gpu"]
        wem.scenarios = with_pool_enabled
        self.addCleanup(setattr, wem, "scenarios", _ORIGINAL_SCENARIOS)
        self.assertIn("R4", rules(wem.check_contract(d)))

    def test_r5_linux_label_on_windows_lane(self) -> None:
        d = docs()
        d["ci-cpu.yml"]["jobs"]["cpu-only-windows-msvc"]["runs-on"] = ["self-hosted", "hc-linux-x64-cpu"]
        wem.scenarios = with_pool_enabled
        self.addCleanup(setattr, wem, "scenarios", _ORIGINAL_SCENARIOS)
        self.assertIn("R5", rules(wem.check_contract(d)))

    def test_r5_macos_asan_lane_moved_to_linux(self) -> None:
        d = docs()
        standalone(d)["runs-on"] = ["self-hosted", "hc-linux-x64-cpu"]
        wem.scenarios = with_pool_enabled
        self.addCleanup(setattr, wem, "scenarios", _ORIGINAL_SCENARIOS)
        self.assertIn("R5", rules(wem.check_contract(d)))

    def test_r6_fork_pr_reaches_self_hosted(self) -> None:
        """Drop the fork guard from the real label expression."""
        d = docs()
        for name, job in d["ci-standalone.yml"]["jobs"].items():
            job["runs-on"] = "".join(
                (
                    "${{ fromJSON(vars.HYPERCINE_CI_LOCAL_MAC == '1'",
                    " && format('[\"self-hosted\",\"{0}\"]', matrix.capability)",
                    " || format('\"{0}\"', matrix.runner)) }}",
                )
            )
        wem.scenarios = with_pool_enabled
        self.addCleanup(setattr, wem, "scenarios", _ORIGINAL_SCENARIOS)
        self.assertIn("R6", rules(wem.check_contract(d)))


    def test_r13_same_repo_pr_exposure(self) -> None:
        d = docs()
        standalone(d)["runs-on"] = ["self-hosted", "hc-macos-arm64"]
        self.assertIn("R13", rules(wem.check_contract(d)))

    def test_r13_manual_feature_ref_exposure(self) -> None:
        d = docs()
        standalone(d)["runs-on"] = "${{ github.event_name == 'workflow_dispatch' && fromJSON('[\"self-hosted\",\"hc-macos-arm64\"]') || matrix.runner }}"
        self.assertIn("R13", rules(wem.check_contract(d)))

    def test_r13_feature_push_exposure(self) -> None:
        d = docs()
        standalone(d)["runs-on"] = "${{ github.ref == 'refs/heads/feature/x' && fromJSON('[\"self-hosted\",\"hc-macos-arm64\"]') || matrix.runner }}"
        self.assertIn("R13", rules(wem.check_contract(d)))

    def test_r13_even_main_cannot_register_a_public_runner(self) -> None:
        d = docs()
        standalone(d)["runs-on"] = "${{ github.ref == 'refs/heads/main' && fromJSON('[\"self-hosted\",\"hc-macos-arm64\"]') || matrix.runner }}"
        self.assertIn("R13", rules(wem.check_contract(d)))

    def test_r14_protected_main_positive_control(self) -> None:
        d = docs()
        matrix = standalone(d)["strategy"]["matrix"]
        matrix["include"] = matrix["include"].replace("github.actor_id == '248477380'", "true")
        self.assertIn("R14", rules(wem.check_contract(d)))

    def test_r14_flag_must_not_omit_pr_mac_tests(self) -> None:
        d = docs()
        matrix = standalone(d)["strategy"]["matrix"]
        matrix["include"] = matrix["include"].replace("github.event_name == 'workflow_dispatch'", "true").replace("inputs.mac_hosted_fallback == true", "true").replace("inputs.mac_candidate_sha == github.sha", "true")
        self.assertIn("R14", rules(wem.check_contract(d)))


class CoverageNotNarrowed(unittest.TestCase):
    def test_r7_docs_only_pr_starts_standalone(self) -> None:
        d = docs()
        # The old behaviour: '**' push/pull_request with no path filter would
        # still schedule the matrix for a docs-only PR.
        for trigger in ("push", "pull_request"):
            d["ci-standalone.yml"]["on"][trigger].pop("paths")
        self.assertIn("R7", rules(wem.check_contract(d)))

    def test_r8_standalone_row_removed(self) -> None:
        d = docs()
        standalone(d)["strategy"]["matrix"]["include"] = [{"name": "Linux x64 Release", "runner": "ubuntu-24.04"}]
        self.assertIn("R8", rules(wem.check_contract(d)))

    def test_r8_standalone_row_renamed(self) -> None:
        d = docs()
        standalone(d)["strategy"]["matrix"]["include"] = standalone(d)["strategy"]["matrix"]["include"].replace("Linux x64 Release", "Linux x64 Debug")
        self.assertIn("R8", rules(wem.check_contract(d)))

    def test_r9_tag_push_schedules_matrix(self) -> None:
        d = docs()
        d["ci-standalone.yml"]["on"]["push"]["tags"] = ["*"]
        self.assertIn("R9", rules(wem.check_contract(d)))

    def test_r10_concurrency_group_per_run_not_per_pr(self) -> None:
        d = docs()
        d["ci-standalone.yml"]["concurrency"] = {
            "group": "${{ github.workflow }}-${{ github.run_id }}",
            "cancel-in-progress": True,
        }
        self.assertIn("R10", rules(wem.check_contract(d)))

    def test_r11_self_hosted_cuda_without_toolkit_check(self) -> None:
        d = docs()
        cuda(d)["runs-on"] = ["self-hosted", "hc-linux-x64-cuda12.8"]
        cuda(d)["steps"] = [
            step
            for step in cuda(d)["steps"]
            if step.get("name") != "Verify pinned CUDA toolkit (self-hosted)"
        ]
        self.assertIn("R11", rules(wem.check_contract(d)))


class SelfHostedEnabled(unittest.TestCase):
    """The same rules must hold with the routing variable switched on.

    Legacy flags cannot affect hosted non-Mac jobs or enable automatic Mac.
    """

    def setUp(self) -> None:
        self._orig = wem.scenarios

        def enabled() -> list[wem.Event]:
            events = self._orig()
            for event in events:
                event.vars = {"HYPERCINE_CI_LOCAL_MAC": "1"}
            return events

        wem.scenarios = enabled
        self.addCleanup(setattr, wem, "scenarios", self._orig)

    def test_contract_clean_with_pool_enabled(self) -> None:
        findings = wem.check_contract(BASE) + wem.check_local_evidence(BASE)
        self.assertEqual([], [f"{f.rule}: {f.detail}" for f in findings])

    def test_only_main_mac_rows_are_delegated(self) -> None:
        event = next(e for e in wem.scenarios() if e.label == "main push")
        items = wem.plan(BASE, event)
        self.assertFalse(any(wem._is_self_hosted(i.labels) for i in items))
        self.assertFalse(any(i.labels == ["macos-15"] for i in items))
        rows = wem._by(items, "ci-standalone.yml", "standalone")
        self.assertEqual(3, len(rows))
        self.assertEqual({"windows-2022", "ubuntu-24.04"}, {i.labels[0] for i in rows})

    def test_prs_feature_pushes_and_manual_refs_stay_hosted(self) -> None:
        for event in wem.scenarios():
            if event.name == "push" and event.ref == "refs/heads/main":
                continue
            for item in wem.plan(BASE, event):
                self.assertFalse(wem._is_self_hosted(item.labels), item.describe())

    def test_privacy_stays_hosted_with_pool_enabled(self) -> None:
        for event in wem.scenarios():
            for item in wem._by(wem.plan(BASE, event), wem.PRIVACY_WORKFLOW, wem.PRIVACY_JOB):
                self.assertFalse(wem._is_self_hosted(item.labels), event.label)


class FailClosed(unittest.TestCase):
    def test_unsupported_syntax_raises(self) -> None:
        with self.assertRaises(wem.UnsupportedExpression):
            wem.eval_expr("${{ startsWith(github.ref, 'refs/heads/') }}", {"github": {}})

    def test_interpolated_group_name_resolves(self) -> None:
        ctx = wem.build_ctx("w.yml", wem.scenarios()[0])
        value = "${{ github.workflow }}-${{ github.event.pull_request.number }}"
        self.assertEqual("w.yml-42", wem.eval_expr(value, ctx))

    def test_truthiness_matches_actions(self) -> None:
        zero = {"github": {"event": {"pull_request": {"number": 0}}}}
        self.assertFalse(wem.eval_condition("${{ github.event.pull_request.number }}", zero))
        ctx = wem.build_ctx("w.yml", wem.scenarios()[3])  # main push
        self.assertTrue(wem.eval_condition("${{ github.ref == 'refs/heads/main' }}", ctx))


if __name__ == "__main__":
    unittest.main(verbosity=2)
