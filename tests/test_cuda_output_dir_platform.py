# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
import contextlib
import io
import unittest
from pathlib import Path
from unittest.mock import patch

import validate_cuda_filmscan3_output_dir as gate


class PlatformPrerequisiteTests(unittest.TestCase):
    def test_windows_privilege_only(self):
        error = OSError('missing privilege')
        error.winerror = 1314
        path = Path()
        with patch.object(gate.os, 'name', 'nt'), patch.object(Path, 'symlink_to', side_effect=error):
            self.assertFalse(gate.create_directory_symlink(path, path))
        with patch.object(gate.os, 'name', 'posix'), patch.object(Path, 'symlink_to', side_effect=error):
            with self.assertRaises(OSError):
                gate.create_directory_symlink(path, path)

    def test_unrelated_errors_are_fatal(self):
        for error in (OSError('unrelated'), FileExistsError('collision'), PermissionError('denied')):
            with patch.object(Path, 'symlink_to', side_effect=error):
                with self.assertRaises(OSError):
                    gate.create_directory_symlink(Path(), Path())

    def test_unavailable_symlink_is_not_pass(self):
        with patch.object(gate, 'create_directory_symlink', return_value=False), \
             patch('sys.argv', ['output-dir-test']), contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(gate.main(), 77)
        self.assertIn('BLOCKED_SYMLINK_PRIVILEGE', out.getvalue())
        self.assertNotIn('CUDA-OUTPUT-DIR-NEGATIVE: PASS', out.getvalue())
        self.assertGreater(gate.checks, 10)

    def test_sentinel_failure_dominates_missing_privilege(self):
        original = gate.v.prepare_output_dir

        def destructive(directory):
            original(directory)
            sentinel = directory / 'KEEP_PARENT'
            if sentinel.exists():
                sentinel.write_bytes(b'corrupted')

        with patch.object(gate, 'create_directory_symlink', return_value=False), \
             patch.object(gate.v, 'prepare_output_dir', side_effect=destructive), \
             patch('sys.argv', ['output-dir-test']), contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(gate.main(), 1)
        self.assertIn('FAIL:', out.getvalue())
        self.assertNotIn('BLOCKED_SYMLINK_PRIVILEGE', out.getvalue())


if __name__ == '__main__':
    unittest.main()
