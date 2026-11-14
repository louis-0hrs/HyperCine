#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Reject raw local command paths in published Windows reference reports."""
import inspect
import json
import os
from unittest.mock import patch
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import validate_windows_reference as windows
from validation_support import portable_command


class WindowsReportProvenance(unittest.TestCase):
    def test_real_report_wires_portable_template(self):
        source = inspect.getsource(windows.main)
        self.assertIn(
            "'command_template': portable_command('validate_windows_reference.py', sys.argv[1:])",
            source)
        self.assertNotIn("'command': [sys.executable] + sys.argv", source)

    def test_private_paths_cannot_enter_command_template(self):
        # Synthetic secrets only. Avoid recording real machine/device paths.
        args = ['--probe', '/private/SECRET_PROBE',
                '--corpus-root', '/private/SECRET_CORPUS',
                '--output', '/private/SECRET_REPORT',
                '--stage-dir', '/private/SECRET_STAGE',
                '--frames', '0', '3', '6', '13', '--inputs-only']
        template = portable_command('validate_windows_reference.py', args)
        self.assertEqual(template, [
            'python3', 'tests/validate_windows_reference.py',
            '--probe', '${HYPERCINE_PROBE}',
            '--corpus-root', '${HYPERCINE_GOLDEN_DIR}',
            '--output', '${HYPERCINE_REPORT}',
            '--stage-dir', '${HYPERCINE_STAGE_DIR}',
            '--frames', '0', '3', '6', '13', '--inputs-only'])
        self.assertNotIn('SECRET_', json.dumps(template))
        # Mutation control: the retired command array leaks all four sentinels.
        self.assertIn('SECRET_PROBE', json.dumps(['python3'] + args))

    def test_unknown_switch_is_rejected_instead_of_published(self):
        with self.assertRaises(ValueError):
            portable_command('validate_windows_reference.py',
                             ['--probe', '/private/SECRET_PROBE',
                              '--unrecognized', '/private/SECRET_PAYLOAD'])


    def test_compiler_label_strips_paths_and_refuses_unqualified_values(self):
        # Exercise the same report-field provider as the real CLI.
        cases = [
            ('GNU 14.2.0 (/private/SECRET_EXAMPLE_COMPILER_PATH)', 'GNU 14.2.0'),
            ('MSVC 19.44.35211.0 (C:/Synthetic/SECRET_EXAMPLE_PATH/cl.exe)', 'MSVC 19.44.35211.0'),
            ('AppleClang 16.0.0 (/synthetic/SECRET_EXAMPLE_PATH/clang++)', 'AppleClang 16.0.0'),
            ('Clang 18.1.7', 'Clang 18.1.7'),
            ('IntelLLVM 2026.1.0', 'IntelLLVM 2026.1.0'),
            ('/private/SECRET_EXAMPLE_PATH GNU 14.2', 'NOT_RECORDED'),
            ('GNU 14.2/SECRET_EXAMPLE_SUFFIX', 'NOT_RECORDED'),
            ('GNU 14.SECRET_EXAMPLE_VERSION', 'NOT_RECORDED'),
            ('SECRET_EXAMPLE_COMPILER_PATH', 'NOT_RECORDED'),
            ('', 'NOT_RECORDED'),
        ]
        for configured, expected in cases:
            with self.subTest(configured=expected), patch.dict(
                    os.environ, {'HYPERCINE_TEST_COMPILER': configured}):
                actual = windows.compiler_info()
                self.assertEqual(actual, expected)
                self.assertNotIn('SECRET_EXAMPLE_', actual)
                self.assertNotIn('/', actual)
                self.assertNotIn('\\\\', actual)
        with patch.dict(os.environ, {}, clear=True):
            self.assertEqual(windows.compiler_info(), 'NOT_RECORDED')

    def test_report_compiler_provider_does_not_run_unbounded_version_probe(self):
        # A fake banner might contain a home path or token: never invoke it.
        self.assertIn("'compiler': compiler_info()", inspect.getsource(windows.main))
        with patch.dict(os.environ, {'HYPERCINE_TEST_COMPILER': 'GNU 14.2.0 (/private/SECRET_EXAMPLE_PATH)'}):
            with patch.object(windows.subprocess, 'check_output',
                              side_effect=AssertionError('version banner must not execute')) as probe:
                self.assertEqual(windows.compiler_info(), 'GNU 14.2.0')
                probe.assert_not_called()

    def test_missing_probe_does_not_leak_path(self):
        # Real CLI failure route, but only a disposable synthetic path.
        with tempfile.TemporaryDirectory(prefix='hypercine-report-privacy-') as root:
            missing = str(Path(root) / 'SECRET_EXAMPLE_MISSING_PROBE')
            completed = subprocess.run(
                [sys.executable, '-B', str(Path(windows.__file__).resolve()),
                 '--probe', missing, '--inputs-only'],
                capture_output=True, text=True, timeout=20, check=False)
            self.assertEqual(completed.returncode, 1)
            output = completed.stdout + completed.stderr
            self.assertIn('FAIL: Windows reference validation rejected', output)
            self.assertNotIn('SECRET_EXAMPLE_MISSING_PROBE', output)
            self.assertNotIn(missing, output)

    def test_invalid_arguments_never_echo_private_values(self):
        # Default argparse diagnostics echo unknown switches and invalid integers.
        # Test the real entrypoint, with disposable synthetic sentinels only.
        script = str(Path(windows.__file__).resolve())
        cases = (
            (['--unexpected', '/private/SECRET_UNKNOWN_ARGUMENT'], 'SECRET_UNKNOWN_ARGUMENT'),
            (['--frames', 'SECRET_FRAME_NUMBER'], 'SECRET_FRAME_NUMBER'),
        )
        for extra, marker in cases:
            with self.subTest(marker=marker):
                completed = subprocess.run(
                    [sys.executable, '-B', script, '--probe',
                     '/private/SECRET_FAKE_PROBE', *extra],
                    capture_output=True, text=True, timeout=20, check=False)
                self.assertEqual(completed.returncode, 1)
                output = completed.stdout + completed.stderr
                self.assertIn('FAIL: Windows reference validation rejected', output)
                self.assertNotIn(marker, output)
                self.assertNotIn('SECRET_FAKE_PROBE', output)


if __name__ == '__main__':
    unittest.main()
