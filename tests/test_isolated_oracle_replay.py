#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Safety/negative controls for the non-destructive CTest oracle wrapper."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import run_isolated_oracle_replay as replay


class IsolatedOracleReplayTests(unittest.TestCase):
    def test_repeated_runs_never_replace_an_old_or_foreign_receipt(self):
        with tempfile.TemporaryDirectory() as base:
            root = Path(base)
            fixtures = root / 'fixtures'
            fixtures.mkdir()
            parent = root / 'scratch'
            parent.mkdir()
            # Even *recognized* destination names may belong to someone else.
            prior = {name: root / 'scratch' / name for name in
                     ('comparison.json', 'control.avi', 'control.ffmpeg-p10')}
            for name, path in prior.items():
                path.write_bytes(('foreign:' + name).encode())
            scratch_paths = []
            before = sys.argv

            def simulated_oracle(script, run_name):
                self.assertEqual(run_name, '__main__')
                self.assertEqual(Path(script).name, 'compare_sdk_oracle.py')
                args = sys.argv
                self.assertEqual(args[1], '--windows-fixtures')
                self.assertEqual(args[3], '--ffmpeg')
                self.assertEqual(args[5], '--ffprobe')
                self.assertEqual(args[7], '--output')
                scratch = Path(args[8])
                self.assertTrue(scratch.is_dir())
                self.assertEqual(scratch.parent, parent)
                self.assertNotIn(scratch, prior.values())
                (scratch / 'comparison.json').write_text('new evidence')
                scratch_paths.append(scratch)

            replay.run_in_scratch(fixtures, 'ffmpeg', 'ffprobe', parent, simulated_oracle)
            replay.run_in_scratch(fixtures, 'ffmpeg', 'ffprobe', parent, simulated_oracle)
            self.assertEqual(len(set(scratch_paths)), 2)
            self.assertTrue(all(not scratch.exists() for scratch in scratch_paths))
            self.assertEqual(sys.argv, before)
            self.assertEqual(set(parent.iterdir()), set(prior.values()))
            for name, path in prior.items():
                self.assertEqual(path.read_bytes(), ('foreign:' + name).encode())

    def test_failure_still_cleans_its_own_scratch_not_foreign_evidence(self):
        with tempfile.TemporaryDirectory() as base:
            root = Path(base)
            fixtures = root / 'fixtures'
            fixtures.mkdir()
            parent = root / 'scratch'
            parent.mkdir()
            foreign = parent / 'comparison.json'
            foreign.write_bytes(b'prior-unaltered')
            before = sys.argv

            def failing(script, run_name):
                Path(sys.argv[-1], 'control.avi').write_bytes(b'temporary')
                raise ValueError('simulated failure')

            with self.assertRaisesRegex(ValueError, 'simulated failure'):
                replay.run_in_scratch(fixtures, 'ffmpeg', 'ffprobe', parent, failing)
            self.assertEqual(list(parent.iterdir()), [foreign])
            self.assertEqual(foreign.read_bytes(), b'prior-unaltered')
            self.assertEqual(sys.argv, before)

    def test_rejects_scratch_below_retained_fixture_source(self):
        with tempfile.TemporaryDirectory() as base:
            fixtures = Path(base) / 'fixtures'
            fixtures.mkdir()
            destination = fixtures / 'output'
            with self.assertRaisesRegex(ValueError, 'overlaps retained input'):
                replay.run_in_scratch(fixtures, 'ffmpeg', 'ffprobe', destination)
            self.assertFalse(destination.exists())

    def test_rejects_symlinked_parent_without_touching_target(self):
        with tempfile.TemporaryDirectory() as base:
            root = Path(base)
            fixtures = root / 'fixtures'
            fixtures.mkdir()
            target = root / 'target'
            target.mkdir()
            foreign = target / 'control.avi'
            foreign.write_bytes(b'keep')
            alias = root / 'alias'
            try:
                alias.symlink_to(target, target_is_directory=True)
            except (OSError, NotImplementedError):
                return  # A Windows runner without symlink privileges.
            with self.assertRaisesRegex(ValueError, 'invalid replay scratch parent'):
                replay.run_in_scratch(fixtures, 'ffmpeg', 'ffprobe', alias)
            self.assertEqual(foreign.read_bytes(), b'keep')

    def test_malformed_cli_does_not_disclose_private_values(self):
        args = ['run_isolated_oracle_replay.py', '--windows-fixtures',
                'PRIVATE_FIXTURE_PATH', '--ffmpeg', 'ffmpeg', '--ffprobe', 'ffprobe',
                '--scratch-parent', 'scratch', '--unexpected', 'PRIVATE_TOKEN']
        with patch.object(sys, 'argv', args):
            with self.assertRaisesRegex(ValueError, '^invalid isolated oracle arguments$') as caught:
                replay.main()
            self.assertNotIn('PRIVATE_TOKEN', str(caught.exception))
            self.assertNotIn('PRIVATE_FIXTURE_PATH', str(caught.exception))

    def test_workflow_calls_wrapper_and_preserves_original_oracle(self):
        root = Path(__file__).resolve().parents[1]
        cmake = (root / 'CMakeLists.txt').read_text(encoding='utf-8')
        self.assertIn('tests/run_isolated_oracle_replay.py', cmake)
        self.assertIn('--scratch-parent', cmake)
        self.assertNotIn('--replace-owned-output', cmake)
        gate = (root / 'ci/run_standalone.py').read_text(encoding='utf-8')
        self.assertIn('standalone_oracle_replay_isolation', gate)
        oracle = (root / 'tests/compare_sdk_oracle.py').read_text(encoding='utf-8')
        self.assertNotIn('clear_owned_destinations', oracle)
        self.assertIn('not target.exists() and not target.is_symlink()', oracle)


if __name__ == '__main__':
    unittest.main()
