# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Deterministic install-gate regressions; no package manager or network needed."""
import contextlib
import io
import subprocess
import unittest
from unittest.mock import patch

import install_windows_ffmpeg as gate


def completed(code=0, stdout='', stderr=''):
    return subprocess.CompletedProcess([], code, stdout, stderr)


class InstallTests(unittest.TestCase):
    def run_install(self, results, probes):
        with patch.object(gate.subprocess, 'run', side_effect=results) as run, \
                patch.object(gate, 'probe_tools', side_effect=probes) as probe, \
                patch.object(gate.time, 'sleep') as sleep, \
                contextlib.redirect_stdout(io.StringIO()) as output:
            code = gate.install()
        return code, run.call_count, probe.call_count, sleep.call_count, output.getvalue()

    def test_success_requires_both_functional_tools(self):
        self.assertEqual(self.run_install([completed()], [True])[:4], (0, 1, 1, 0))

    def test_zero_exit_with_missing_tools_fails(self):
        self.assertEqual(self.run_install([completed()], [False])[:4], (1, 1, 1, 0))

    def test_original_zero_exit_503_retries_then_succeeds(self):
        results = [completed(stdout='HTTP 503 Service Unavailable; installed 0/0 packages'), completed()]
        outcome = self.run_install(results, [False, True])
        self.assertEqual(outcome[:4], (0, 2, 2, 1))
        self.assertIn('HTTP=503', outcome[4])

    def test_nonzero_503_retries(self):
        self.assertEqual(self.run_install([completed(1, stderr='(503) Service Unavailable'),
                                           completed()], [False, True])[:4], (0, 2, 2, 1))

    def test_chocolatey_feed_status_retries(self):
        for status in ('429', '500', '502', '503', '504'):
            for exit_code in (0, 1):
                with self.subTest(status=status, exit_code=exit_code):
                    message = ('Response status code does not indicate success: '
                               + status + ' (Service Unavailable).')
                    outcome = self.run_install([completed(exit_code, stderr=message),
                                                completed()], [False, True])
                    self.assertEqual(outcome[:4], (0, 2, 2, 1))
                    self.assertIn('HTTP=' + status, outcome[4])

    def test_chocolatey_feed_retries_are_bounded(self):
        message = 'Response status code does not indicate success: 503 (Service Unavailable).'
        self.assertEqual(self.run_install([completed(0, message)] * 3,
                                           [False] * 3)[:4], (1, 3, 3, 2))

    def test_chocolatey_permanent_feed_status_does_not_retry(self):
        for status in ('400', '401', '403', '404'):
            with self.subTest(status=status):
                message = 'Response status code does not indicate success: ' + status
                self.assertEqual(self.run_install([completed(1, message)],
                                                   [False])[:4], (1, 1, 1, 0))

    def test_transient_retries_are_bounded(self):
        self.assertEqual(self.run_install([completed(1, 'HTTP 503')] * 3,
                                           [False] * 3)[:4], (1, 3, 3, 2))

    def test_permanent_failure_does_not_retry_even_with_existing_tools(self):
        self.assertEqual(self.run_install([completed(1, 'HTTP 404 package not found')],
                                           [True])[:4], (1, 1, 1, 0))

    def test_installer_timeout_retries(self):
        self.assertEqual(self.run_install([subprocess.TimeoutExpired('choco', 600), completed()],
                                           [False, True])[:4], (0, 2, 2, 1))

    def test_missing_installer_fails_without_retry(self):
        self.assertEqual(self.run_install([FileNotFoundError('private detail')],
                                           [False])[:4], (1, 1, 1, 0))

    def test_raw_installer_output_and_exception_are_not_logged(self):
        for result in (completed(1, 'private detail', 'private detail'),
                       OSError('private detail')):
            self.assertNotIn('private detail', self.run_install([result], [False])[4])

    def test_transient_classification(self):
        for message in ('HTTP 429', 'HTTP/1.1 502', 'status code: 503', '(504)',
                        'The operation has timed out', 'connection reset'):
            with self.subTest(message=message):
                self.assertEqual(self.run_install([completed(1, message)] * 3,
                                                   [False] * 3)[:4], (1, 3, 3, 2))
        self.assertEqual(self.run_install([completed(1, 'ffmpeg version 503')],
                                           [False])[:4], (1, 1, 1, 0))


class ProbeTests(unittest.TestCase):
    def run_probe(self, paths, results):
        with patch.object(gate.shutil, 'which', side_effect=paths), \
                patch.object(gate.subprocess, 'run', side_effect=results) as run, \
                contextlib.redirect_stdout(io.StringIO()) as output:
            ready = gate.probe_tools()
        return ready, run, output.getvalue()

    def test_actual_version_commands_for_both_tools(self):
        ready, run, _ = self.run_probe(['ffmpeg.exe', 'ffprobe.exe'],
                                      [completed(stdout='ffmpeg version test'),
                                       completed(stdout='ffprobe version test')])
        self.assertTrue(ready)
        self.assertEqual([call.args[0] for call in run.call_args_list],
                         [['ffmpeg.exe', '-version'], ['ffprobe.exe', '-version']])

    def test_each_tool_is_required(self):
        for paths, results in (([None, 'ffprobe.exe'], [completed(stdout='ffprobe version test')]),
                               (['ffmpeg.exe', None], [completed(stdout='ffmpeg version test')])):
            self.assertFalse(self.run_probe(paths, results)[0])

    def test_broken_or_fake_shims_fail_and_other_tool_is_still_checked(self):
        for result in (completed(1), completed(), completed(stdout='wrong version'),
                       OSError('private detail'), subprocess.TimeoutExpired('tool', 20)):
            ready, run, output = self.run_probe(['ffmpeg.exe', 'ffprobe.exe'],
                                               [result, completed(stdout='ffprobe version test')])
            self.assertFalse(ready)
            self.assertEqual(run.call_count, 2)
            self.assertNotIn('private detail', output)


if __name__ == '__main__':
    unittest.main()
