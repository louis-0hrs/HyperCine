#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.

"""Regression coverage for the Windows SDK-oracle log replay filter."""
import base64
import hashlib
import importlib.util
import inspect
import json
import os
import subprocess
import struct
import sys
import tempfile
import time
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

    def test_external_decoded_control_read_is_bounded_and_exact(self):
        # The child decoder can produce an oversized file even if ffprobe
        # previously reported 128x128. The reader must never use read_bytes().
        self.assertIn('read_bounded_decoded(decoded)', inspect.getsource(compare.main))

        class GuardedOutput:
            def __init__(self, length):
                self.length = length
                self.requested = []

            def open(self, mode):
                if mode != 'rb':
                    raise AssertionError('expected binary read')
                return self

            def __enter__(self):
                return self

            def __exit__(self, *args):
                return False

            def read(self, size=-1):
                self.requested.append(size)
                if size != 65537:
                    raise AssertionError('unbounded FFmpeg output read')
                return bytes(min(size, self.length))

        for size in (0, 65535, 65536, 65537, 65538):
            with self.subTest(size=size):
                output = GuardedOutput(size)
                if size == 65536:
                    self.assertEqual(len(compare.read_bounded_decoded(output)), 65536)
                else:
                    with self.assertRaisesRegex(ValueError, 'invalid decoded control size'):
                        compare.read_bounded_decoded(output)
                self.assertEqual(output.requested, [65537])

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


    def test_retained_fixture_reads_are_io_bounded(self):
        # stat() alone cannot protect a read if a producer grows its file.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixtures = root / 'retained'
            fixtures.mkdir()
            name = 'flat.sdk-p10'
            first_file = fixtures / name
            first_file.write_bytes(bytes(65536))
            manifest_path = fixtures / 'manifest.json'
            manifest_path.write_text(json.dumps({
                'files': [{'file': item, 'bytes': 65536, 'sha256': '0' * 64}
                          for item in [name] + sorted(compare.ORACLE_NAMES - {name})],
                'run_url': 'https://github.com/louis-0hrs/HyperCine/actions/runs/1',
                'log_sha256': '0' * 64,
            }))
            real_open = Path.open
            seen = {}

            class BoundedReader:
                def __init__(self, stream, subject):
                    self.stream, self.subject = stream, subject

                def __enter__(self):
                    self.stream.__enter__()
                    return self

                def __exit__(self, *exc):
                    return self.stream.__exit__(*exc)

                def read(self, size=-1):
                    seen[self.subject.name] = size
                    # An unbounded read() would violate this contract.
                    expected = 64 * 1024 + 1 if self.subject == manifest_path else 65536 + 1
                    if size != expected:
                        raise AssertionError('oracle input read is not bounded')
                    return self.stream.read(size)

            def intercept_open(subject, *args, **kwargs):
                stream = real_open(subject, *args, **kwargs)
                # Replay resolves the retained root before reading packets.
                # Windows temporary roots may have alternate physical spellings.
                # Match the actual resolved fixture, not the original path text.
                if (args and args[0] == 'rb' and
                        subject.resolve() in (manifest_path.resolve(), first_file.resolve())):
                    return BoundedReader(stream, subject)
                return stream

            argv = ['compare_sdk_oracle.py', '--windows-fixtures', str(fixtures),
                    '--ffmpeg', 'unused', '--ffprobe', 'unused',
                    '--output', str(root / 'output')]
            with patch.object(sys, 'argv', argv), \
                 patch.object(Path, 'open', autospec=True, side_effect=intercept_open):
                with self.assertRaisesRegex(ValueError, 'fixture length or digest mismatch'):
                    compare.main()
            self.assertEqual(seen, {'manifest.json': 64 * 1024 + 1,
                                    name: 65536 + 1})

    def test_log_digest_binds_exact_parsed_bytes_even_if_file_changes(self):
        # A producer can rewrite the log immediately after the bounded read.
        original = b'status=1\nORACLE_DATA_BEGIN control.cfhd 4\n'
        changed = b'status=0\nORACLE_DATA_BEGIN control.cfhd 5\n'
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'sdk-oracle.log'
            path.write_bytes(original)
            real_open = Path.open
            sizes = []

            class MutatingReader:
                def __init__(self, stream):
                    self.stream = stream

                def __enter__(self):
                    self.stream.__enter__()
                    return self

                def __exit__(self, *exc):
                    # Close before rewriting: Windows may deny an in-place
                    # overwrite while the input handle remains open.
                    result = self.stream.__exit__(*exc)
                    path.write_bytes(changed)
                    return result

                def read(self, size=-1):
                    sizes.append(size)
                    return self.stream.read(size)

            def intercept_open(subject, *args, **kwargs):
                stream = real_open(subject, *args, **kwargs)
                if subject == path and args and args[0] == 'rb':
                    return MutatingReader(stream)
                return stream

            with patch.object(Path, 'open', autospec=True, side_effect=intercept_open), \
                 patch.object(Path, 'read_bytes',
                              side_effect=AssertionError('unbounded log read')):
                digest, lines = compare.read_bounded_log(path)
            self.assertEqual(sizes, [64 * 1024 * 1024 + 1])
            self.assertEqual(digest, hashlib.sha256(original).hexdigest())
            self.assertEqual(lines, original.decode('utf-8').splitlines())
            self.assertEqual(path.read_bytes(), changed)


class BoundedExternalProcess(unittest.TestCase):
    """#377: bounded child runtime and stdout captured under a hard cap."""

    def child(self, directory, source, name='synthetic-child.py'):
        """A disposable synthetic tool: never the real ffmpeg/ffprobe."""
        script = Path(directory) / name
        script.write_text(source, encoding='utf-8')
        return [sys.executable, '-I', '-B', str(script)]

    def test_nonzero_exit_fails_closed_without_echoing_the_banner(self):
        with tempfile.TemporaryDirectory() as directory:
            argv = self.child(directory,
                              'import sys\n'
                              'sys.stdout.write("/private/HOST_SECRET_PATH PRIVATE_TOKEN")\n'
                              'sys.exit(3)\n')
            with self.assertRaises(ValueError) as caught:
                compare.run_bounded(argv, timeout=60, limit=4096)
            text = str(caught.exception)
            self.assertEqual(text, 'external tool failed')
            for leaked in ('HOST_SECRET_PATH', 'PRIVATE_TOKEN', directory, 'synthetic'):
                self.assertNotIn(leaked, text)

    def test_hanging_child_is_killed_and_reaped(self):
        with tempfile.TemporaryDirectory() as directory:
            pid_file = Path(directory) / 'child.pid'
            argv = self.child(directory,
                              'import os, pathlib, time\n'
                              'pathlib.Path(' + repr(str(pid_file)) + ').write_text(str(os.getpid()))\n'
                              'while True:\n'
                              '    time.sleep(0.05)\n')
            started = time.monotonic()
            with self.assertRaisesRegex(
                    ValueError, 'external tool exceeded its runtime budget'):
                compare.run_bounded(argv, timeout=1.0, limit=4096)
            self.assertLess(time.monotonic() - started, 60)
            pid = int(pid_file.read_text())
            self.assertNotEqual(pid, os.getpid())
            if os.name == 'posix':
                # An unreaped child would still answer signal 0.
                with self.assertRaises(ProcessLookupError):
                    os.kill(pid, 0)

    def test_exited_parent_with_inherited_stdout_must_not_look_successful(self):
        """A departed root with a live pipe-holding descendant is not EOF.

        The retired implementation returned partial valid JSON after a fixed
        ten-second reader.join(), and left the descendant alive. This test
        requires fail-closed completion within the caller's wall budget.
        """
        with tempfile.TemporaryDirectory() as directory:
            pid_file = Path(directory) / 'held-pipe-grandchild.pid'
            grandchild = Path(directory) / 'held-pipe-grandchild.py'
            grandchild.write_text(
                'import os, pathlib, time\n'
                + f'pathlib.Path({str(pid_file)!r}).write_text(str(os.getpid()))\n'
                + 'time.sleep(30)\n', encoding='utf-8')
            argv = self.child(directory,
                'import json, pathlib, subprocess, sys, time\n'
                + f'subprocess.Popen([sys.executable, "-I", "-B", {str(grandchild)!r}], '
                + 'stdin=subprocess.DEVNULL, stderr=subprocess.DEVNULL)\n'
                + 'for _ in range(200):\n'
                + f'    if pathlib.Path({str(pid_file)!r}).exists():\n'
                + '        break\n'
                + '    time.sleep(0.01)\n'
                + 'print(json.dumps({"streams": [{"codec_name": "cfhd"}]}), flush=True)\n',
                'held-pipe-parent.py')
            started = time.monotonic()
            descendant_pid = None
            try:
                with self.assertRaisesRegex(
                        ValueError, 'external tool exceeded its runtime budget'):
                    compare.run_bounded(argv, timeout=3.0, limit=65536)
                self.assertLess(time.monotonic() - started, 8.0)
                self.assertTrue(pid_file.exists(), 'synthetic descendant did not start')
                descendant_pid = int(pid_file.read_text())
                if os.name == 'posix':
                    # Zombies are terminated, not running; they may temporarily
                    # remain visible when an unrelated init has yet to reap them.
                    for _ in range(30):
                        stat_path = Path(f'/proc/{descendant_pid}/stat')
                        if stat_path.exists():
                            alive = stat_path.read_text().split()[2] != 'Z'
                        else:
                            try:
                                os.kill(descendant_pid, 0)
                            except ProcessLookupError:
                                alive = False
                            else:
                                alive = True
                        if not alive:
                            break
                        time.sleep(0.05)
                    self.assertFalse(alive, 'pipe-holding descendant survived cleanup')
            finally:
                # A failed regression must not leave synthetic test processes
                # running on the builder. Windows tree cleanup requires a
                # separate native proof (taskkill may see an exited parent).
                if descendant_pid is None and pid_file.exists():
                    descendant_pid = int(pid_file.read_text())
                if descendant_pid is not None:
                    if os.name == 'nt':
                        subprocess.run(['taskkill', '/F', '/PID', str(descendant_pid)],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                       timeout=10, check=False)
                    else:
                        try:
                            os.kill(descendant_pid, 9)
                        except ProcessLookupError:
                            pass

    def test_large_stdout_is_cut_off_while_it_is_produced(self):
        with tempfile.TemporaryDirectory() as directory:
            # 8 MiB cannot sit in a pipe buffer: the writer blocks until we kill it.
            argv = self.child(directory,
                              'import sys\n'
                              'sys.stdout.write("x" * (8 * 1024 * 1024))\n')
            started = time.monotonic()
            with self.assertRaisesRegex(ValueError,
                                        'external tool produced too much output'):
                compare.run_bounded(argv, timeout=60, limit=1024)
            self.assertLess(time.monotonic() - started, 60)

    def test_exact_limit_is_accepted_and_one_more_byte_is_not(self):
        with tempfile.TemporaryDirectory() as directory:
            exact = self.child(directory,
                               'import sys\n'
                               'sys.stdout.write("A" * 4096)\n', 'exact.py')
            self.assertEqual(len(compare.run_bounded(exact, timeout=60, limit=4096)), 4096)
            one_more = self.child(directory,
                                 'import sys\n'
                                 'sys.stdout.write("A" * 4097)\n', 'more.py')
            with self.assertRaisesRegex(ValueError, 'too much output'):
                compare.run_bounded(one_more, timeout=60, limit=4096)

    def test_suppressed_stdout_is_never_captured(self):
        with tempfile.TemporaryDirectory() as directory:
            argv = self.child(directory,
                              'import sys\n'
                              'sys.stdout.write("x" * (4 * 1024 * 1024))\n')
            # The decode lane discards stdout: a chatty tool must not stall or buffer.
            self.assertEqual(compare.run_bounded(argv, timeout=60), b'')

    def test_missing_tool_fails_closed_without_the_path(self):
        with tempfile.TemporaryDirectory() as directory:
            missing = str(Path(directory) / 'absent-tool-PRIVATE_TOKEN')
            with self.assertRaisesRegex(ValueError, 'external tool unavailable') as caught:
                compare.run_bounded([missing], timeout=10, limit=64)
            self.assertNotIn('PRIVATE_TOKEN', str(caught.exception))

    def test_invalid_budget_is_refused_before_spawning(self):
        for timeout in (0, -1, float('nan'), float('inf'), True, None, '30'):
            with self.subTest(timeout=timeout), \
                 patch.object(compare.subprocess, 'Popen',
                              side_effect=AssertionError('spawned without a budget')):
                with self.assertRaisesRegex(ValueError, 'invalid external tool budget'):
                    compare.run_bounded([sys.executable, '-V'], timeout=timeout, limit=64)
        for limit in (-1, True, 1.5, '64'):
            with self.subTest(limit=limit), \
                 patch.object(compare.subprocess, 'Popen',
                              side_effect=AssertionError('spawned without a budget')):
                with self.assertRaisesRegex(ValueError, 'invalid external tool budget'):
                    compare.run_bounded([sys.executable, '-V'], timeout=10, limit=limit)

    def test_mutation_control_legacy_capture_is_unbounded(self):
        """The retired check_output() collected everything; the fix refuses at the cap."""
        with tempfile.TemporaryDirectory() as directory:
            argv = self.child(directory,
                              'import sys\n'
                              'sys.stdout.write("A" * (512 * 1024))\n')
            self.assertEqual(len(subprocess.check_output(argv)), 512 * 1024)
            with self.assertRaisesRegex(ValueError, 'too much output'):
                compare.run_bounded(argv, timeout=60, limit=1024)

    def test_replay_uses_only_the_bounded_runner_for_external_tools(self):
        source = inspect.getsource(compare.main)
        self.assertNotIn('check_output', source)
        self.assertNotIn('capture_output', source)
        self.assertNotIn('subprocess.run(', source)
        self.assertEqual(source.count('run_bounded('), 3)
        # The one-frame decode keeps FFmpeg's no-overwrite semantics.
        self.assertIn("'-n'", source)
        for budget in ('VERSION_BUDGET_SECONDS', 'PROBE_BUDGET_SECONDS',
                       'DECODE_BUDGET_SECONDS'):
            self.assertIn(budget, source)

    def test_malformed_probe_payload_is_rejected(self):
        for payload in (b'not json', b'{"streams": []}', b'{}', b'[]'):
            with self.subTest(payload=payload):
                with self.assertRaisesRegex(ValueError, 'invalid oracle stream'):
                    compare.parse_probe_stream(payload)
        for payload in (b'', b'\xff\xfe'):
            with self.subTest(payload=payload):
                with self.assertRaisesRegex(ValueError, 'invalid external tool output'):
                    compare.parse_probe_stream(payload)


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
