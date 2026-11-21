#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Contract tests for the external-oracle qualification harness.

No FFmpeg and no SDK are needed: these pin the fail-closed decisions that the
harness makes before any external decoder runs.
"""
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
            with mock.patch.dict(harness.QUALIFIED_EXECUTABLE_SHA256, actual,
                                 clear=True):
                self.assertEqual(
                    harness.require_measured_executables(
                        DEBIAN_TOKEN, str(ffmpeg), str(ffprobe)),
                    actual['7.1.5-0+deb13u1'])
            for name in ('ffmpeg', 'ffprobe'):
                wrong = dict(actual['7.1.5-0+deb13u1'])
                wrong[name] = '0' * 64
                with mock.patch.dict(harness.QUALIFIED_EXECUTABLE_SHA256,
                                     {DEBIAN_TOKEN: wrong}, clear=True):
                    with self.assertRaises(harness.Blocked) as caught:
                        harness.require_measured_executables(
                            DEBIAN_TOKEN, str(ffmpeg), str(ffprobe))
                    self.assertIn(name, str(caught.exception))
            with mock.patch.dict(harness.QUALIFIED_EXECUTABLE_SHA256, actual,
                                 clear=True):
                self.assertEqual(
                    harness.require_measured_executables(
                        DEBIAN_TOKEN, str(ffmpeg), str(ffprobe)),
                    actual['7.1.5-0+deb13u1'])

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


if __name__ == '__main__':
    unittest.main()
