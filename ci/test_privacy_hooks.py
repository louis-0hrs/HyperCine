#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Exercise installed publication hooks against disposable local Git remotes."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipIf(os.name == "nt", "Native POSIX hook qualification")
class InstalledHookTests(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory()
        self.addCleanup(work.cleanup)
        self.root = Path(work.name)
        self.repo = self.root / "work"
        self.repo.mkdir()
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith("GIT_")}
        self.env.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")
        self.git("init", "-q")
        self.identity("Alex Example", "alex@example.org")
        self.git("config", "user.useConfigOnly", "true")
        source = Path(__file__).resolve().parent.parent
        (self.repo / "ci").mkdir()
        shutil.copyfile(source / "ci/privacy_guard.py",
                        self.repo / "ci/privacy_guard.py")
        shutil.copytree(source / ".githooks", self.repo / ".githooks")
        self.git("add", ".")
        self.git("commit", "-qm", "safe baseline")
        self.git("config", "core.hooksPath", ".githooks")
        self.git("init", "--bare", "-q", str(self.root / "public.git"))
        self.git("remote", "add", "public", str(self.root / "public.git"))
        self.git("push", "-q", "public", "HEAD:refs/heads/main")
        self.base = self.git("rev-parse", "HEAD").stdout.strip()

    def git(self, *args, expected=0):
        result = subprocess.run(["git", *args], cwd=self.repo, env=self.env,
                                text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
        self.assertEqual(expected, result.returncode,
                         "Unexpected exit status for git " + args[0])
        return result

    def identity(self, name, email):
        self.git("config", "user.name", name)
        self.git("config", "user.email", email)

    def commit(self):
        self.git("commit", "--allow-empty", "-qm", "safe update")

    def rejected(self, result, marker, value):
        self.assertIn(marker, result.stderr)
        self.assertNotIn(value, result.stdout + result.stderr)

    def unpublished(self):
        result = self.git("ls-remote", "public", "refs/heads/review")
        self.assertEqual("", result.stdout)

    def test_human_public_identity_can_commit_and_push(self):
        self.identity("Alex Example", "alex@company.example.org")
        self.commit()
        self.git("push", "-q", "public", "HEAD:refs/heads/review")

    def test_neutral_agent_can_commit_and_push(self):
        self.identity("HyperCine Codex Agent", "agent@example.invalid")
        self.commit()
        self.git("push", "-q", "public", "HEAD:refs/heads/review")

    def test_github_noreply_agent_can_commit_and_push(self):
        self.identity("Codex Agent", "12345+agent@users.noreply.github.com")
        self.commit()
        self.git("push", "-q", "public", "HEAD:refs/heads/review")

    def test_agent_requires_config_only(self):
        self.identity("Codex Agent", "agent@example.invalid")
        self.git("config", "user.useConfigOnly", "false")
        result = self.git("commit", "--allow-empty", "-qm", "safe update", expected=1)
        self.rejected(result, "AGENT_USE_CONFIG_ONLY_REQUIRED", "agent@example.invalid")

    def test_agent_corporate_identity_is_explicitly_rejected(self):
        email = "agent@company.example.org"
        self.identity("Codex Agent", email)
        result = self.git("commit", "--allow-empty", "-qm", "safe update", expected=1)
        self.rejected(result, "UNAPPROVED_AGENT_IDENTITY", email)

    def test_author_override_blocks_real_commit(self):
        value = "agent@node" + ".internal"
        self.env["GIT_AUTHOR_EMAIL"] = value
        result = self.git("commit", "--allow-empty", "-qm", "safe update", expected=1)
        self.rejected(result, "AUTHOR", value)
        self.assertEqual(self.base, self.git("rev-parse", "HEAD").stdout.strip())

    def test_committer_override_blocks_real_commit(self):
        value = "agent@node" + ".local"
        self.env["GIT_COMMITTER_EMAIL"] = value
        result = self.git("commit", "--allow-empty", "-qm", "safe update", expected=1)
        self.rejected(result, "COMMITTER", value)

    def import_commit(self, text="safe\n", email=None):
        # Model imported history before hook installation, never bypass a hook.
        incoming = self.root / "incoming"
        self.git("clone", "-q", str(self.repo), str(incoming))
        def run(*args):
            return subprocess.run(["git", *args], cwd=incoming, env=self.env,
                                  check=True, text=True, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE)
        run("config", "user.name", "Imported Example")
        run("config", "user.email", email or "imported@example.org")
        (incoming / "report.txt").write_text(text, encoding="utf-8")
        run("add", "report.txt")
        run("commit", "-qm", "imported update")
        self.git("fetch", "-q", str(incoming), "HEAD")
        self.git("merge", "--ff-only", "-q", "FETCH_HEAD")

    def test_first_push_rejects_imported_identity(self):
        value = "agent@node" + ".local"
        self.import_commit(email=value)
        result = self.git("push", "-q", "public", "HEAD:refs/heads/review", expected=1)
        self.rejected(result, "AUTHOR", value)
        self.unpublished()

    def test_private_remote_cannot_hide_first_public_push(self):
        value = "192." + "168.8.9"
        self.import_commit(text="network " + value + "\n")
        self.git("init", "--bare", "-q", str(self.root / "private.git"))
        self.git("remote", "add", "private", str(self.root / "private.git"))
        # Seed the other remote from the hook-free imported repository.
        subprocess.run(["git", "push", "-q", str(self.root / "private.git"),
                        "HEAD:refs/heads/feature"], cwd=self.root / "incoming",
                       env=self.env, check=True, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
        self.git("fetch", "-q", "private")
        result = self.git("push", "-q", "public", "HEAD:refs/heads/review", expected=1)
        self.rejected(result, "PRIVATE_IPV4", value)
        self.unpublished()

    def test_cleanup_cannot_hide_intermediate_disclosure(self):
        value = "/" + "c/" + "Users/" + "synthetic-agent/report"
        self.import_commit(text="ROOT=" + value + "\n")
        (self.repo / "report.txt").write_text("safe again\n", encoding="utf-8")
        self.git("add", "report.txt")
        self.git("commit", "-qm", "sanitized update")
        result = self.git("push", "-q", "public", "HEAD:refs/heads/review", expected=1)
        self.rejected(result, "PERSONAL_ABSOLUTE_PATH", value)
        self.unpublished()

    def test_existing_branch_update_rejects_imported_disclosure(self):
        value = "10." + "4.6.8"
        self.import_commit(text="network " + value + "\n")
        result = self.git("push", "-q", "public", "HEAD:refs/heads/main", expected=1)
        self.rejected(result, "PRIVATE_IPV4", value)
        remote_sha = self.git("ls-remote", "public", "refs/heads/main").stdout.split()[0]
        self.assertEqual(self.base, remote_sha)

    def test_raw_url_push_fails_closed(self):
        self.commit()
        result = self.git("push", "-q", str(self.root / "public.git"),
                          "HEAD:refs/heads/review", expected=1)
        self.assertIn("configured remote name", result.stderr)
        self.unpublished()


if __name__ == "__main__":
    unittest.main()
