#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Synthetic tests: real hostnames, IPs, home paths and credentials must not appear."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import privacy_guard as guard


class RuleTests(unittest.TestCase):
    def codes(self, value):
        return set(guard.rules_for(value))

    def test_rfc1918_only(self):
        for candidate in ("192." + "168.12.8", "10." + "0.20.3",
                          "172." + "31.9.8"):
            self.assertIn("PRIVATE_IPV4", self.codes(candidate))
        for public in ("8.8.8.8", "192.0.2.3", "127.0.0.1",
                       "172.32.0.1"):
            self.assertNotIn("PRIVATE_IPV4", self.codes(public))

    def test_tilde_homedir(self):
        self.assertIn("PERSONAL_ABSOLUTE_PATH",
                      self.codes("~/" + ".openclaw/private/log.txt"))
        self.assertNotIn("PERSONAL_ABSOLUTE_PATH",
                         self.codes("docs/validation/log.txt"))

    def test_local_home_paths(self):
        self.assertIn("PERSONAL_ABSOLUTE_PATH",
                      self.codes("/Users/" + "worker/build/results.json"))
        self.assertIn("PERSONAL_ABSOLUTE_PATH",
                      self.codes("/home/" + "worker/worktree"))
        self.assertIn("PERSONAL_ABSOLUTE_PATH",
                      self.codes("C:" + "\\Users\\" + "worker\\repo"))
        self.assertNotIn("PERSONAL_ABSOLUTE_PATH",
                         self.codes("docs/validation/result.json"))

    def test_network_mounts(self):
        self.assertIn("NETWORK_SHARE", self.codes("\\\\" + "TESTNAS" + "\\golden"))
        self.assertIn("NETWORK_SHARE", self.codes("//" + "TESTNAS/golden"))
        self.assertIn("NETWORK_SHARE", self.codes("smb" + "://TESTNAS/golden"))
        self.assertNotIn("NETWORK_SHARE",
                         self.codes("https://example.com/doc"))

    def test_private_dns(self):
        self.assertIn("PRIVATE_HOSTNAME", self.codes("node" + ".local"))
        self.assertNotIn("PRIVATE_HOSTNAME", self.codes("example.org"))

    def test_credentials_and_placeholders(self):
        self.assertIn("POSSIBLE_CREDENTIAL", self.codes("password" + "=BadSecret123"))
        self.assertNotIn("POSSIBLE_CREDENTIAL", self.codes("password" + "=REDACTED"))
        self.assertIn("TOKEN_PATTERN", self.codes("ghp_" + "A" * 24))

    def test_sensitive_filenames_do_not_echo_name(self):
        suspect = "private/" + "192." + "168.4.8"
        patch = "\n".join(("diff --git a/x b/" + suspect, "--- /dev/null",
                           "+++ b/" + suspect, "@@ -0,0 +1 @@", "+safe"))
        findings = guard.scan_patch(patch)
        self.assertIn(("[SENSITIVE_PATH]", 0, "PATH_PRIVATE_IPV4"), findings)
        self.assertNotIn(suspect, str(findings))

    def test_added_only_and_location(self):
        sample = "\n".join(("diff --git a/a b/a", "--- a/a", "+++ b/a",
                            "@@ -4,1 +4,2 @@", "-old " + "192." + "168.1.2",
                            "+safe text", "+new " + "192." + "168.1.3"))
        self.assertEqual([("a", 5, "PRIVATE_IPV4")], guard.scan_patch(sample))


class GitIntegrationTests(unittest.TestCase):
    def test_commit_message_leak_is_checked(self):
        with tempfile.TemporaryDirectory() as work:
            cwd = Path(work)
            def run(*args):
                return subprocess.run(["git", *args], cwd=cwd, check=True,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                      text=True).stdout.strip()
            run("init", "-q")
            run("config", "user.email", "test@example.invalid")
            run("config", "user.name", "Privacy")
            (cwd / "safe.txt").write_text("safe\n", encoding="utf-8")
            run("add", "safe.txt")
            run("commit", "-qm", "baseline")
            leaked = "10." + "4.6.8"
            run("commit", "--allow-empty", "-qm", "internal " + leaked)
            result = subprocess.run([sys.executable, str(Path(guard.__file__).resolve()),
                                     "--commit", "HEAD"], cwd=cwd, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertEqual(1, result.returncode)
            self.assertIn("COMMIT_MESSAGE", result.stderr)
            self.assertNotIn(leaked, result.stderr)

    @unittest.skipIf(os.name == "nt", "POSIX shell hook integration runs on Linux")
    def test_new_public_branch_rechecks_commit_from_private_remote(self):
        with tempfile.TemporaryDirectory() as work:
            cwd = Path(work)
            public = cwd / "public.git"
            private = cwd / "private.git"
            def git(*args):
                return subprocess.run(["git", *args], cwd=cwd, check=True,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                      text=True).stdout.strip()
            git("init", "-q")
            git("config", "user.email", "privacy-test@example.invalid")
            git("config", "user.name", "Privacy Test")
            git("init", "--bare", "-q", str(public))
            git("init", "--bare", "-q", str(private))
            git("remote", "add", "public", str(public))
            git("remote", "add", "private", str(private))
            artifact = cwd / "report.txt"
            artifact.write_text("baseline\n", encoding="utf-8")
            git("add", "report.txt")
            git("commit", "-qm", "baseline")
            git("push", "-q", "public", "HEAD:refs/heads/main")

            synthetic_address = "192." + "168.8.9"
            artifact.write_text("private test " + synthetic_address + "\n",
                                encoding="utf-8")
            git("add", "report.txt")
            git("commit", "-qm", "unsafe example")
            git("push", "-q", "private", "HEAD:refs/heads/feature")
            sha = git("rev-parse", "HEAD")

            # Former '--not --remotes' would wrongly suppress this commit.
            self.assertEqual("", git("rev-list", sha, "--not", "--remotes"))
            self.assertIn(sha, git("rev-list", sha, "--not", "--remotes=public"))
            hook = Path(guard.__file__).resolve().parent.parent / ".githooks/pre-push"
            ref_line = "refs/heads/feature {} refs/heads/feature {}\n".format(
                sha, "0" * 40)
            result = subprocess.run(["sh", str(hook), "public", str(public)],
                                    input=ref_line, cwd=cwd, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertEqual(1, result.returncode)
            self.assertIn("PRIVATE_IPV4", result.stderr)
            self.assertNotIn(synthetic_address, result.stderr)

    def test_intermediate_exposure_is_not_hidden_by_later_cleanup(self):
        with tempfile.TemporaryDirectory() as work:
            cwd = Path(work)
            def run(*args):
                return subprocess.run(["git", *args], cwd=cwd, check=True,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                      text=True).stdout.strip()
            run("init", "-q")
            run("config", "user.email", "privacy-test@example.invalid")
            run("config", "user.name", "Privacy Test")
            artifact = cwd / "report.txt"
            artifact.write_text("safe\n", encoding="utf-8")
            run("add", "report.txt")
            run("commit", "-qm", "initial")
            base = run("rev-parse", "HEAD")
            private_example = "192." + "168.4.8"
            artifact.write_text("network " + private_example + "\n", encoding="utf-8")
            run("add", "report.txt")
            run("commit", "-qm", "bad intermediate commit")
            artifact.write_text("safe again\n", encoding="utf-8")
            run("add", "report.txt")
            run("commit", "-qm", "later cleanup")
            command = [sys.executable, str(Path(guard.__file__).resolve()),
                       "--commits", base + "..HEAD"]
            result = subprocess.run(command, cwd=cwd, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertEqual(1, result.returncode)
            self.assertIn("PRIVATE_IPV4", result.stderr)
            self.assertNotIn(private_example, result.stderr)


if __name__ == "__main__":
    unittest.main()
