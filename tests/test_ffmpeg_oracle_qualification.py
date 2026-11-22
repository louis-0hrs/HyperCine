#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Contract tests for the external-oracle qualification harness.

No FFmpeg and no SDK are needed: these pin the fail-closed decisions that the
harness makes before any external decoder runs.
"""
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).parent))
import qualify_ffmpeg_oracle as harness

FIXTURES = Path(__file__).parent / 'fixtures' / 'standalone'
HARNESS = Path(__file__).parent / 'qualify_ffmpeg_oracle.py'
DEBIAN_TOKEN = '7.1.5-0+deb13u1'
DEBIAN_BANNER = 'ffmpeg version ' + DEBIAN_TOKEN


def banner_shim(directory, name, banner=DEBIAN_BANNER):
    """A private executable that reports a banner and never decodes."""
    path = Path(directory) / name
    path.write_text('#!/bin/sh\nprintf "%s\\n" "{}"\n'.format(banner))
    path.chmod(0o755)
    return path


class QualificationContractTest(unittest.TestCase):
    def test_measured_builds_are_accepted(self):
        for token in ('n8.0-140fd653aed8', '9.0.2-essentials_build-www.gyan.dev',
                      '7.1.5-0+deb13u1'):
            self.assertEqual(harness.require_qualified_build(token), token)

    def test_unmeasured_builds_are_blocked(self):
        # 7.1.50 and 7.1.5.1 are different builds from 7.1.5, exactly like
        # 9.0.20 is different from 9.0.2; the suffix rule is not a prefix rule.
        for token in ('9.1.0', 'n7.1', '7.0.2', '9.0.20', 'n8.0.1',
                      '7.1.5', '7.1.5-unmeasured', '7.1.5-0+deb13u2',
                      '7.1.5-0+deb13u1-fork', '7.1.50', '7.1.5.1'):
            with self.assertRaises(harness.Blocked):
                harness.require_qualified_build(token)

    def test_unrecognized_banner_is_blocked_not_failed(self):
        # Contract row 77: "build not on the measured list", not a failed
        # control. A wrapper or a non-FFmpeg executable lands here.
        for banner in ('definitely not ffmpeg', '', 'ffmpeg version',
                       'ffmpeg 9.0.2-essentials_build-www.gyan.dev'):
            with self.assertRaises(harness.Blocked):
                harness.parse_banner(banner)
        self.assertEqual(
            harness.parse_banner(
                'ffmpeg version 9.0.2-essentials_build-www.gyan.dev\n'
                'built with gcc 13.2.0\n'),
            '9.0.2-essentials_build-www.gyan.dev')

    def test_unrunnable_candidate_is_blocked(self):
        # A real executable that is not FFmpeg at all: this interpreter
        # rejects `-version`, so the run itself fails and the harness blocks.
        with self.assertRaises(harness.Blocked):
            harness.build_token(sys.executable)

    @unittest.skipUnless(os.name == 'posix', 'needs a POSIX executable shim')
    def test_non_ffmpeg_executable_is_blocked(self):
        # End-to-end over a real subprocess: exit 0 with an unparseable
        # banner must skip, not fail the lane.
        with tempfile.TemporaryDirectory(prefix='hypercine-oracle-banner-') as temporary:
            shim = Path(temporary) / 'not-ffmpeg'
            shim.write_text('#!/bin/sh\nprintf "%s\\n" "definitely not ffmpeg"\n')
            shim.chmod(0o755)
            with self.assertRaises(harness.Blocked):
                harness.build_token(str(shim))

    def test_adapter_rejects_out_of_contract_output(self):
        harness.validate_output(bytes(harness.OUTPUT_BYTES))
        with self.assertRaises(harness.Failed):
            harness.validate_output(bytes(harness.OUTPUT_BYTES - 1))
        samples = [0] * (harness.OUTPUT_BYTES // 2)
        samples[0] = harness.PEAK + 1
        with self.assertRaises(harness.Failed):
            harness.validate_output(struct.pack('<{}H'.format(len(samples)), *samples))

    def test_self_check_passes(self):
        harness.self_check_rejects_corrupt_output()

    def test_missing_tool_is_blocked(self):
        with self.assertRaises(harness.Blocked):
            harness.find_tool(str(FIXTURES / 'does-not-exist'), 'ffmpeg')
        with self.assertRaises(harness.Blocked):
            harness.find_tool(None, 'hypercine-no-such-tool')

    def test_bound_fixture_is_accepted(self):
        declared = harness.load_manifest(FIXTURES)
        packet = harness.load_bound(FIXTURES, declared, 'flat.cfhd')
        self.assertEqual(harness.sha256(packet), declared['flat.cfhd']['sha256'])

    def test_tampered_fixture_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix='hypercine-oracle-contract-') as temporary:
            copy = Path(temporary) / 'fixtures'
            shutil.copytree(FIXTURES, copy)
            declared = harness.load_manifest(copy)
            path = copy / 'canonical.p10'
            data = bytearray(path.read_bytes())
            data[0] ^= 1
            path.write_bytes(data)
            with self.assertRaises(harness.Failed):
                harness.load_bound(copy, declared, 'canonical.p10')
            (copy / 'flat.cfhd').unlink()
            with self.assertRaises(harness.Failed):
                harness.load_bound(copy, declared, 'flat.cfhd')


class MeasuredExecutableIdentityTest(unittest.TestCase):
    """Token admission and measured executable identity are separate gates."""

    def test_only_the_debian_entry_binds_executable_bytes(self):
        self.assertEqual(set(harness.QUALIFIED_EXECUTABLE_SHA256),
                         {DEBIAN_TOKEN})
        for digests in harness.QUALIFIED_EXECUTABLE_SHA256.values():
            self.assertEqual(set(digests), {'ffmpeg', 'ffprobe'})
            for digest in digests.values():
                self.assertRegex(digest, r'^[0-9a-f]{64}$')

    def test_legacy_prefix_entries_stay_token_only(self):
        # n8.0/9.0.2 documented a banner, not a binary digest; they must not
        # silently acquire a hash requirement they were never measured against.
        for token in ('n8.0-140fd653aed8',
                      '9.0.2-essentials_build-www.gyan.dev'):
            self.assertIsNone(harness.require_measured_executables(
                token, '/nonexistent/ffmpeg', '/nonexistent/ffprobe'))

    def test_unreadable_bound_binary_is_blocked(self):
        with self.assertRaises(harness.Blocked):
            harness.require_measured_executables(
                DEBIAN_TOKEN, str(FIXTURES / 'no-such-ffmpeg'),
                str(FIXTURES / 'no-such-ffprobe'))
        with tempfile.TemporaryDirectory(prefix='hypercine-oracle-dir-') as temporary:
            with self.assertRaises(harness.Blocked):
                harness.require_measured_executables(
                    DEBIAN_TOKEN, temporary, temporary)

    @unittest.skipUnless(os.name == 'posix', 'needs POSIX executable shims')
    def test_same_banner_different_bytes_blocks_each_tool(self):
        # Same banner, different bytes. The recorded digests are bound to the
        # fake files for the GREEN baseline, so the *only* difference in the
        # RED arms is one tool's bytes; no decode is ever executed.
        with tempfile.TemporaryDirectory(prefix='hypercine-oracle-identity-') as temporary:
            ffmpeg = banner_shim(temporary, 'ffmpeg')
            ffprobe = banner_shim(temporary, 'ffprobe')
            actual = {
                '7.1.5-0+deb13u1': {
                    'ffmpeg': harness.file_sha256(str(ffmpeg), 'ffmpeg'),
                    'ffprobe': harness.file_sha256(str(ffprobe), 'ffprobe'),
                }
            }
            tools = {'ffmpeg': ffmpeg, 'ffprobe': ffprobe}
            original = {name: path.read_bytes() for name, path in tools.items()}

            def banner(path):
                result = subprocess.run(
                    [str(path), '-version'], capture_output=True,
                    check=True, timeout=5)
                return result.stdout, result.stderr

            banners = {name: banner(path) for name, path in tools.items()}
            expected = dict(actual[DEBIAN_TOKEN])
            # One fixed mapping covers baseline, both byte mutations and each
            # restoration. Never change the expected record to manufacture RED.
            with mock.patch.dict(harness.QUALIFIED_EXECUTABLE_SHA256, actual,
                                 clear=True):
                self.assertEqual(
                    harness.require_measured_executables(
                        DEBIAN_TOKEN, str(ffmpeg), str(ffprobe)),
                    expected)
                for name, path in tools.items():
                    with self.subTest(tool=name):
                        peer = 'ffprobe' if name == 'ffmpeg' else 'ffmpeg'
                        try:
                            path.write_bytes(original[name] +
                                             b'\n# identity-only byte mutation\n')
                            self.assertNotEqual(
                                harness.file_sha256(str(path), name),
                                expected[name])
                            self.assertEqual(banner(path), banners[name])
                            self.assertEqual(tools[peer].read_bytes(), original[peer])
                            self.assertEqual(banner(tools[peer]), banners[peer])
                            self.assertEqual(
                                harness.QUALIFIED_EXECUTABLE_SHA256[DEBIAN_TOKEN],
                                expected)
                            with self.assertRaises(harness.Blocked) as caught:
                                harness.require_measured_executables(
                                    DEBIAN_TOKEN, str(ffmpeg), str(ffprobe))
                            self.assertIn(name, str(caught.exception))
                        finally:
                            path.write_bytes(original[name])
                        self.assertEqual(path.read_bytes(), original[name])
                        self.assertEqual(banner(path), banners[name])
                        self.assertEqual(
                            harness.require_measured_executables(
                                DEBIAN_TOKEN, str(ffmpeg), str(ffprobe)),
                            expected)
                        self.assertEqual(
                            harness.QUALIFIED_EXECUTABLE_SHA256[DEBIAN_TOKEN],
                            expected)

    @unittest.skipUnless(os.name == 'posix', 'needs POSIX executable shims')
    def test_cli_exits_77_before_qualification_on_identity_mismatch(self):
        # End-to-end through main() in normal and optimized Python: the fake
        # tools carry the measured banner but not the measured bytes, so the
        # run must stop at 77 with no QUALIFIED line and no fixture work.
        with tempfile.TemporaryDirectory(prefix='hypercine-oracle-cli-') as temporary:
            ffmpeg = banner_shim(temporary, 'ffmpeg')
            ffprobe = banner_shim(temporary, 'ffprobe')
            for flags in (['-B'], ['-B', '-O']):
                completed = subprocess.run(
                    [sys.executable] + flags + [str(HARNESS),
                     '--ffmpeg', str(ffmpeg), '--ffprobe', str(ffprobe),
                     '--fixtures', str(Path(temporary) / 'no-fixtures')],
                    capture_output=True, text=True, check=False)
                self.assertEqual(completed.returncode, harness.EXIT_BLOCKED,
                                 completed.stdout + completed.stderr)
                output = completed.stdout + completed.stderr
                self.assertIn('ORACLE_QUALIFICATION_BLOCKED', output)
                self.assertIn('sha256', output)
                self.assertNotIn('state=QUALIFIED', output)

    def test_measured_debian_binaries_on_this_host_are_accepted(self):
        # The real measured host: accepted bytes must pass, and a host whose
        # binaries are not the measured ones is a skip, never a fake pass.
        try:
            ffmpeg = harness.find_tool(None, 'ffmpeg')
            ffprobe = harness.find_tool(None, 'ffprobe')
            observed = harness.require_measured_executables(
                DEBIAN_TOKEN, ffmpeg, ffprobe)
        except harness.Blocked as error:
            self.skipTest(str(error))
        self.assertEqual(observed,
                         harness.QUALIFIED_EXECUTABLE_SHA256[DEBIAN_TOKEN])




class BoundedExternalToolTest(unittest.TestCase):
    """The external oracle runner must never use unbounded capture or raw output."""

    def test_budgeted_success_paths_and_exact_decode_bytes(self):
        stream = {'streams': [{'codec_name': 'cfhd', 'width': 128,
                               'height': 128, 'pix_fmt': 'yuv422p10le'}]}
        calls = []
        def simulated(argv, **kwargs):
            calls.append((list(argv), kwargs))
            if '-version' in argv:
                return b'ffmpeg version 9.0.2-essentials_build-www.gyan.dev\n'
            if '-show_streams' in argv:
                return json.dumps(stream).encode('utf-8')
            return bytes(harness.OUTPUT_BYTES)

        with tempfile.TemporaryDirectory(prefix='hypercine-oracle-bounds-') as tmp:
            with mock.patch.object(harness.compat_process, 'run_bounded',
                                   side_effect=simulated):
                self.assertEqual(harness.build_token('candidate'),
                                 '9.0.2-essentials_build-www.gyan.dev')
                self.assertEqual(harness.decode(
                    'candidate', 'probe', b'test-packet', Path(tmp), 'bounded'),
                    bytes(harness.OUTPUT_BYTES))
        self.assertEqual(len(calls), 3)
        self.assertEqual([c[1]['timeout'] for c in calls],
                         [30.0, 60.0, 120.0])
        self.assertEqual([c[1]['stdout_limit'] for c in calls],
                         [65536, 65536, 65536])
        self.assertEqual(calls[-1][0][-1], 'pipe:1')
        self.assertNotIn('-y', calls[-1][0])
        self.assertFalse(any('.yuv' in a for a in calls[-1][0]))

    def test_under_exact_over_length_fail_closed(self):
        with tempfile.TemporaryDirectory(prefix='hypercine-oracle-length-') as tmp:
            with mock.patch.object(harness, 'probe_stream'):
                for size in (65535, 65537):
                    with self.subTest(size=size):
                        with mock.patch.object(harness.compat_process,
                                               'run_bounded',
                                               return_value=bytes(size)):
                            with self.assertRaises(harness.Failed):
                                harness.decode('candidate', 'probe', b'p',
                                               Path(tmp), 'bad-length')
                with mock.patch.object(harness.compat_process, 'run_bounded',
                                       return_value=bytes(65536)):
                    self.assertEqual(
                        len(harness.decode('candidate', 'probe', b'p',
                                           Path(tmp), 'exact-length')), 65536)

    def test_public_reasons_never_reemit_external_bytes(self):
        marker = 'EXTERNAL_SENTINEL_731'
        for banner in (marker, 'ffmpeg version ' + marker,
                       'ffmpeg version 9.0.2\n' + marker):
            if banner.startswith('ffmpeg version 9.0.2\n'):
                self.assertEqual(harness.parse_banner(banner), '9.0.2')
            else:
                with self.assertRaises(harness.Blocked) as caught:
                    harness.require_qualified_build(harness.parse_banner(banner))
                self.assertNotIn(marker, str(caught.exception))
        with tempfile.TemporaryDirectory(prefix='hypercine-oracle-privacy-') as tmp:
            with mock.patch.object(harness.compat_process, 'run_bounded',
                                   return_value=marker.encode('utf-8')):
                with self.assertRaises(harness.Failed) as caught:
                    harness.probe_stream('probe', Path(tmp) / 'input.avi')
                self.assertNotIn(marker, str(caught.exception))
            with mock.patch.object(harness, 'probe_stream'):
                with mock.patch.object(harness.compat_process, 'run_bounded',
                                       side_effect=harness.compat_process.ToolFailure(
                                           'tool nonzero exit')):
                    with self.assertRaises(harness.Failed) as caught:
                        harness.decode('ffmpeg', 'ffprobe', b'p', Path(tmp), 'failed')
                    self.assertNotIn(marker, str(caught.exception))

    def test_actual_supervisor_output_and_timeout_are_finite(self):
        def script(source, timeout=5):
            return harness.run_tool([sys.executable, '-c', source],
                                    timeout, 65536)
        self.assertEqual(len(script(
            'import sys; sys.stdout.buffer.write(b"x"*65536)')), 65536)
        for source, timeout in (
                ('import sys; sys.stdout.buffer.write(b"x"*65537)', 5),
                ('import time; time.sleep(10)', 0.35)):
            with self.subTest(source=source):
                with self.assertRaises(harness.Failed):
                    script(source, timeout)

    @unittest.skipUnless(os.name == 'posix', 'needs POSIX shell shims')
    def test_real_external_stderr_and_banner_never_leak(self):
        marker = 'EXTERNAL_SENTINEL_731'
        with tempfile.TemporaryDirectory(prefix='hypercine-oracle-sentinel-') as tmp:
            shim = Path(tmp) / 'tool'
            shim.write_text('#!/bin/sh\n'
                            'if [ "$1" = "-version" ]; then\n'
                            '  printf "%s\\n" "' + marker + '"\n'
                            '  exit 0\n'
                            'fi\n'
                            'printf "%s\\n" "' + marker + '" >&2\n'
                            'exit 3\n')
            shim.chmod(0o755)
            with self.assertRaises(harness.Blocked) as caught:
                harness.build_token(str(shim))
            self.assertNotIn(marker, str(caught.exception))
            with self.assertRaises(harness.Failed) as caught:
                harness.probe_stream(str(shim), Path(tmp) / 'probe.avi')
            self.assertNotIn(marker, str(caught.exception))
            with mock.patch.object(harness, 'probe_stream'):
                with self.assertRaises(harness.Failed) as caught:
                    harness.decode(str(shim), str(shim), b'p', Path(tmp), 'decode')
                self.assertNotIn(marker, str(caught.exception))


    @unittest.skipUnless(os.name == 'posix', 'needs POSIX CLI executable shims')
    def test_actual_cli_redacts_external_banner_probe_and_decode_stderr(self):
        marker = 'EXTERNAL_SENTINEL_731'
        with tempfile.TemporaryDirectory(prefix='hypercine-oracle-cli-privacy-') as tmp:
            def shim(name, content):
                path = Path(tmp) / name
                path.write_text('#!/bin/sh\n' + content)
                path.chmod(0o755)
                return path

            ffmpeg_bad = shim('bad-banner',
                              'printf "%s\\n" "' + marker + '"\n')
            ffmpeg = shim(
                'version-and-fail',
                'if [ "$1" = "-version" ]; then\n'
                '  printf "%s\\n" "ffmpeg version 9.0.2-privacy-test"\n'
                '  exit 0\n'
                'fi\n'
                'printf "%s\\n" "' + marker + '" >&2\n'
                'exit 1\n')
            probe_bad = shim('probe-fail',
                             'printf "%s\\n" "' + marker + '" >&2\nexit 1\n')
            probe_good = shim(
                'probe-good',
                'printf "%s\\n" '
                '\'{"streams":[{"codec_name":"cfhd","width":128,'
                '"height":128,"pix_fmt":"yuv422p10le"}]}\'\n')
            cases = (
                ('banner', ffmpeg_bad, probe_good, 77),
                ('probe', ffmpeg, probe_bad, 1),
                ('decode', ffmpeg, probe_good, 1),
            )
            for flags in (['-B'], ['-B', '-O']):
                for label, exe, probe, expected_exit in cases:
                    with self.subTest(flags=flags, case=label):
                        proc = subprocess.run(
                            [sys.executable] + flags + [str(HARNESS),
                             '--ffmpeg', str(exe), '--ffprobe', str(probe),
                             '--repeats', '2'],
                            capture_output=True, timeout=30, text=True)
                        public = proc.stdout + proc.stderr
                        self.assertEqual(proc.returncode, expected_exit, public)
                        self.assertIn('ORACLE_QUALIFICATION_', public)
                        self.assertNotIn(marker, public)
                        self.assertNotIn('state=QUALIFIED', public)


if __name__ == '__main__':
    unittest.main()
