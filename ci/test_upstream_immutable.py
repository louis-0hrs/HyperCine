#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Real temporary-Git controls; the repository's upstream/ is never modified."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

if __package__:
    from . import check_upstream_immutable as checker
else:
    import check_upstream_immutable as checker

FROZEN_UPSTREAM_TREE = checker.FROZEN_UPSTREAM_TREE
verify_upstream = checker.verify_upstream


class FrozenUpstreamTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Build one synthetic seed, then copy it into a private per-test repo.
        # Windows Git process startup made 26 identical init/config/commit
        # sequences exceed the existing 120-second CTest deadline. No tested
        # mutation or verifier invocation is shared, cached or omitted.
        cls.seed_temp = tempfile.TemporaryDirectory(prefix='frozen-reference-seed-')
        cls.addClassCleanup(cls.seed_temp.cleanup)
        cls.seed = Path(cls.seed_temp.name) / 'repository'
        cls.seed.mkdir()
        cls.fixture_env = dict(os.environ, GIT_CONFIG_NOSYSTEM='1',
                               GIT_CONFIG_GLOBAL=os.devnull, GIT_TERMINAL_PROMPT='0')
        for key in ('GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE', 'GIT_COMMON_DIR',
                    'GIT_OBJECT_DIRECTORY', 'GIT_ALTERNATE_OBJECT_DIRECTORIES',
                    'GIT_AUTHOR_NAME', 'GIT_AUTHOR_EMAIL', 'GIT_COMMITTER_NAME',
                    'GIT_COMMITTER_EMAIL'):
            cls.fixture_env.pop(key, None)
        def seed_git(*args):
            return subprocess.run(['git', '-C', str(cls.seed), *args],
                                  capture_output=True, env=cls.fixture_env,
                                  timeout=30, check=True).stdout
        seed_git('init', '-q')
        for key, value in (('user.name', 'HyperCine Test'),
                           ('user.email', 'test@users.noreply.github.com'),
                           ('commit.gpgsign', 'false'), ('core.autocrlf', 'false'),
                           ('core.hooksPath', 'disabled-test-hooks')):
            seed_git('config', key, value)
        (cls.seed / 'upstream').mkdir()
        (cls.seed / 'upstream' / 'reference.txt').write_bytes(b'frozen reference\n')
        seed_git('add', '--', 'upstream')
        seed_git('commit', '-qm', 'test: synthetic frozen reference')
        cls.pristine = seed_git('rev-parse', 'HEAD').strip().decode()
        cls.tree = seed_git('rev-parse', 'HEAD:upstream').strip().decode()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name) / 'repository'
        # copytree copies Git objects/index/config; it does not create a linked
        # worktree, alternate object store or hard links to the shared seed.
        shutil.copytree(self.seed, self.repo)
        self.env = self.fixture_env.copy()
        self.source = self.repo / 'upstream' / 'reference.txt'

    def git(self, *args, repo=None, input_data=None):
        result = subprocess.run(['git', '-C', str(repo or self.repo), *args],
                                input=input_data, capture_output=True,
                                env=self.env, timeout=30)
        self.assertEqual(result.returncode, 0, 'synthetic Git fixture command failed')
        return result.stdout

    def verify(self, repo=None):
        with patch.dict(os.environ, self.env, clear=True):
            return verify_upstream(repo or self.repo, expected_tree=self.tree)

    def test_fixture_copy_cannot_mutate_seed_bytes_or_index(self):
        seed_source = self.seed / 'upstream' / 'reference.txt'
        seed_index = self.seed / '.git' / 'index'
        original_source = seed_source.read_bytes()
        original_index = seed_index.read_bytes()
        self.source.write_bytes(b'changed private fixture\n')
        self.git('add', '--', 'upstream')
        self.assertEqual(seed_source.read_bytes(), original_source)
        self.assertEqual(seed_index.read_bytes(), original_index)
        with self.assertRaisesRegex(ValueError, 'upstream index'):
            self.verify()

    def test_clean_reference_passes(self):
        self.assertEqual(self.verify(), self.tree)

    def test_real_repository_matches_production_pin(self):
        root = Path(__file__).resolve().parents[1]
        self.assertEqual(verify_upstream(root), FROZEN_UPSTREAM_TREE)

    def test_committed_modification_with_clean_status_fails(self):
        self.source.write_bytes(b'changed reference\n')
        self.git('add', '--', 'upstream')
        self.git('commit', '-qm', 'test: synthetic committed drift')
        self.assertEqual(self.git('status', '--porcelain', '--', 'upstream/'), b'')
        with self.assertRaisesRegex(ValueError, 'committed upstream tree'):
            self.verify()

    def test_committed_addition_fails(self):
        (self.repo / 'upstream' / 'extra.h').write_bytes(b'extra\n')
        self.git('add', '--', 'upstream')
        self.git('commit', '-qm', 'test: synthetic addition')
        with self.assertRaisesRegex(ValueError, 'committed upstream tree'):
            self.verify()

    def test_committed_deletion_fails(self):
        self.git('rm', '--', 'upstream/reference.txt')
        self.git('commit', '-qm', 'test: synthetic deletion')
        with self.assertRaises(ValueError):
            self.verify()

    def test_staged_modification_fails_even_if_worktree_restored(self):
        self.source.write_bytes(b'changed reference\n')
        self.git('add', '--', 'upstream')
        self.source.write_bytes(b'frozen reference\n')
        with self.assertRaisesRegex(ValueError, 'upstream index'):
            self.verify()

    def test_staged_addition_fails(self):
        (self.repo / 'upstream' / 'extra.h').write_bytes(b'extra\n')
        self.git('add', '--', 'upstream')
        with self.assertRaisesRegex(ValueError, 'upstream index'):
            self.verify()

    def test_staged_deletion_fails(self):
        self.git('rm', '--cached', '--', 'upstream/reference.txt')
        with self.assertRaisesRegex(ValueError, 'upstream index'):
            self.verify()

    def test_unstaged_modification_fails(self):
        self.source.write_bytes(b'changed reference\n')
        with self.assertRaisesRegex(ValueError, 'worktree bytes'):
            self.verify()

    def test_unstaged_deletion_fails(self):
        self.source.unlink()
        with self.assertRaisesRegex(ValueError, 'worktree is incomplete'):
            self.verify()

    def test_untracked_addition_fails(self):
        (self.repo / 'upstream' / 'extra.h').write_bytes(b'extra\n')
        with self.assertRaisesRegex(ValueError, 'untracked files'):
            self.verify()

    def test_ignored_addition_fails(self):
        (self.repo / '.gitignore').write_text('upstream/extra.h\n')
        (self.repo / 'upstream' / 'extra.h').write_bytes(b'extra\n')
        self.assertEqual(self.git('status', '--porcelain', '--', 'upstream/'), b'')
        with self.assertRaisesRegex(ValueError, 'untracked files'):
            self.verify()

    def test_ignorecase_cannot_hide_a_distinct_extra_file(self):
        extra = self.repo / 'upstream' / 'REFERENCE.TXT'
        if extra.exists():
            self.skipTest('filesystem cannot store both case variants')
        self.git('config', 'core.ignorecase', 'true')
        extra.write_bytes(b'new shadow file\n')
        self.assertEqual(self.git('ls-files', '--others', '-z', '--', 'upstream/'), b'')
        with self.assertRaisesRegex(ValueError, 'untracked files'):
            self.verify()

    def test_assume_unchanged_cannot_hide_modified_bytes(self):
        self.git('update-index', '--assume-unchanged', '--', 'upstream/reference.txt')
        self.source.write_bytes(b'changed reference\n')
        self.assertEqual(self.git('status', '--porcelain', '--', 'upstream/'), b'')
        with self.assertRaisesRegex(ValueError, 'worktree bytes'):
            self.verify()

    def test_skip_worktree_cannot_hide_modified_bytes(self):
        self.git('update-index', '--skip-worktree', '--', 'upstream/reference.txt')
        self.source.write_bytes(b'changed reference\n')
        self.assertEqual(self.git('status', '--porcelain', '--', 'upstream/'), b'')
        with self.assertRaisesRegex(ValueError, 'worktree bytes'):
            self.verify()

    def test_replace_object_cannot_hide_committed_drift(self):
        self.source.write_bytes(b'changed reference\n')
        self.git('add', '--', 'upstream')
        self.git('commit', '-qm', 'test: synthetic hidden drift')
        changed = self.git('rev-parse', 'HEAD').strip().decode()
        self.git('replace', changed, self.pristine)
        self.assertEqual(self.git('rev-parse', 'HEAD:upstream').strip().decode(), self.tree)
        with self.assertRaisesRegex(ValueError, 'committed upstream tree'):
            self.verify()

    def test_legitimate_crlf_checkout_passes(self):
        self.git('config', 'core.autocrlf', 'true')
        self.source.unlink()
        self.git('checkout-index', '--force', '--', 'upstream/reference.txt')
        self.assertIn(b'\r\n', self.source.read_bytes())
        self.assertEqual(self.verify(), self.tree)

    def test_original_crlf_blob_is_not_reinterpreted_as_lf(self):
        # The real SDK pin itself contains CRLF blobs. core.autocrlf must not
        # reinterpret an already byte-identical original file as a changed one.
        self.source.write_bytes(b'frozen reference\r\n')
        self.git('add', '--', 'upstream')
        self.git('commit', '-qm', 'test: synthetic original CRLF reference')
        self.tree = self.git('rev-parse', 'HEAD:upstream').strip().decode()
        self.git('config', 'core.autocrlf', 'true')
        raw = self.git('hash-object', '--no-filters', 'upstream/reference.txt')
        cleaned = self.git('hash-object', 'upstream/reference.txt')
        self.assertNotEqual(raw, cleaned)
        self.assertEqual(self.verify(), self.tree)

    def test_binary_newline_changes_are_not_normalized(self):
        self.source.write_bytes(b'binary\0reference\n')
        self.git('add', '--', 'upstream')
        self.git('commit', '-qm', 'test: synthetic binary reference')
        self.tree = self.git('rev-parse', 'HEAD:upstream').strip().decode()
        self.source.write_bytes(b'binary\0reference\r\n')
        with self.assertRaisesRegex(ValueError, 'worktree bytes'):
            self.verify()

    def test_clean_filter_cannot_conceal_drift_and_is_never_executed(self):
        canary = self.repo / 'filter-was-executed'
        driver = self.repo / 'restore-filter.py'
        driver.write_text('import sys\nfrom pathlib import Path\n'
                          'sys.stdin.buffer.read()\n'
                          'Path(sys.argv[1]).write_text("executed")\n'
                          'sys.stdout.buffer.write(b"frozen reference\\n")\n')
        command = ' '.join(shlex.quote(Path(part).as_posix())
                           for part in (sys.executable, driver, canary))
        self.git('config', 'filter.restore.clean', command)
        (self.repo / '.gitattributes').write_text('upstream/reference.txt filter=restore\n')
        self.source.write_bytes(b'changed reference\n')
        # A real Git diff is falsely clean because the filter emits the original.
        self.assertEqual(self.git('diff', 'HEAD', '--', 'upstream/'), b'')
        self.assertTrue(canary.exists())
        canary.unlink()
        with self.assertRaisesRegex(ValueError, 'worktree bytes'):
            self.verify()
        self.assertFalse(canary.exists(), 'the verifier must not execute clean filters')
        self.source.write_bytes(b'frozen reference\n')
        self.assertEqual(self.verify(), self.tree)
        self.assertFalse(canary.exists(), 'even a successful gate must not run filters')

    @unittest.skipUnless(os.name == 'posix', 'native executable bits require POSIX')
    def test_assume_unchanged_cannot_hide_executable_mode_drift(self):
        self.git('config', 'core.filemode', 'true')
        self.git('update-index', '--assume-unchanged', '--', 'upstream/reference.txt')
        self.source.chmod(self.source.stat().st_mode | 0o111)
        with self.assertRaisesRegex(ValueError, 'executable mode'):
            self.verify()

    def test_shallow_checkout_needs_no_original_commit(self):
        (self.repo / 'unrelated.txt').write_bytes(b'other commit\n')
        self.git('add', '--', 'unrelated.txt')
        self.git('commit', '-qm', 'test: unrelated descendant')
        clone = Path(self.temp.name) / 'shallow'
        self.git('clone', '-q', '--depth=1', self.repo.as_uri(), str(clone))
        self.assertEqual(self.git('rev-parse', '--is-shallow-repository', repo=clone).strip(), b'true')
        self.assertEqual(self.verify(clone), self.tree)

    def test_unresolved_index_fails(self):
        blob = self.git('rev-parse', 'HEAD:upstream/reference.txt').strip()
        self.git('update-index', '--force-remove', '--', 'upstream/reference.txt')
        self.git('update-index', '--index-info', input_data=(
            b'100644 ' + blob + b' 1\tupstream/reference.txt\n' +
            b'100644 ' + blob + b' 2\tupstream/reference.txt\n'))
        with self.assertRaisesRegex(ValueError, 'upstream index'):
            self.verify()

    def test_nonrepository_fails(self):
        with self.assertRaisesRegex(ValueError, 'Git verification failed'):
            self.verify(Path(self.temp.name))

    def test_nested_invocation_cannot_verify_another_root(self):
        with self.assertRaisesRegex(ValueError, 'repository root'):
            self.verify(self.repo / 'upstream')

    def test_missing_git_fails_closed(self):
        with patch.object(checker.subprocess, 'run', side_effect=FileNotFoundError), \
             self.assertRaisesRegex(ValueError, 'Git verification unavailable'):
            self.verify()


if __name__ == '__main__':
    unittest.main()
