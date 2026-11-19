# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Fast controls for reuse-only setup and non-authoritative timing receipts."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
if __package__:
    from . import ci_performance as perf
else:
    import ci_performance as perf


class CIPerformanceTests(unittest.TestCase):
    def setUp(self):
        self.quiet = contextlib.redirect_stdout(io.StringIO())
        self.quiet.__enter__()
        self.addCleanup(self.quiet.__exit__, None, None, None)

    def test_available_tools_need_no_package_manager(self):
        with patch.object(perf.platform, 'system', return_value='Linux'), \
             patch.object(perf.shutil, 'which', side_effect=lambda name: name), \
             patch.object(perf.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0)) as run:
            report = perf.prepare_linux()
        self.assertEqual(report['setup'], 'reused_image_tools')
        self.assertEqual(run.call_count, 3)
        self.assertTrue(all(call.args[0][0] != 'sudo' for call in run.call_args_list))
        for call in run.call_args_list:
            self.assertEqual(call.kwargs['timeout'], 15)
            self.assertEqual(call.kwargs['stdout'], subprocess.DEVNULL)
            self.assertEqual(call.kwargs['stderr'], subprocess.DEVNULL)

    def test_ffmpeg_and_ffprobe_share_one_package(self):
        with patch.object(perf.shutil, 'which', return_value=None):
            self.assertEqual(perf.missing_packages(), ['ffmpeg', 'ninja-build'])

    def test_missing_ffprobe_is_not_hidden_by_ffmpeg(self):
        with patch.object(perf.shutil, 'which', side_effect=lambda n: None if n == 'ffprobe' else n), \
             patch.object(perf.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0)):
            self.assertEqual(perf.missing_packages(), ['ffmpeg'])

    def test_broken_existing_tool_is_not_reused(self):
        for effect in (subprocess.CompletedProcess([], 1), OSError('private-marker'),
                       subprocess.TimeoutExpired('private-marker', 15)):
            with self.subTest(effect=type(effect).__name__), \
                 patch.object(perf.shutil, 'which', return_value='ffmpeg'), \
                 patch.object(perf.subprocess, 'run') as run:
                if isinstance(effect, BaseException):
                    run.side_effect = effect
                else:
                    run.return_value = effect
                with self.assertRaisesRegex(RuntimeError, '^installed tool probe failed: ffmpeg$'):
                    perf.missing_packages()

    def test_missing_tools_cannot_install_without_hosted_optin(self):
        for install, actions, runner in ((False, 'true', 'github-hosted'),
                (True, 'false', 'github-hosted'), (True, 'true', 'self-hosted'),
                (True, None, None)):
            env = {k: v for k, v in {'GITHUB_ACTIONS': actions, 'HC_RUNNER_ENVIRONMENT': runner}.items() if v}
            with self.subTest(install=install, runner=runner), \
                 patch.object(perf.platform, 'system', return_value='Linux'), \
                 patch.object(perf, 'missing_packages', return_value=['ffmpeg']), \
                 patch.dict(perf.os.environ, env, clear=True), \
                 patch.object(perf.subprocess, 'run') as run, self.assertRaises(RuntimeError):
                perf.prepare_linux(install)
            run.assert_not_called()

    def test_non_linux_setup_is_refused(self):
        with patch.object(perf.platform, 'system', return_value='Windows'), self.assertRaises(RuntimeError):
            perf.prepare_linux()

    def test_install_only_missing_then_reprobe(self):
        env = {'GITHUB_ACTIONS': 'true', 'HC_RUNNER_ENVIRONMENT': 'github-hosted'}
        with patch.object(perf.platform, 'system', return_value='Linux'), \
             patch.dict(perf.os.environ, env, clear=True), \
             patch.object(perf, 'missing_packages', side_effect=[['ninja-build'], []]) as probe, \
             patch.object(perf.subprocess, 'run') as run:
            report = perf.prepare_linux(True)
        self.assertEqual(probe.call_count, 2)
        self.assertEqual(run.call_args_list[0].args[0], ['sudo', '-n', 'apt-get', 'update'])
        self.assertEqual(run.call_args_list[1].args[0],
                         ['sudo', '-n', 'apt-get', 'install', '--no-install-recommends', '-y', 'ninja-build'])
        self.assertEqual(report['installed_packages'], ['ninja-build'])
        self.assertTrue(all(call.kwargs['check'] for call in run.call_args_list))
        self.assertEqual([call.kwargs['timeout'] for call in run.call_args_list], [600, 1200])

    def test_slow_install_exceeding_extended_bound_is_still_failure(self):
        env = {'GITHUB_ACTIONS': 'true', 'HC_RUNNER_ENVIRONMENT': 'github-hosted'}
        with patch.object(perf.platform, 'system', return_value='Linux'), \
             patch.dict(perf.os.environ, env, clear=True), \
             patch.object(perf, 'missing_packages', return_value=['ffmpeg']), \
             patch.object(perf.subprocess, 'run', side_effect=[
                 subprocess.CompletedProcess([], 0),
                 subprocess.TimeoutExpired('apt-get install', 1200)]) as run, \
             self.assertRaises(subprocess.TimeoutExpired):
            perf.prepare_linux(True)
        self.assertEqual(run.call_count, 2)
        self.assertEqual(run.call_args_list[1].kwargs['timeout'], 1200)

    def test_failed_install_never_reports_setup_success(self):
        env = {'GITHUB_ACTIONS': 'true', 'HC_RUNNER_ENVIRONMENT': 'github-hosted'}
        with patch.object(perf.platform, 'system', return_value='Linux'), \
             patch.dict(perf.os.environ, env, clear=True), \
             patch.object(perf, 'missing_packages', return_value=['ffmpeg']), \
             patch.object(perf.subprocess, 'run', side_effect=subprocess.CalledProcessError(100, 'apt')), \
             self.assertRaises(subprocess.CalledProcessError):
            perf.prepare_linux(True)

    def test_missing_after_install_is_failure(self):
        env = {'GITHUB_ACTIONS': 'true', 'HC_RUNNER_ENVIRONMENT': 'github-hosted'}
        with patch.object(perf.platform, 'system', return_value='Linux'), \
             patch.dict(perf.os.environ, env, clear=True), \
             patch.object(perf, 'missing_packages', return_value=['ffmpeg']), \
             patch.object(perf.subprocess, 'run'), self.assertRaises(RuntimeError):
            perf.prepare_linux(True)

    def xml(self, body):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        path = Path(temp.name) / 'timing.xml'
        path.write_text(body, encoding='utf-8')
        return path

    def test_timing_totals_ranking_and_skip_are_explicit(self):
        path = self.xml('<testsuite><testcase name="fast" time="0.25" status="run"/>'
                        '<testcase name="slow" time="2.5" status="run"><failure/></testcase>'
                        '<testcase name="blocked" time="0" status="notrun"><skipped/></testcase></testsuite>')
        report = perf.junit_timings(path)
        self.assertEqual((report['test_count'], report['skipped_count'], report['failed_count']), (3, 1, 1))
        self.assertEqual(report['summed_test_seconds'], 2.75)
        self.assertEqual(report['slowest_tests'][0]['name'], 'slow')
        self.assertNotIn('PASS', json.dumps(report))

    def test_invalid_timing_is_rejected(self):
        for value in ('-1', 'nan', 'inf', '-inf', '', 'text'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                perf.junit_timings(self.xml('<testsuite><testcase name="x" time="'+value+'"/></testsuite>'))

    def test_empty_duplicate_and_missing_timing_rejected(self):
        for body in ('<testsuite/>', '<testsuite><testcase name="x"/></testsuite>',
                '<testsuite><testcase name="x" time="1"/><testcase name="x" time="1"/></testsuite>'):
            with self.subTest(body=body), self.assertRaises((ValueError, KeyError)):
                perf.junit_timings(self.xml(body))

    def test_xml_limits_and_entities_rejected(self):
        for body in ('<!DOCTYPE testsuite><testsuite/>', '<!ENTITY x "x"><testsuite/>'):
            with self.assertRaises(ValueError):
                perf.junit_timings(self.xml(body))
        with patch.object(perf, 'MAX_JUNIT_BYTES', 8), self.assertRaises(ValueError):
            perf.junit_timings(self.xml('<testsuite/>'))

    def test_arbitrary_test_name_not_published(self):
        result = perf.junit_timings(self.xml('<testsuite><testcase name="private/marker" time="1"/></testsuite>'))
        self.assertNotIn('private', json.dumps(result))
        self.assertTrue(result['slowest_tests'][0]['name'].startswith('redacted-test-'))

    def test_measure_preserves_real_child_failure_and_removes_stale_xml(self):
        with tempfile.TemporaryDirectory() as temp:
            output, junit = Path(temp) / 'report.json', Path(temp) / 'junit.xml'
            output.write_text('stale PASS')
            junit.write_text('stale PASS')
            with patch.object(perf, 'checkout_sha', return_value='a'*40):
                code = perf.measure([sys.executable, '-c', 'raise SystemExit(7)'], output, 'standalone-tests', junit)
            self.assertEqual(code, 7)
            self.assertFalse(junit.exists())
            report = json.loads(output.read_text())
            self.assertEqual(report['command_exit_code'], 7)
            self.assertNotEqual(report['status'], 'MEASURED')
            self.assertNotIn('PASS', output.read_text())

    def test_real_successful_child_timing(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / 'report.json'
            with patch.object(perf, 'checkout_sha', return_value='a'*40):
                code = perf.measure([sys.executable, '-c', 'pass'], output, 'standalone-build')
            report = json.loads(output.read_text())
            self.assertEqual(code, 0)
            self.assertEqual(report['status'], 'MEASURED')
            self.assertGreater(report['wall_seconds'], 0)
            self.assertEqual(report['checkout_sha'], 'a'*40)

    def test_success_without_expected_junit_is_not_success(self):
        with tempfile.TemporaryDirectory() as temp, patch.object(perf, 'checkout_sha', return_value='a'*40):
            self.assertEqual(perf.measure([sys.executable, '-c', 'pass'], Path(temp)/'out.json',
                                        'standalone-tests', Path(temp)/'missing.xml'), 2)

    def test_real_child_writes_valid_junit(self):
        with tempfile.TemporaryDirectory() as temp, patch.object(perf, 'checkout_sha', return_value='a'*40):
            output, junit = Path(temp)/'report.json', Path(temp)/'result.xml'
            script = "from pathlib import Path; Path(" + repr(str(junit)) + ").write_text('<testsuite><testcase name=\"x\" time=\"1.25\" status=\"run\"/></testsuite>')"
            self.assertEqual(perf.measure([sys.executable, '-c', script], output, 'standalone-tests', junit), 0)
            self.assertEqual(json.loads(output.read_text())['summed_test_seconds'], 1.25)

    def test_successful_child_with_malformed_junit_fails_measurement(self):
        with tempfile.TemporaryDirectory() as temp, patch.object(perf, 'checkout_sha', return_value='a'*40):
            output, junit = Path(temp)/'report.json', Path(temp)/'result.xml'
            script = "from pathlib import Path; Path(" + repr(str(junit)) + ").write_text('not XML')"
            self.assertEqual(perf.measure([sys.executable, '-c', script], output, 'standalone-tests', junit), 2)
            report = json.loads(output.read_text())
            self.assertEqual(report['command_exit_code'], 0)
            self.assertEqual(report['measurement_exit_code'], 2)

    def test_private_or_invalid_job_value_not_published(self):
        with tempfile.TemporaryDirectory() as temp, patch.dict(perf.os.environ, {'HYPERCINE_STANDALONE_CTEST_JOBS': 'private-marker'}):
            output = Path(temp)/'report.json'
            output.write_text('stale PASS')
            with self.assertRaises(ValueError):
                perf.measure([sys.executable, '-c', 'pass'], output, 'standalone-build')
            self.assertFalse(output.exists())

    def test_identical_report_paths_refused(self):
        with tempfile.TemporaryDirectory() as temp, self.assertRaises(ValueError):
            output = Path(temp) / 'same.json'
            perf.measure([sys.executable, '-c', 'pass'], output, 'standalone-tests', output)

    def test_negative_exit_is_failure(self):
        with tempfile.TemporaryDirectory() as temp, patch.object(perf, 'checkout_sha', return_value='a'*40), \
             patch.object(perf.subprocess, 'run', return_value=subprocess.CompletedProcess([], -9)):
            self.assertEqual(perf.measure(['synthetic'], Path(temp)/'out.json', 'standalone-build'), 137)

    def test_bad_checkout_identity_refused(self):
        with patch.object(perf.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, 'not-a-sha')), \
             self.assertRaises(ValueError):
            perf.checkout_sha()

    def test_workflow_keeps_full_tests_and_mac_lane(self):
        root = Path(__file__).resolve().parents[1]
        workflow = (root / '.github/workflows/ci-standalone.yml').read_text()
        self.assertIn('python ci/test_ci_performance.py', workflow)
        self.assertIn('python ci/run_standalone.py --system "$EXPECTED_SYSTEM" --machine "$EXPECTED_MACHINE"', workflow)
        self.assertIn("HYPERCINE_STANDALONE_CTEST_JOBS: '2'", workflow)
        self.assertIn('mac_candidate_sha == github.sha', workflow)
        self.assertIn('ASAN_OPTIONS: detect_leaks=0:halt_on_error=1', workflow)
        self.assertIn('UBSAN_OPTIONS: halt_on_error=1:print_stacktrace=1', workflow)
        self.assertIn("if: runner.os != 'macOS'", workflow)
        self.assertNotIn('continue-on-error', workflow)
        cpu = (root / '.github/workflows/ci-cpu.yml').read_text()
        self.assertIn('cmake --build build-cpu --config Release --parallel 3', cpu)
        self.assertIn('ctest --test-dir build-cpu -C Release --output-on-failure', cpu)
        self.assertNotIn('ctest --test-dir build-cpu -C Release --parallel', cpu)


if __name__ == '__main__':
    unittest.main()
