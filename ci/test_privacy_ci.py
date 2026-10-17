#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Run the actual Ubuntu CI range-selection shell against synthetic Git history."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest


@unittest.skipIf(os.name == "nt", "Ubuntu CI shell qualification on POSIX")
class WorkflowIdentityTests(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory()
        self.addCleanup(work.cleanup)
        self.repo = Path(work.name)
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith("GIT_")}
        self.env.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1",
                        GIT_AUTHOR_DATE="2026-01-01T00:00:00+0000",
                        GIT_COMMITTER_DATE="2026-01-01T00:00:00+0000")
        self.git("init", "-q", "--template=")
        self.git("config", "user.name", "Alex Example")
        self.git("config", "user.email", "alex@example.org")
        source = Path(__file__).resolve().parent.parent
        (self.repo / "ci").mkdir()
        shutil.copyfile(source / "ci/privacy_guard.py",
                        self.repo / "ci/privacy_guard.py")
        self.git("add", "ci/privacy_guard.py")
        self.git("commit", "-qm", "synthetic public baseline")
        self.base = self.git("rev-parse", "HEAD")
        self.git("update-ref", "refs/remotes/origin/main", self.base)
        # Execute the checked-in workflow, not a second copy of its logic.
        workflow = (source / ".github/workflows/privacy-guard.yml").read_text(
            encoding="utf-8")
        step = workflow.split(
            "      - name: Inspect each introduced commit (not just final diff)\n", 1)[1]
        block = step.split("        run: |\n", 1)[1]
        lines = []
        for line in block.splitlines():
            if line.strip() and not line.startswith("          "):
                break
            lines.append(line)
        self.script = textwrap.dedent("\n".join(lines))
        self.assertIn('python ci/privacy_guard.py --commits "$base..HEAD"', self.script)
        # setup-python supplies `python` on Actions; use this test interpreter.
        bin_dir = self.repo / "bin"
        bin_dir.mkdir()
        (bin_dir / "python").symlink_to(sys.executable)
        self.env["PATH"] = str(bin_dir) + os.pathsep + self.env["PATH"]

    def git(self, *args):
        result = subprocess.run(["git", *args], cwd=self.repo, env=self.env,
                                text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
        self.assertEqual(0, result.returncode, "Git fixture operation failed: " + args[0])
        return result.stdout.strip()

    def commit(self, author=None, committer=None):
        for role, identity in (("AUTHOR", author), ("COMMITTER", committer)):
            if identity:
                self.env["GIT_" + role + "_NAME"] = identity[0]
                self.env["GIT_" + role + "_EMAIL"] = identity[1]
        try:
            # Deliberately model imported history in a hook-free disposable repo.
            self.git("commit", "--allow-empty", "-qm", "synthetic update")
        finally:
            for role in ("AUTHOR", "COMMITTER"):
                for field in ("NAME", "EMAIL"):
                    self.env.pop("GIT_" + role + "_" + field, None)

    def run_gate(self, event, before=None, pr_base=None):
        env = dict(self.env, GITHUB_EVENT_NAME=event,
                   PR_BASE_SHA=self.base if pr_base is None else pr_base,
                   PUSH_BEFORE_SHA="0" * 40 if before is None else before)
        return subprocess.run(["bash", "-e", "-o", "pipefail", "-c", self.script],
                              cwd=self.repo, env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def assert_rejected(self, result, roles, email):
        self.assertEqual(1, result.returncode)
        for role in roles:
            self.assertIn("[" + role + "]:0: UNAPPROVED_AGENT_IDENTITY", result.stderr)
        self.assertNotIn(email, result.stdout + result.stderr)
        self.assertNotIn(str(self.repo), result.stdout + result.stderr)

    def test_pr_rejects_both_unapproved_envelope_identities(self):
        identity = ("Codex Agent", "agent@company.example.org")
        self.commit(identity, identity)
        self.assert_rejected(self.run_gate("pull_request"),
                             ("AUTHOR", "COMMITTER"), identity[1])

    def test_first_push_rejects_both_unapproved_envelope_identities(self):
        identity = ("Codex Agent", "agent@company.example.org")
        self.commit(identity, identity)
        self.assert_rejected(self.run_gate("push"), ("AUTHOR", "COMMITTER"), identity[1])

    def test_each_role_is_independently_checked(self):
        identity = ("Codex Agent", "agent@company.example.org")
        for role in ("AUTHOR", "COMMITTER"):
            with self.subTest(role=role):
                self.git("reset", "--hard", "-q", self.base)
                self.commit(**{role.lower(): identity})
                for event in ("pull_request", "push"):
                    with self.subTest(event=event):
                        self.assert_rejected(self.run_gate(event), (role,), identity[1])

    def test_allowed_agents_and_explicit_humans_pass_both_events(self):
        for identity in (("Codex Agent", "12345+agent@users.noreply.github.com"),
                         ("Codex Agent", "agent@example.invalid"),
                         ("Alex Example", "alex@company.example.org")):
            with self.subTest(identity=identity):
                self.git("reset", "--hard", "-q", self.base)
                self.commit(identity, identity)
                for event in ("pull_request", "push"):
                    with self.subTest(event=event):
                        result = self.run_gate(event)
                        self.assertEqual(0, result.returncode, result.stderr)
                        self.assertIn("Privacy guard passed", result.stdout)

    def test_later_safe_commit_cannot_hide_intermediate_bad_identity(self):
        identity = ("Codex Agent", "agent@company.example.org")
        self.commit(identity, identity)
        self.commit()
        for event in ("pull_request", "push"):
            with self.subTest(event=event):
                self.assert_rejected(self.run_gate(event),
                                     ("AUTHOR", "COMMITTER"), identity[1])

    def test_existing_branch_push_checks_before_range(self):
        self.commit()
        before = self.git("rev-parse", "HEAD")
        identity = ("Codex Agent", "agent@company.example.org")
        self.commit(committer=identity)
        self.assert_rejected(self.run_gate("push", before=before),
                             ("COMMITTER",), identity[1])

    def test_missing_push_before_falls_back_to_main(self):
        identity = ("Codex Agent", "agent@company.example.org")
        self.commit(author=identity)
        for before in ("", "f" * 40):
            with self.subTest(before=before):
                self.assert_rejected(self.run_gate("push", before=before),
                                     ("AUTHOR",), identity[1])

    def test_missing_main_merge_base_fails_closed_on_first_push(self):
        self.git("update-ref", "-d", "refs/remotes/origin/main")
        result = self.run_gate("push")
        self.assertEqual(2, result.returncode)
        self.assertIn("No verifiable main merge base; fail closed.", result.stdout)

    def test_missing_pr_base_fails_closed(self):
        for base in ("", "f" * 40):
            with self.subTest(base=base):
                result = self.run_gate("pull_request", pr_base=base)
                self.assertEqual(2, result.returncode)
                self.assertIn("Missing reliable base SHA; fail closed.", result.stdout)

    def test_empty_range_passes(self):
        for event in ("pull_request", "push"):
            with self.subTest(event=event):
                result = self.run_gate(event)
                self.assertEqual(0, result.returncode, result.stderr)
                self.assertIn("Privacy guard passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
