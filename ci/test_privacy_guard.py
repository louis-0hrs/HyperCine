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


class HookInterpreterTests(unittest.TestCase):
    @unittest.skipIf(os.name == "nt", "POSIX shell test; Git for Windows uses Git Bash")
    def test_windows_store_alias_falls_back_to_working_python(self):
        # A found python3 command that cannot run Python must not be selected.
        with tempfile.TemporaryDirectory() as work:
            cwd = Path(work)
            fake = cwd / "fakebin"
            fake.mkdir()
            alias = fake / "python3"
            alias.write_text("#!/bin/sh\nexit 37\n", encoding="utf-8")
            alias.chmod(0o755)
            fallback = fake / "python"
            import shlex
            fallback.write_text('#!/bin/sh\nexec {} "$@"\n'.format(
                shlex.quote(sys.executable)), encoding="utf-8")
            fallback.chmod(0o755)
            def git(*args):
                return subprocess.run(["git", *args], cwd=cwd, check=True,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                      text=True).stdout.strip()
            git("init", "-q")
            git("config", "user.email", "test@example.invalid")
            git("config", "user.name", "Privacy Hook")
            git("remote", "add", "origin", str(cwd / "public.git"))
            (cwd / "ci").mkdir()
            (cwd / "ci/privacy_guard.py").write_bytes(
                Path(guard.__file__).resolve().read_bytes())
            (cwd / "safe.txt").write_text("clean test input\n", encoding="utf-8")
            git("add", "safe.txt")
            env = dict(os.environ, PATH=str(fake) + os.pathsep + os.environ["PATH"])
            hooks = Path(guard.__file__).resolve().parent.parent / ".githooks"
            check = subprocess.run(["sh", str(hooks / "pre-commit")], cwd=cwd,
                                   env=env, text=True, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)
            self.assertEqual(0, check.returncode, check.stderr)
            self.assertIn("Privacy guard passed", check.stdout)
            git("commit", "-qm", "safe commit")
            sha = git("rev-parse", "HEAD")
            ref_line = "refs/heads/feature {} refs/heads/feature {}\n".format(
                sha, "0" * 40)
            check = subprocess.run(["sh", str(hooks / "pre-push"), "origin",
                                    str(cwd / "public.git")], cwd=cwd, env=env,
                                   input=ref_line, text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertEqual(0, check.returncode, check.stderr)
            self.assertIn("Privacy guard passed", check.stdout)

    @unittest.skipIf(os.name == "nt", "POSIX shell test; Git for Windows uses Git Bash")
    def test_no_working_python_fails_closed(self):
        with tempfile.TemporaryDirectory() as work:
            cwd = Path(work)
            fake = cwd / "fakebin"
            fake.mkdir()
            for candidate in ("python3", "python", "py"):
                stub = fake / candidate
                stub.write_text("#!/bin/sh\nexit 37\n", encoding="utf-8")
                stub.chmod(0o755)
            subprocess.run(["git", "init", "-q"], cwd=cwd, check=True)
            env = dict(os.environ, PATH=str(fake) + os.pathsep + os.environ["PATH"])
            hook = Path(guard.__file__).resolve().parent.parent / ".githooks/pre-commit"
            result = subprocess.run(["sh", str(hook)], cwd=cwd, env=env,
                                    text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE)
            self.assertEqual(2, result.returncode)
            self.assertIn("commit blocked", result.stderr)


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
            (cwd / "ci").mkdir()
            (cwd / "ci/privacy_guard.py").write_bytes(
                Path(guard.__file__).resolve().read_bytes())
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


class IdentityIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory()
        self.addCleanup(self.work.cleanup)
        self.cwd = Path(self.work.name)
        self.env = os.environ.copy()
        for key in tuple(self.env):
            if key.startswith(("GIT_AUTHOR_", "GIT_COMMITTER_")):
                del self.env[key]
        self.git("init", "-q")
        self.git("config", "user.name", "Privacy Test")
        self.git("config", "user.email", "privacy-test@example.invalid")
        (self.cwd / "safe.txt").write_text("safe\n", encoding="utf-8")
        self.git("add", "safe.txt")
        self.git("commit", "-qm", "baseline")
        self.base = self.git("rev-parse", "HEAD").strip()

    def git(self, *args):
        return subprocess.run(["git", *args], cwd=self.cwd, env=self.env,
                              check=True, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True).stdout

    def scan(self, *args):
        return subprocess.run([sys.executable, str(Path(guard.__file__).resolve()),
                               *args], cwd=self.cwd, env=self.env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def assert_blocked(self, result, value, marker):
        self.assertEqual(1, result.returncode)
        self.assertIn(marker, result.stderr)
        self.assertNotIn(value, result.stdout + result.stderr)

    def test_staged_checks_effective_author_and_committer_overrides(self):
        for role in ("AUTHOR", "COMMITTER"):
            for field, value in (("NAME", "node" + ".internal"),
                                 ("EMAIL", "agent@node" + ".local")):
                key = "GIT_" + role + "_" + field
                with self.subTest(key=key):
                    self.env[key] = value
                    self.assert_blocked(self.scan("--staged"), value, role)
                    del self.env[key]

    def test_committed_metadata_checked_in_single_and_range_modes(self):
        for role in ("AUTHOR", "COMMITTER"):
            for field, value in (("NAME", "node" + ".internal"),
                                 ("EMAIL", "agent@node" + ".local")):
                key = "GIT_" + role + "_" + field
                with self.subTest(key=key):
                    self.env[key] = value
                    self.git("commit", "--allow-empty", "-qm", "safe subject")
                    del self.env[key]
                    self.assert_blocked(self.scan("--commit", "HEAD"), value, role)
                    self.assert_blocked(self.scan("--commits", self.base + "..HEAD"),
                                        value, role)

    def test_single_label_and_git_none_domains_blocked(self):
        for value in ("agent@synthetic-node", "agent@synthetic-node.(none)"):
            with self.subTest(value=value):
                self.env["GIT_AUTHOR_EMAIL"] = value
                self.assert_blocked(self.scan("--staged"), value, "HOST_DERIVED_IDENTITY")
                self.git("commit", "--allow-empty", "-qm", "safe subject")
                del self.env["GIT_AUTHOR_EMAIL"]
                self.assert_blocked(self.scan("--commit", "HEAD"), value,
                                    "HOST_DERIVED_IDENTITY")

    def test_safe_identities_pass(self):
        for value in ("agent@example.invalid", "agent@example.org",
                      "12345+agent@users.noreply.github.com"):
            with self.subTest(value=value):
                self.env["GIT_AUTHOR_EMAIL"] = value
                self.env["GIT_COMMITTER_EMAIL"] = value
                self.assertEqual(0, self.scan("--staged").returncode)
                self.git("commit", "--allow-empty", "-qm", "safe identity")
                self.assertEqual(0, self.scan("--commit", "HEAD").returncode)
                self.assertEqual(0, self.scan("--commits", self.base + "..HEAD").returncode)

    def test_staged_checks_configured_identity(self):
        value = "agent@node" + ".internal"
        self.git("config", "user.email", value)
        self.assert_blocked(self.scan("--staged"), value, "PRIVATE_HOSTNAME")

    def test_mailmap_and_later_safe_commit_do_not_hide_raw_identity(self):
        value = "agent@node" + ".local"
        self.env["GIT_AUTHOR_EMAIL"] = value
        self.git("commit", "--allow-empty", "-qm", "safe subject")
        sha = self.git("rev-parse", "HEAD").strip()
        del self.env["GIT_AUTHOR_EMAIL"]
        (self.cwd / ".mailmap").write_text(
            "Privacy Test <agent@example.invalid> <" + value + ">\n", encoding="utf-8")
        self.git("config", "log.mailmap", "true")
        self.git("commit", "--allow-empty", "-qm", "later safe identity")
        self.assertEqual(0, self.scan("--commit", "HEAD").returncode)
        self.assert_blocked(self.scan("--commit", sha), value, "AUTHOR")
        self.assert_blocked(self.scan("--commits", self.base + "..HEAD"), value, "AUTHOR")

    @unittest.skipIf(os.name == "nt", "POSIX shell hook integration runs on Linux/macOS")
    def test_first_push_hook_checks_commit_identity(self):
        public = self.cwd / "public.git"
        self.git("init", "--bare", "-q", str(public))
        self.git("remote", "add", "public", str(public))
        self.git("push", "-q", "public", "HEAD:refs/heads/main")
        value = "agent@node" + ".local"
        self.env["GIT_COMMITTER_EMAIL"] = value
        self.git("commit", "--allow-empty", "-qm", "safe subject")
        del self.env["GIT_COMMITTER_EMAIL"]
        sha = self.git("rev-parse", "HEAD").strip()
        (self.cwd / "ci").mkdir()
        source = Path(guard.__file__).resolve()
        (self.cwd / "ci/privacy_guard.py").write_bytes(source.read_bytes())
        hook = source.parent.parent / ".githooks/pre-push"
        result = subprocess.run(["sh", str(hook), "public", str(public)],
                                input="refs/heads/feature {} refs/heads/feature {}\n".format(
                                    sha, "0" * 40), cwd=self.cwd, env=self.env,
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assert_blocked(result, value, "COMMITTER")

    def test_unresolvable_effective_identity_fails_closed(self):
        self.env["GIT_AUTHOR_NAME"] = ""
        result = self.scan("--staged")
        self.assertEqual(2, result.returncode)
        self.assertIn("publication blocked", result.stderr)
        self.assertNotIn(str(self.cwd), result.stderr)


if __name__ == "__main__":
    unittest.main()
