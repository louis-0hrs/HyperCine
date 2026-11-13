#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Reject raw local command paths in published Windows reference reports."""
import inspect
import json
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


if __name__ == '__main__':
    unittest.main()
