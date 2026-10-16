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

    def test_nested_private_remote_cannot_hide_first_public_push(self):
        value = "192." + "168.8.9"
        self.import_commit(text="network " + value + "\n")
        self.git("init", "--bare", "-q", str(self.root / "private.git"))
        self.git("remote", "add", "public/private", str(self.root / "private.git"))
        subprocess.run(["git", "push", "-q", str(self.root / "private.git"),
                        "HEAD:refs/heads/feature"], cwd=self.root / "incoming",
                       env=self.env, check=True, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
        self.git("fetch", "-q", "public/private")
        result = self.git("push", "-q", "public", "HEAD:refs/heads/review", expected=1)
        self.rejected(result, "OVERLAPPING_REMOTE_NAMESPACES", value)
        self.unpublished()

    def test_nested_destination_remote_fails_closed(self):
        self.git("remote", "add", "public/review", str(self.root / "public.git"))
        result = self.git("push", "-q", "public/review",
                          "HEAD:refs/heads/review", expected=1)
        self.rejected(result, "OVERLAPPING_REMOTE_NAMESPACES", "unused-private-value")
        self.unpublished()

    def test_remote_names_with_shared_text_prefix_can_push(self):
        self.git("remote", "add", "public-review", str(self.root / "public.git"))
        self.commit()
        self.git("push", "-q", "public", "HEAD:refs/heads/review")
        self.assertEqual(self.git("rev-parse", "HEAD").stdout.strip(),
                         self.git("ls-remote", "public", "refs/heads/review").stdout.split()[0])

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


    def tag(self, name="release-test", message="safe release", target="HEAD"):
        self.git("tag", "-a", name, "-m", message, target)

    def push_rejected(self, ref, marker, value, *extra):
        result = self.git("push", "-q", *extra, "public", ref, expected=1)
        self.rejected(result, marker, value)
        self.assertEqual("", self.git("ls-remote", "public", ref).stdout)

    def test_annotated_tag_on_published_commit_rejects_message(self):
        value = "192." + "168.8.9"
        self.tag(message="network " + value)
        self.push_rejected("refs/tags/release-test", "TAG_METADATA", value)

    def test_tag_rejects_private_tagger_email(self):
        value = "release@node" + ".internal"
        self.identity("Release Example", value)
        self.tag()
        self.push_rejected("refs/tags/release-test", "TAGGER", value)

    def test_tag_rejects_host_inferred_tagger_domain(self):
        value = "release@node"
        self.identity("Release Example", value)
        self.tag()
        self.push_rejected("refs/tags/release-test", "HOST_DERIVED_IDENTITY", value)

    def test_tag_rejects_private_tagger_name(self):
        value = "10." + "4.6.8"
        self.identity("Release " + value, "release@example.org")
        self.tag()
        self.push_rejected("refs/tags/release-test", "TAGGER", value)

    def test_nested_annotated_tag_rejects_inner_message(self):
        value = "10." + "4.6.8"
        self.tag("inner", "network " + value)
        self.tag("outer", target="inner")
        self.push_rejected("refs/tags/outer", "TAG_METADATA", value)

    def test_sensitive_local_branch_name_rejected(self):
        value = "10." + "4.6.8"
        self.git("branch", "review-" + value)
        result = self.git("push", "-q", "public", "review-" + value +
                          ":refs/heads/review", expected=1)
        self.rejected(result, "LOCAL_REF", value)
        self.unpublished()

    def test_sensitive_remote_branch_name_rejected(self):
        value = "10." + "4.6.8"
        self.push_rejected("HEAD:refs/heads/review-" + value, "REMOTE_REF", value)

    def test_sensitive_lightweight_tag_name_rejected(self):
        value = "node" + ".internal"
        self.git("tag", value)
        self.push_rejected("refs/tags/" + value, "LOCAL_REF", value)

    def test_clean_annotated_and_lightweight_tags_pass(self):
        self.tag()
        self.git("tag", "light-test")
        self.git("push", "-q", "public", "refs/tags/release-test", "refs/tags/light-test")
        self.assertEqual(3, len(self.git("ls-remote", "public", "refs/tags/*").stdout.splitlines()))

    def test_clean_nested_tags_pass(self):
        self.tag("inner")
        self.tag("outer", target="inner")
        self.git("push", "-q", "public", "refs/tags/outer")

    def test_repointed_tag_rejects_metadata(self):
        self.tag()
        self.git("push", "-q", "public", "refs/tags/release-test")
        before = self.git("ls-remote", "public", "refs/tags/release-test").stdout
        value = "10." + "4.6.8"
        self.git("tag", "-f", "-a", "release-test", "-m", "network " + value)
        result = self.git("push", "-q", "--force", "public", "refs/tags/release-test", expected=1)
        self.rejected(result, "TAG_METADATA", value)
        self.assertEqual(before, self.git("ls-remote", "public", "refs/tags/release-test").stdout)

    def test_follow_tags_rejects_metadata(self):
        value = "10." + "4.6.8"
        self.commit()
        self.tag(message="network " + value)
        result = self.git("push", "-q", "--follow-tags", "public",
                          "HEAD:refs/heads/review", expected=1)
        self.rejected(result, "TAG_METADATA", value)
        self.unpublished()
        self.assertEqual("", self.git("ls-remote", "public", "refs/tags/*").stdout)

    def test_clean_delete_passes(self):
        self.tag()
        self.git("push", "-q", "public", "refs/tags/release-test")
        self.git("push", "-q", "--delete", "public", "release-test")
        self.assertEqual("", self.git("ls-remote", "public", "refs/tags/*").stdout)

    def test_blob_ref_fails_closed(self):
        blob = self.git("hash-object", "-w", "ci/privacy_guard.py").stdout.strip()
        self.git("update-ref", "refs/tags/blob-test", blob)
        self.push_rejected("refs/tags/blob-test", "UNSUPPORTED_OBJECT_TYPE", "unused-private-value")

    def test_tag_to_tree_fails_closed(self):
        self.tag(target="HEAD^{tree}")
        self.push_rejected("refs/tags/release-test", "UNSUPPORTED_OBJECT_TYPE", "unused-private-value")


    def test_sensitive_remote_tag_name_rejected(self):
        value = "node" + ".internal"
        self.git("tag", "light-test")
        self.push_rejected("refs/tags/light-test:refs/tags/" + value,
                           "REMOTE_REF", value)

    def test_home_shaped_ref_name_rejected(self):
        value = "Users/" + "synthetic-agent/review"
        self.git("branch", value)
        result = self.git("push", "-q", "public", value +
                          ":refs/heads/review", expected=1)
        self.rejected(result, "PERSONAL_ABSOLUTE_PATH", value)
        self.unpublished()

    def test_clean_repointed_annotated_tag_passes(self):
        self.tag()
        self.git("push", "-q", "public", "refs/tags/release-test")
        self.commit()
        self.git("tag", "-f", "-a", "release-test", "-m", "safe revision")
        self.git("push", "-q", "--force", "public", "refs/tags/release-test")
        actual = self.git("ls-remote", "public", "refs/tags/release-test").stdout.split()[0]
        self.assertEqual(self.git("rev-parse", "release-test").stdout.strip(), actual)

    def test_mixed_refs_reject_entire_push(self):
        value = "10." + "4.6.8"
        self.tag(message="network " + value)
        result = self.git("push", "-q", "public", "HEAD:refs/heads/review",
                          "refs/tags/release-test", expected=1)
        self.rejected(result, "TAG_METADATA", value)
        self.unpublished()
        self.assertEqual("", self.git("ls-remote", "public", "refs/tags/*").stdout)

    def raw_tag(self, payload):
        path = self.root / "tag-object"
        path.write_bytes(payload)
        oid = self.git("hash-object", "-t", "tag", "-w", "--literally",
                       str(path)).stdout.strip()
        self.git("update-ref", "refs/tags/raw-test", oid)
        return oid

    def test_tag_without_tagger_fails_closed(self):
        self.raw_tag(("object " + self.base + "\ntype commit\ntag raw-test\n\nsafe\n").encode())
        self.push_rejected("refs/tags/raw-test", "PRIVACY GUARD ERROR", "unused-private-value")

    def test_non_utf8_tag_fails_closed(self):
        self.raw_tag(("object " + self.base + "\ntype commit\ntag raw-test\n" +
                      "tagger Release Example <release@example.org> 1 +0000\n\n").encode() + b"\xff")
        self.push_rejected("refs/tags/raw-test", "PRIVACY GUARD ERROR", "unused-private-value")

    def test_replacement_object_cannot_hide_sensitive_tag(self):
        value = "10." + "4.6.8"
        self.tag("unsafe", "network " + value)
        self.tag("safe")
        self.git("replace", "unsafe", "safe")
        self.push_rejected("refs/tags/unsafe", "TAG_METADATA", value)

    def test_tag_internal_name_is_scanned(self):
        value = "node" + ".internal"
        self.tag(value)
        self.git("update-ref", "refs/tags/review", self.git("rev-parse", value).stdout.strip())
        self.push_rejected("refs/tags/review", "TAG_METADATA", value)


    def test_home_shaped_custom_local_ref_rejected(self):
        value = "Users/" + "synthetic-agent/review"
        ref = "refs/archive/" + value
        self.git("update-ref", ref, self.base)
        result = self.git("push", "-q", "public", ref +
                          ":refs/heads/review", expected=1)
        self.rejected(result, "LOCAL_REF", value)
        self.unpublished()

    def test_home_shaped_custom_remote_ref_rejected(self):
        value = "home/" + "synthetic-agent/review"
        ref = "refs/archive/" + value
        result = self.git("push", "-q", "public", "HEAD:" + ref, expected=1)
        self.rejected(result, "REMOTE_REF", value)
        self.assertEqual("", self.git("ls-remote", "public", ref).stdout)

    def test_clean_custom_commit_ref_and_delete_pass(self):
        ref = "refs/archive/review"
        self.git("update-ref", ref, self.base)
        self.git("push", "-q", "public", ref)
        actual = self.git("ls-remote", "public", ref).stdout.split()[0]
        self.assertEqual(self.base, actual)
        self.git("push", "-q", "public", ":" + ref)
        self.assertEqual("", self.git("ls-remote", "public", ref).stdout)


if __name__ == "__main__":
    unittest.main()
