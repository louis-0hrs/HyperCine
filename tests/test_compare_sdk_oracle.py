#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.

"""Regression coverage for the Windows SDK-oracle log replay filter."""
import base64
import hashlib
import importlib.util
import json
import subprocess
import struct
import sys
import tempfile
from unittest.mock import patch
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "compare_sdk_oracle", Path(__file__).resolve().parent / "compare_sdk_oracle.py")
compare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare)


class ObservationFilter(unittest.TestCase):
    def test_retired_pass_marker_is_not_required(self):
        self.assertFalse(compare.is_observation_line(
            "SDK decode PASS: all three standalone packets"))

    def test_summary_line_is_recorded(self):
        self.assertTrue(compare.is_observation_line(
            "standalone_sdk_decode_3_packets quality_measured=1 "
            "quality_meets_threshold=1 distinctOutputsMax=1 "
            "BLOCKED_ORACLE_NOT_QUALIFIED exit=77"))

    def test_per_pattern_lines_are_recorded(self):
        self.assertTrue(compare.is_observation_line(
            "flat distinctYU64=1 distinctBGRA=1"))
        self.assertTrue(compare.is_observation_line(
            "invalid zero header PrepareToDecode status=1"))

    def test_strict_failure_lines_are_recorded(self):
        self.assertTrue(compare.is_observation_line(
            "gradient STRICT_FAILURE Cannot open fixture"))
        self.assertTrue(compare.is_observation_line(
            "Oracle harness error: boom"))

    def test_unrelated_lines_are_ignored(self):
        self.assertFalse(compare.is_observation_line("100% tests passed"))


class OracleFailClosed(unittest.TestCase):
    def test_full_bounded_inventory(self):
        lines = []
        for name in sorted(compare.ORACLE_NAMES):
            raw = b'cfhd' if name == 'control.cfhd' else bytes(65536)
            lines.extend(['ORACLE_DATA_BEGIN %s %d' % (name, len(raw)),
                          'ORACLE_DATA ' + base64.b64encode(raw).decode('ascii'),
                          'ORACLE_DATA_END ' + name])
        data = compare.extract_log(lines)
        self.assertEqual(set(data), compare.ORACLE_NAMES)

    def test_unsafe_filename_and_incomplete_log_are_rejected(self):
        for name in ('../escape', '/tmp/escape', 'foreign.sdk-p10'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                compare.extract_log(['ORACLE_DATA_BEGIN %s 4' % name])
        with self.assertRaises(ValueError):
            compare.extract_log(['ORACLE_DATA_BEGIN control.cfhd 4',
                                 'ORACLE_DATA ZGF0YQ==', 'ORACLE_DATA_END control.cfhd'])

    def test_duplicate_and_oversize_refusals(self):
        with self.assertRaises(ValueError):
            compare.extract_log(['ORACLE_DATA_BEGIN control.cfhd 16777217'])
        with self.assertRaises(ValueError):
            compare.extract_log(['ORACLE_DATA_END control.cfhd'])

    def test_invalid_p10_identical_pixels_are_not_perfect_metrics(self):
        bad = struct.pack('<32768H', *([1024] * 32768))
        with self.assertRaises(ValueError):
            compare.p10_metrics(bad, bad)

    def test_main_never_writes_untrusted_log_name(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / 'oracle.log'
            log.write_text('ORACLE_DATA_BEGIN ../escape 4\n', encoding='utf-8')
            output = root / 'output'
            argv = ['compare_sdk_oracle.py', '--log', str(log), '--run-url',
                    'https://example.invalid/run', '--ffmpeg', 'unused',
                    '--ffprobe', 'unused', '--output', str(output)]
            with patch.object(sys, 'argv', argv), self.assertRaises(ValueError):
                compare.main()
            self.assertFalse((root / 'escape').exists())
            self.assertFalse(any(output.iterdir()))


    def test_all_output_destinations_checked_before_extraction(self):
        # A valid oracle inventory must not clobber an unrelated earlier comparison.
        lines = []
        for name in sorted(compare.ORACLE_NAMES):
            raw = b'cfhd' if name == 'control.cfhd' else bytes(65536)
            lines.extend(['ORACLE_DATA_BEGIN %s %d' % (name, len(raw)),
                          'ORACLE_DATA ' + base64.b64encode(raw).decode('ascii'),
                          'ORACLE_DATA_END ' + name])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / 'oracle.log'
            log.write_text(chr(10).join(lines) + chr(10), encoding='utf-8')
            output = root / 'output'
            output.mkdir()
            sentinel = output / 'comparison.json'
            sentinel.write_bytes(b'prior-independent-evidence')
            argv = ['compare_sdk_oracle.py', '--log', str(log), '--run-url',
                    'https://example.invalid/run', '--ffmpeg', 'must-not-run',
                    '--ffprobe', 'must-not-run', '--output', str(output)]
            with patch.object(sys, 'argv', argv), self.assertRaises(ValueError):
                compare.main()
            self.assertEqual(sentinel.read_bytes(), b'prior-independent-evidence')
            self.assertEqual(sorted(p.name for p in output.iterdir()), ['comparison.json'])

    def test_exclusive_evidence_write_does_not_clobber(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            occupied = root / 'comparison.json'
            occupied.write_bytes(b'previous-evidence')
            with self.assertRaises(FileExistsError):
                compare.write_exclusive(occupied, b'corrupt')
            self.assertEqual(occupied.read_bytes(), b'previous-evidence')
            outside = root / 'outside'
            outside.write_bytes(b'protected')
            alias = root / 'alias'
            try:
                alias.symlink_to(outside)
            except (OSError, NotImplementedError):
                return  # Windows without symlink privilege still exercises O_EXCL above.
            with self.assertRaises(OSError):
                compare.write_exclusive(alias, b'corrupt')
            self.assertEqual(outside.read_bytes(), b'protected')


    def test_windows_fixtures_cannot_be_mutated_by_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixtures = root / 'retained-sdk-input'
            fixtures.mkdir()
            for output in (fixtures, fixtures / 'accidental-report'):
                argv = ['compare_sdk_oracle.py', '--windows-fixtures', str(fixtures),
                        '--ffmpeg', 'must-not-run', '--ffprobe', 'must-not-run',
                        '--output', str(output)]
                with self.subTest(output=output), patch.object(sys, 'argv', argv):
                    with self.assertRaises(ValueError):
                        compare.main()
                self.assertFalse((fixtures / 'accidental-report').exists())


    def test_extreme_decimal_length_is_rejected_before_conversion(self):
        # Length bound is checked before int(), also on Python 3.9 without its
        # newer built-in maximum-digit conversion limit.
        with self.assertRaisesRegex(ValueError, 'invalid oracle length'):
            compare.extract_log(['ORACLE_DATA_BEGIN control.cfhd ' + '9' * 10000])

    def test_retained_fixture_manifest_rejects_bad_entries_before_binary_read(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixtures = root / 'retained'
            fixtures.mkdir()
            output = root / 'output'
            for label, first_name, bad_value in (
                ('unhashable', 'control.cfhd', []),
                ('oversized_packet', 'control.cfhd', 16 * 1024 * 1024 + 1),
                ('invalid_pixel_storage', 'flat.sdk-p10', 64),
            ):
                names = [first_name] + sorted(compare.ORACLE_NAMES - {first_name})
                records = [{'file': name, 'bytes': 65536, 'sha256': '0' * 64}
                           for name in names]
                records[0]['file' if label == 'unhashable' else 'bytes'] = bad_value
                manifest = {'files': records, 'run_url': 'https://example.invalid/run',
                            'log_sha256': '0' * 64}
                (fixtures / 'manifest.json').write_text(json.dumps(manifest))
                argv = ['compare_sdk_oracle.py', '--windows-fixtures', str(fixtures),
                        '--ffmpeg', 'must-not-run', '--ffprobe', 'must-not-run',
                        '--output', str(output)]
                with self.subTest(label=label), patch.object(sys, 'argv', argv), \
                     patch.object(Path, 'read_bytes', side_effect=AssertionError('unbounded read')):
                    with self.assertRaisesRegex(ValueError, 'invalid or duplicate fixture name'
                                               if label == 'unhashable' else
                                               'invalid fixture byte count'):
                        compare.main()
                self.assertFalse(any(output.iterdir()))

    def test_oversized_oracle_log_refuses_before_payload_read(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / 'huge.log'
            with log.open('wb') as target:
                target.truncate(64 * 1024 * 1024 + 1)
            output = root / 'output'
            argv = ['compare_sdk_oracle.py', '--log', str(log), '--run-url',
                    'https://example.invalid/run', '--ffmpeg', 'must-not-run',
                    '--ffprobe', 'must-not-run', '--output', str(output)]
            with patch.object(sys, 'argv', argv), \
                 patch.object(Path, 'read_bytes', side_effect=AssertionError('unbounded read')):
                with self.assertRaisesRegex(ValueError, 'oracle log exceeds size limit'):
                    compare.main()
            self.assertFalse(any(output.iterdir()))


class OraclePublicationPrivacy(unittest.TestCase):
    def test_public_run_url_requires_canonical_credential_free_github_link(self):
        good = 'https://github.com/louis-0hrs/HyperCine/actions/runs/38627689864'
        self.assertEqual(compare.validated_run_url(good), good)
        for bad in (None, 100, '', '/private/host/path',
                    'https://user:token@github.com/louis-0hrs/HyperCine/actions/runs/1',
                    good + '?access_token=dummy',
                    good + '#PRIVATE_DO_NOT_PUBLISH',
                    'https://internal.example/actions/runs/123',
                    'https://github.com/louis-0hrs/HyperCine/actions/runs/not-a-number'):
            with self.subTest(bad=type(bad).__name__), self.assertRaisesRegex(
                    ValueError, 'invalid public oracle run URL'):
                compare.validated_run_url(bad)

    def test_ffmpeg_version_receipt_never_copies_vendor_banner_or_paths(self):
        for line, expected in (
            ('ffmpeg version n8.0 Copyright /private/SDK_PATH PRIVATE_TOKEN',
             'ffmpeg version n8.0'),
            ('ffmpeg version 7.0.2-essentials_build built with INTERNAL_PATH',
             'ffmpeg version 7.0.2-essentials_build'),
            ('ffmpeg version N-118228-g1234abcd local:/sensitive',
             'ffmpeg version N-118228-g1234abcd'),
            ('ffmpeg version 7.1.5-0+deb13u1 Copyright /private/VENDOR_PATH',
             'ffmpeg version 7.1.5-0+deb13u1'),
        ):
            with self.subTest(line=line.split()[2]):
                self.assertEqual(compare.validated_ffmpeg_version(line), expected)
        for invalid in ('ffmpeg version PRIVATE_CREDENTIAL',
                        'ffmpeg version 8.0/PRIVATE_PATH',
                        'ffmpeg version 7.1.5-0+deb13u1/PRIVATE_PATH',
                        'external-version', ''):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                compare.validated_ffmpeg_version(invalid)

    def test_unrecognized_cli_arguments_are_not_echoed(self):
        cmd = [sys.executable, '-B', str(Path(compare.__file__)),
               '--log', 'unused', '--run-url',
               'https://github.com/louis-0hrs/HyperCine/actions/runs/1',
               '--ffmpeg', 'unused', '--ffprobe', 'unused',
               '--output', 'unused',
               '--unrecognized', 'PRIVATE_CREDENTIAL_DO_NOT_PUBLISH']
        proc = subprocess.run(cmd, capture_output=True, text=True)
        self.assertNotEqual(proc.returncode, 0)
        self.assertEqual(proc.stderr.strip(),
                         'FAIL: SDK oracle replay rejected or unavailable')
        self.assertNotIn('PRIVATE_CREDENTIAL_DO_NOT_PUBLISH',
                         proc.stdout + proc.stderr)
        self.assertNotIn('Traceback', proc.stderr)

    def test_observation_receipt_never_echoes_raw_log_content(self):
        line = ('2026-11-11T12:00Z status=1 '
                '/private/PRIVATE_SOURCE_PATH ACCESS_KEY_DO_NOT_PUBLISH')
        receipt = compare.observation_receipt(line)
        self.assertEqual(set(receipt), {'marker', 'line_sha256'})
        self.assertEqual(receipt['marker'], 'status=')
        self.assertEqual(receipt['line_sha256'],
                         hashlib.sha256(line.encode('utf-8')).hexdigest())
        self.assertNotIn('PRIVATE_SOURCE_PATH', json.dumps(receipt))
        self.assertNotIn('ACCESS_KEY_DO_NOT_PUBLISH', json.dumps(receipt))

    def test_cli_error_does_not_publish_private_path_or_traceback(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / 'PRIVATE_SDK_LOG_DO_NOT_PUBLISH.txt'
            log.write_text('Oracle harness error: /private/HOST_SECRET_PATH\n',
                           encoding='utf-8')
            cmd = [sys.executable, '-B', str(Path(compare.__file__)),
                   '--log', str(log), '--run-url', 'https://example.invalid/run',
                   '--ffmpeg', 'not-executed', '--ffprobe', 'not-executed',
                   '--output', str(root / 'output')]
            proc = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(proc.returncode, 1)
            self.assertEqual(proc.stderr.strip(),
                             'FAIL: SDK oracle replay rejected or unavailable')
            self.assertNotIn('Traceback', proc.stderr)
            self.assertNotIn('PRIVATE_SDK_LOG_DO_NOT_PUBLISH',
                             proc.stdout + proc.stderr)
            self.assertNotIn('HOST_SECRET_PATH', proc.stdout + proc.stderr)

    def test_excessive_observations_fail_before_receipts_written(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / 'oracle.log'
            log.write_text('status=1\n' * 4097, encoding='utf-8')
            output = root / 'output'
            argv = ['compare_sdk_oracle.py', '--log', str(log),
                    '--run-url', 'https://example.invalid/run',
                    '--ffmpeg', 'not-executed', '--ffprobe', 'not-executed',
                    '--output', str(output)]
            with patch.object(sys, 'argv', argv), self.assertRaisesRegex(
                    ValueError, 'too many oracle observations'):
                compare.main()
            self.assertEqual(list(output.iterdir()), [])


if __name__ == '__main__':
    unittest.main()
