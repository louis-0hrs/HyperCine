# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Corpus-free controls for the compatibility qualification matrix.

The matrix is evidence, so every guard below is a way it could lie: a row that
claims identity while carrying differing pixels, a verdict that contradicts its
own measurement, a decoder that was never named, a feature without a measured
case presented as a pass, or an artifact whose bytes no longer match its
recorded digest.

Synthetic fixtures only; no retained corpus and no external decoder is needed.
"""
import copy
import os
from unittest import mock
import pathlib
import subprocess
import time
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import compat_qualification as cq
import compat_process
from compat_process import run_bounded

FRAME = bytes(range(256)) * 256
OTHER = bytes(reversed(bytes(range(256)))) * 256

FIXTURES = (
    ('standalone', 'flat.cfhd', FRAME),
    ('standalone', 'flat.input-p10', FRAME),
    ('standalone', 'flat.p10', FRAME),
    ('standalone', 'flat.bgra', FRAME),
    ('standalone', 'canonical.cfhd', OTHER),
    ('standalone', 'canonical.input-p10', FRAME),
    ('standalone', 'canonical.p10', OTHER),
    ('standalone', 'canonical.bgra', FRAME),
    ('standalone', 'gradient.cfhd', FRAME),
    ('standalone', 'gradient.input-p10', FRAME),
    ('standalone', 'gradient.p10', FRAME),
    ('standalone', 'gradient.bgra', FRAME),
    ('windows-oracle', 'flat.sdk-p10', FRAME),
    ('windows-oracle', 'flat.sdk-bgra', FRAME),
    ('windows-oracle', 'canonical.sdk-p10', OTHER),
    ('windows-oracle', 'canonical.sdk-bgra', FRAME),
    ('windows-oracle', 'gradient.sdk-p10', FRAME),
    ('windows-oracle', 'gradient.sdk-bgra', FRAME),
    ('windows-oracle', 'control.cfhd', FRAME),
    ('windows-oracle', 'control.sdk-p10', FRAME),
    ('macos-filmscan3', 'flat.cfhd', FRAME),
    ('macos-filmscan3', 'gradient.cfhd', FRAME),
)


def digest(payload):
    return cq.sha256_bytes(payload)


class SyntheticTree:
    """Build or mutate a fixture tree with honest or dishonestly edited digests."""

    def __init__(self, root):
        self.root = root

    def write(self, tamper=None, drop=()):
        groups = {}
        for group, name, payload in FIXTURES:
            if name in drop:
                continue
            groups.setdefault(group, []).append((name, payload))
        for group, entries in groups.items():
            directory = self.root / group
            directory.mkdir(parents=True, exist_ok=True)
            records = []
            for name, payload in entries:
                (directory / name).write_bytes(payload)
                size, sha = len(payload), digest(payload)
                if tamper and name == tamper[0]:
                    sha = tamper[1]
                records.append({'file': name, 'bytes': size, 'sha256': sha})
            (directory / 'manifest.json').write_text(json.dumps({'files': records}))
        return self.root


class Inventory(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def build(self, **kwargs):
        SyntheticTree(self.root).write(**kwargs)
        return cq.inventory(self.root)

    def test_inventory_binds_roles_and_payloads(self):
        artifacts, payloads = self.build()
        self.assertEqual(artifacts['standalone/flat.input-p10']['role'],
                         'encoder_input_p10')
        self.assertEqual(artifacts['windows-oracle/control.cfhd']['role'],
                         'legacy_gopro_packet')
        self.assertEqual(payloads['standalone/flat.cfhd'], FRAME)

    def test_digest_mismatch_is_refused(self):
        with self.assertRaises(ValueError) as caught:
            self.build(tamper=('control.cfhd', '0' * 64))
        self.assertEqual(str(caught.exception), 'artifact digest mismatch')

    def test_declared_length_mismatch_is_refused(self):
        SyntheticTree(self.root).write()
        manifest = self.root / 'standalone' / 'manifest.json'
        records = json.loads(manifest.read_text())
        for record in records['files']:
            if record['file'] == 'flat.cfhd':
                record['bytes'] = 65535
        manifest.write_text(json.dumps(records))
        with self.assertRaises(ValueError) as caught:
            cq.inventory(self.root)
        self.assertEqual(str(caught.exception), 'artifact length mismatch')

    def test_unknown_artifact_name_is_refused(self):
        SyntheticTree(self.root).write()
        manifest = self.root / 'standalone' / 'manifest.json'
        records = json.loads(manifest.read_text())
        records['files'].append({'file': 'unrelated.bin', 'bytes': 1,
                                 'sha256': '0' * 64})
        manifest.write_text(json.dumps(records))
        with self.assertRaises(ValueError) as caught:
            cq.inventory(self.root)
        self.assertEqual(str(caught.exception), 'unclassified reference artifact')

    def test_path_escape_is_refused(self):
        with self.assertRaises(ValueError):
            cq.bound_artifact(self.root, '../escape.p10', 3, '0' * 64)
        with self.assertRaises(ValueError):
            cq.bound_artifact(self.root, 'a/../../escape.p10', 3, '0' * 64)

    def test_missing_manifest_is_refused(self):
        (self.root / 'standalone').mkdir()
        with self.assertRaises(ValueError) as caught:
            cq.load_manifest(self.root / 'standalone' / 'manifest.json')
        self.assertEqual(str(caught.exception), 'missing fixture manifest')


class Matrix(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        SyntheticTree(self.root).write()
        self.artifacts, self.payloads = cq.inventory(self.root)
        self.matrix = cq.build_matrix(self.artifacts, self.payloads)

    def row(self, identifier):
        return next(row for row in self.matrix['rows'] if row['id'] == identifier)

    def test_build_passes_its_own_validation(self):
        cq.validate_matrix(self.matrix)
        cq.validate_matrix(self.matrix, self.root)

    def test_identical_row_may_bind_equal_digests(self):
        row = self.row('hypercine-packet-flat')
        self.assertEqual(row['status'], cq.STATUS_IDENTITY)
        for item in row['comparisons']:
            self.assertEqual(item['metrics']['mismatches'], 0)

    def test_differing_row_is_not_an_identity(self):
        row = self.row('hypercine-packet-canonical')
        self.assertEqual(row['status'], cq.STATUS_DIFFERENCE)
        self.assertTrue(any(item['metrics']['mismatches'] > 0
                            for item in row['comparisons']))

    def test_packets_without_capture_are_not_observable(self):
        row = self.row('hypercine-metal-filmscan3-packets')
        self.assertEqual(row['status'], cq.STATUS_NOT_OBSERVABLE)
        self.assertEqual(row['comparisons'], [])

    def test_identity_claim_with_measured_error_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        item = broken['rows'][0]['comparisons'][0]
        item['metrics']['mismatches'] = 12
        item['metrics']['max_error'] = 3
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception),
                         'IDENTICAL verdict contradicts the measured error')

    def test_difference_without_measured_error_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        item = broken['rows'][0]['comparisons'][0]
        item['verdict'] = cq.VERDICT_DIFFERENT
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception),
                         'DIFFERENT verdict without a measured difference')

    def test_status_contradicting_comparisons_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        broken['rows'][1]['status'] = cq.STATUS_IDENTITY
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception),
                         'row status contradicts its comparisons')

    def test_unbound_local_decoder_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        broken['rows'][0]['comparisons'].append({
            'basis': cq.BASIS_LOCAL,
            'decoder': cq.recorded_decoder('ffmpeg-cfhd', 'n8.0'),
            'capture': {'artifact': 'local/x.p10', 'sha256': digest(FRAME)},
            'against': {'artifact': 'standalone/flat.p10', 'sha256': digest(FRAME)},
            'metrics': cq.metrics(0, 0, None, 32768),
            'verdict': cq.VERDICT_IDENTICAL,
        })
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(
            str(caught.exception),
            'local measurement without a bound decoder executable')

    def test_committed_comparison_may_not_claim_a_binary(self):
        broken = copy.deepcopy(self.matrix)
        broken['rows'][0]['comparisons'][0]['decoder']['binary_sha256'] = '0' * 64
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(
            str(caught.exception),
            'committed-artifact comparison must not claim a binary digest')

    def test_identity_outside_admitted_scope_is_refused(self):
        for field, value in (('scan', 'interlaced'), ('gop', 2),
                             ('pixel_format', 'rgba10le'), ('geometry', '1920x1080')):
            broken = copy.deepcopy(self.matrix)
            broken['rows'][0]['scope'][field] = value
            with self.assertRaises(ValueError) as caught:
                cq.validate_matrix(broken)
            self.assertIn('outside the admitted scope', str(caught.exception))

    def test_self_comparison_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        item = broken['rows'][0]['comparisons'][0]
        item['against'] = item['capture']
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception),
                         'comparison compares an artifact with itself')

    def test_backlog_entry_carrying_a_verdict_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        broken['backlog'][0]['status'] = cq.STATUS_IDENTITY
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception),
                         'backlog entry carries unexpected fields')

    def test_empty_backlog_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        broken['backlog'] = []
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception), 'backlog must be enumerated')

    def test_duplicate_row_id_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        broken['rows'].append(copy.deepcopy(broken['rows'][0]))
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception), 'duplicate matrix row id')

    def test_unknown_direction_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        broken['rows'][0]['direction'] = 'universal-interoperability'
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception),
                         'unknown compatibility direction')

    def test_zero_sample_comparison_is_refused(self):
        broken = copy.deepcopy(self.matrix)
        broken['rows'][0]['comparisons'][0]['metrics']['samples'] = 0
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken)
        self.assertEqual(str(caught.exception), 'comparison without samples')

    def test_tampered_packet_digest_is_refused_on_disk(self):
        broken = copy.deepcopy(self.matrix)
        broken['rows'][0]['packet']['sha256'] = '1' * 64
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(broken, self.root)
        self.assertEqual(str(caught.exception), 'artifact digest mismatch')

    def test_mutated_packet_bytes_are_refused_on_disk(self):
        (self.root / 'standalone' / 'flat.cfhd').write_bytes(OTHER)
        with self.assertRaises(ValueError) as caught:
            cq.validate_matrix(self.matrix, self.root)
        self.assertEqual(str(caught.exception), 'artifact digest mismatch')


class ReplayControls(unittest.TestCase):
    setUp = Matrix.setUp
    row = Matrix.row
    def test_mutated_capture(self):
        (self.root / 'standalone/flat.p10').write_bytes(OTHER)
        with self.assertRaisesRegex(ValueError, 'artifact digest mismatch'):
            cq.validate_matrix(self.matrix, self.root)

    def test_forged_consistent_metrics(self):
        item = self.matrix['rows'][1]['comparisons'][0]
        item['metrics']['max_error'] += 1
        with self.assertRaisesRegex(ValueError, 'comparison metrics mismatch'):
            cq.validate_matrix(self.matrix, self.root)

    def test_wrong_linkage(self):
        self.matrix['rows'][0]['comparisons'][0]['capture'] = cq.artifact_ref(
            self.artifacts, 'standalone/gradient.p10', 'committed_ffmpeg_decoded_p10')
        with self.assertRaisesRegex(ValueError, 'packet linkage mismatch'):
            cq.validate_matrix(self.matrix, self.root)

    def test_wrong_packet_role(self):
        self.matrix['rows'][0]['packet']['role'] = 'legacy_gopro_packet'
        with self.assertRaisesRegex(ValueError, 'packet role mismatch'):
            cq.validate_matrix(self.matrix, self.root)

    def test_json_scope_types(self):
        for field, value in [('gop', True), ('gop', 1.0), ('bit_depth', 10.0),
                             ('bit_depth', True), ('scan', 1), ('geometry', []),
                             ('range', None), ('chroma_siting', False), ('pixel_format', 0)]:
            broken = json.loads(json.dumps(self.matrix))
            broken['rows'][0]['scope'][field] = value
            with self.assertRaisesRegex(ValueError, 'invalid scope type: ' + field):
                cq.validate_matrix(broken, self.root)


class ProcessControls(unittest.TestCase):
    def run_tool(self, source, **kwargs):
        from compat_process import run_bounded
        return run_bounded([sys.executable, '-c', source], **kwargs)

    def test_success(self):
        self.assertEqual(self.run_tool("print('ok')"), b'ok\n' if sys.platform != 'win32' else b'ok\r\n')

    def test_scalar_executable_and_pathlike(self):
        for executable in (sys.executable, Path(sys.executable)):
            with self.subTest(executable=type(executable).__name__):
                self.assertEqual(run_bounded(executable, timeout=5), b'')

    def test_hanging(self):
        with self.assertRaisesRegex(ValueError, '^tool timeout$'):
            self.run_tool('import time; time.sleep(60)', timeout=0.2)

    def test_overflow(self):
        with self.assertRaisesRegex(ValueError, '^tool output limit exceeded$'):
            self.run_tool("import os;\nwhile True: os.write(1,b'x'*4096)", stdout_limit=8192)

    def test_nonzero_secret_banner(self):
        with self.assertRaisesRegex(ValueError, '^tool nonzero exit$'):
            self.run_tool("import sys; print('SECRET_BANNER_PRIVATE'); sys.exit(3)")

    def test_stderr_overflow(self):
        with self.assertRaisesRegex(ValueError, '^tool output limit exceeded$'):
            self.run_tool("import os;\nwhile True: os.write(2,b'x'*4096)", stdout_limit=8192)

    def test_discard_stdout_drains_large_output_without_retaining_it(self):
        # No output is retained. The process still has to reach clean EOF.
        script = "import os;\nfor _ in range(64): os.write(1,b'x'*65536)"
        self.assertEqual(self.run_tool(script, timeout=15, stdout_limit=4096,
                                       discard_stdout=True), b'')
        with self.assertRaisesRegex(ValueError, '^tool output limit exceeded$'):
            self.run_tool(script, timeout=15, stdout_limit=4096)

    def test_discard_stdout_keeps_stderr_bounded_and_sanitized(self):
        with self.assertRaisesRegex(ValueError, '^tool output limit exceeded$') as caught:
            self.run_tool("import os;os.write(2,b'PRIVATE_STDERR_CANARY'+b'x'*8192)",
                          timeout=5, stdout_limit=4096, discard_stdout=True)
        self.assertNotIn('PRIVATE_STDERR_CANARY', str(caught.exception))

    def test_discard_stdout_keeps_exit_and_parameter_validation(self):
        with self.assertRaisesRegex(ValueError, '^tool nonzero exit$'):
            self.run_tool("import sys;print('PRIVATE_STDOUT_CANARY');sys.exit(7)",
                          timeout=5, stdout_limit=4096, discard_stdout=True)
        with self.assertRaisesRegex(ValueError, '^invalid tool budget$'):
            self.run_tool("print('should never execute')", discard_stdout=1)

    def test_discard_stdout_still_requires_descendant_pipe_eof(self):
        with tempfile.TemporaryDirectory() as temp:
            sentinel = Path(temp) / 'escaped'
            child = "import time,pathlib;time.sleep(1.0);pathlib.Path(%r).write_text('escaped')" % str(sentinel)
            parent = "import subprocess,sys;subprocess.Popen([sys.executable,'-c',%r]);print('ready')" % child
            with self.assertRaisesRegex(ValueError, '^tool timeout$'):
                self.run_tool(parent, timeout=0.3, stdout_limit=4096, discard_stdout=True)
            time.sleep(1.2)
            self.assertFalse(sentinel.exists(), 'discard mode lost descendant containment')

    def test_malformed_json(self):
        with self.assertRaises(json.JSONDecodeError):
            json.loads(self.run_tool("print('{bad')"))

    def test_launch_error_sanitized(self):
        from compat_process import run_bounded
        with self.assertRaisesRegex(ValueError, '^tool launch failed$'):
            run_bounded(['MISSING_SECRET_PRIVATE_EXECUTABLE'])

    def test_exited_parent_grandchild_cleanup(self):
        import time
        with tempfile.TemporaryDirectory() as temp:
            sentinel = Path(temp) / 'escaped'
            child = "import time,pathlib; time.sleep(1); pathlib.Path(%r).write_text('escaped')" % str(sentinel)
            source = "import subprocess,sys; subprocess.Popen([sys.executable,'-c',%r],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)" % child
            self.run_tool(source, timeout=0.5)
            time.sleep(1.2)
            self.assertFalse(sentinel.exists(), 'grandchild escaped containment')

    def test_inherited_pipe_descendant(self):
        import time
        with tempfile.TemporaryDirectory() as temp:
            sentinel = Path(temp) / 'escaped'
            child = "import time,pathlib; time.sleep(1); pathlib.Path(%r).write_text('escaped')" % str(sentinel)
            source = "import subprocess,sys; subprocess.Popen([sys.executable,'-c',%r])" % child
            with self.assertRaisesRegex(ValueError, '^tool timeout$'):
                self.run_tool(source, timeout=0.3)
            time.sleep(1.1)
            self.assertFalse(sentinel.exists())


class WindowsCleanupModelControls(unittest.TestCase):
    """Control-flow model only; real OVERLAPPED behavior needs native Windows."""
    class Stream:
        def __init__(self, number, fail=False):
            self.number, self.writer = number, number + 100
            self.fail, self.closed, self.pending = fail, False, False
            self.reads = [None]
        def read_ready(self): return self.reads.pop(0) if self.reads else None
        def close_writer(self): self.writer = None
        def cancel(self): pass
        def finish_pending(self): return not self.pending
        def close(self):
            self.closed = True
            if self.fail: raise OSError('PRIVATE_CLOSE_CANARY')

    def setUp(self):
        self.out, self.err = self.Stream(901), self.Stream(902)
        self.process = mock.Mock(_handle=123, returncode=0)
        self.process.poll.return_value = 0
        self.process.wait.return_value = 0
        self.kernel = mock.Mock()
        self.kernel.AssignProcessToJobObject.return_value = 1
        self.kernel.CloseHandle.return_value = 1
        self.kernel.TerminateJobObject.return_value = 1
        self.job = 456
        self.patches = [
            mock.patch('compat_process._windows_job', return_value=(self.kernel, self.job)),
            mock.patch('compat_process._windows_resume', return_value=True),
            mock.patch('compat_process.subprocess.Popen', return_value=self.process),
            mock.patch('compat_process._windows_open_pipe', side_effect=[self.out, self.err]),
            mock.patch('compat_process._retain_reap'),
            mock.patch('compat_process._retain_job'),
            mock.patch('compat_process._retain_io'),
        ]
        self.mocks = [p.start() for p in self.patches]
        self.addCleanup(lambda: [p.stop() for p in reversed(self.patches)])

    def execute(self): return compat_process._run_windows(['PRIVATE_ARGV_CANARY'], .2, 8)

    def test_unknown_overlapped_result_retains_buffer_and_operation(self):
        import ctypes
        pipe = object.__new__(compat_process._WindowsPipe)
        pipe.kernel = mock.Mock()
        pipe.kernel.GetOverlappedResult.return_value = 0
        pipe.handle, pipe.event = 111, 222
        pipe.pending = True
        pipe.overlap = compat_process._Overlapped()
        pipe.buffer = ctypes.create_string_buffer(4096)
        original = pipe.buffer
        with mock.patch.object(compat_process.ctypes, 'get_last_error', return_value=5, create=True):
            with self.assertRaises(OSError): pipe.read_ready()
        self.assertTrue(pipe.pending)
        self.assertIs(pipe.buffer, original)

    def test_overlapped_read_does_not_pass_ephemeral_count_pointer(self):
        pipe = object.__new__(compat_process._WindowsPipe)
        pipe.kernel = mock.Mock()
        pipe.kernel.ResetEvent.return_value = 1
        pipe.kernel.ReadFile.return_value = 0
        pipe.kernel.GetOverlappedResult.return_value = 0
        pipe.handle, pipe.event, pipe.pending = 111, 222, False
        with mock.patch.object(compat_process.ctypes, 'get_last_error', side_effect=[997, 996], create=True):
            self.assertEqual(pipe.read_ready(), b'')
        self.assertTrue(pipe.pending)
        self.assertIsNone(pipe.kernel.ReadFile.call_args.args[3])

    def test_interrupt_at_read_submission_keeps_overlapped_pinned(self):
        pipe = object.__new__(compat_process._WindowsPipe)
        pipe.kernel = mock.Mock()
        pipe.kernel.ResetEvent.return_value = 1
        pipe.kernel.ReadFile.side_effect = KeyboardInterrupt('FIRST')
        pipe.handle, pipe.event, pipe.pending = 111, 222, False
        with self.assertRaisesRegex(KeyboardInterrupt, 'FIRST'):
            pipe.read_ready()
        self.assertTrue(pipe.pending)
        self.assertIsNotNone(pipe.overlap)
        self.assertIsNotNone(pipe.buffer)

    def test_assignment_before_resume_and_explicit_pipes(self):
        def resume(process):
            self.kernel.AssignProcessToJobObject.assert_called_once_with(self.job, 123)
            return True
        self.mocks[1].side_effect = resume
        self.assertEqual(self.execute(), b'')
        kwargs = self.mocks[2].call_args.kwargs
        self.assertEqual(kwargs['creationflags'], 4)
        self.assertEqual(kwargs['stdout'], 1001)
        self.assertEqual(kwargs['stderr'], 1002)
        self.assertTrue(self.out.closed and self.err.closed)

    def test_job_close_failure_never_returns_bytes_and_retains_owner(self):
        self.kernel.CloseHandle.return_value = 0
        with self.assertRaises(compat_process.ToolFailure) as caught: self.execute()
        self.assertTrue(caught.exception.cleanup_failed)
        self.assertTrue(caught.exception.containment_unconfirmed)
        self.kernel.TerminateJobObject.assert_called_once_with(self.job, 1)
        self.mocks[5].assert_called_once_with(self.kernel, self.job)
        self.assertTrue(self.out.closed and self.err.closed)

    def test_failed_job_close_then_success_still_refuses_output(self):
        self.kernel.CloseHandle.side_effect = [0, 1]
        with self.assertRaises(compat_process.ToolFailure) as caught: self.execute()
        self.assertTrue(caught.exception.containment_unconfirmed)
        self.kernel.TerminateJobObject.assert_called_once_with(self.job, 1)
        self.mocks[5].assert_not_called()

    def test_wait_timeout_is_sanitized_and_disposes_both(self):
        self.out.reads = [b''] * 10000
        self.err.reads = [b''] * 10000
        self.process.poll.return_value = None
        self.process.returncode = None
        self.process.wait.side_effect = subprocess.TimeoutExpired('PRIVATE_ARGV_CANARY', 1)
        with self.assertRaises(compat_process.ToolFailure) as caught: self.execute()
        self.assertNotIn('PRIVATE_ARGV_CANARY', str(caught.exception))
        self.assertTrue(caught.exception.cleanup_failed)
        self.mocks[4].assert_called_once_with(self.process)
        self.assertTrue(self.out.closed and self.err.closed)

    def test_poll_failure_still_attempts_owned_process_termination(self):
        self.process.poll.side_effect = OSError('PRIVATE_POLL_CANARY')
        with self.assertRaises(compat_process.ToolFailure) as caught: self.execute()
        self.process.kill.assert_called_once()
        self.assertNotIn('PRIVATE_POLL_CANARY', str(caught.exception))
        self.assertTrue(self.out.closed and self.err.closed)

    def test_first_close_failure_still_closes_second(self):
        self.out.fail = True
        with self.assertRaises(compat_process.ToolFailure) as caught: self.execute()
        self.assertNotIn('PRIVATE_CLOSE_CANARY', str(caught.exception))
        self.assertTrue(caught.exception.cleanup_failed)
        self.assertTrue(self.out.closed and self.err.closed)

    def test_unconfirmed_job_retained_pipe_is_bounded_and_preserves_timeout(self):
        self.out.reads = [b''] * 10000
        self.err.reads = [b''] * 10000
        self.kernel.CloseHandle.return_value = 0
        self.kernel.TerminateJobObject.return_value = 0
        start = time.monotonic()
        with self.assertRaises(compat_process.ToolFailure) as caught: self.execute()
        self.assertLess(time.monotonic() - start, 2)
        self.assertEqual(caught.exception.primary_category, 'tool timeout')
        self.assertTrue(caught.exception.cleanup_failed)
        self.assertTrue(caught.exception.containment_unconfirmed)
        self.mocks[5].assert_called_once_with(self.kernel, self.job)
        self.assertTrue(self.out.closed and self.err.closed)

    def test_pending_cancellation_failure_still_disposes_other_stream(self):
        self.out.pending = True
        self.out.reads = [b''] * 10000
        self.err.reads = [b''] * 10000
        self.out.cancel = mock.Mock(side_effect=OSError('PRIVATE_CANCEL'))
        def finish():
            self.out.pending = False
            return True
        self.out.finish_pending = finish
        with self.assertRaises(compat_process.ToolFailure) as caught:
            self.execute()
        self.assertEqual(caught.exception.primary_category, 'tool timeout')
        self.assertTrue(caught.exception.cleanup_failed)
        self.assertNotIn('PRIVATE_CANCEL', str(caught.exception))
        self.assertTrue(self.out.closed and self.err.closed)

    def test_unfinished_overlapped_operation_retains_buffer_and_handle(self):
        self.out.pending = True
        self.out.reads = [b''] * 10000
        self.err.reads = [b''] * 10000
        self.out.finish_pending = mock.Mock(return_value=False)
        ticks = [time.monotonic()]
        def clock():
            ticks[0] += .15
            return ticks[0]
        with mock.patch('compat_process.time.monotonic', side_effect=clock):
            with self.assertRaises(compat_process.ToolFailure) as caught:
                self.execute()
        self.assertTrue(caught.exception.cleanup_failed)
        self.mocks[6].assert_called_once_with(self.out)
        self.assertFalse(self.out.closed)
        self.assertTrue(self.err.closed)

    def test_assignment_refusal_never_resumes_child(self):
        self.kernel.AssignProcessToJobObject.return_value = 0
        self.process.poll.return_value = None
        with self.assertRaisesRegex(compat_process.ToolFailure, 'containment unavailable'): self.execute()
        self.mocks[1].assert_not_called()
        self.process.kill.assert_called_once()
        self.assertTrue(self.out.closed and self.err.closed)

    def test_produced_bytes_and_both_limits(self):
        self.out.reads = [b'abc', None]
        self.assertEqual(self.execute(), b'abc')

    def test_first_wait_interrupt_is_retried_then_preserved(self):
        self.out.reads = [b''] * 10000
        self.err.reads = [b''] * 10000
        self.process.poll.return_value = None
        self.process.returncode = None
        self.process.wait.side_effect = [KeyboardInterrupt('FIRST'), 0]
        with self.assertRaisesRegex(KeyboardInterrupt, 'FIRST'): self.execute()
        self.assertEqual(self.process.wait.call_count, 2)
        self.assertTrue(self.out.closed and self.err.closed)


@unittest.skipUnless(os.name == 'nt', 'native Windows pipe-cancellation gate')
class WindowsNativeCleanupControls(unittest.TestCase):
    def test_real_overlapped_pipe_pending_read_and_cancel(self):
        kernel, job = compat_process._windows_job()
        pipe = None
        try:
            pipe = compat_process._windows_open_pipe(kernel)
            self.assertIsNotNone(pipe.handle)
            self.assertIsNotNone(pipe.event)
            self.assertIsNotNone(pipe.writer)
            self.assertEqual(pipe.read_ready(), b'')
            self.assertTrue(pipe.pending)
            pipe.cancel()
            deadline = time.monotonic() + 2
            while pipe.pending and time.monotonic() < deadline:
                pipe.finish_pending()
                if pipe.pending: time.sleep(.005)
            self.assertFalse(pipe.pending, 'overlapped cancellation did not complete')
        finally:
            if pipe is not None:
                pipe.close_writer()
                if pipe.pending:
                    compat_process._retain_io(pipe)
                else:
                    pipe.close()
            self.assertTrue(kernel.CloseHandle(job))

    def test_successful_writer_close_consumes_both_owners_once(self):
        import ctypes, msvcrt
        from ctypes import wintypes as w
        kernel, job = compat_process._windows_job()
        pipe = compat_process._windows_open_pipe(kernel)
        kernel.GetHandleInformation.argtypes = [w.HANDLE, ctypes.POINTER(w.DWORD)]
        kernel.GetHandleInformation.restype = w.BOOL
        raw, witness = msvcrt.get_osfhandle(pipe.writer), pipe.writer_witness
        try:
            pipe.close_writer()
            self.assertEqual(pipe.writer_state, 'closed')
            self.assertIsNone(pipe.writer)
            self.assertIsNone(pipe.writer_handle)
            self.assertIsNone(pipe.writer_witness)
            info = w.DWORD()
            self.assertFalse(kernel.GetHandleInformation(raw, ctypes.byref(info)))
            self.assertFalse(kernel.GetHandleInformation(witness, ctypes.byref(info)))
            pipe.close_writer()  # must not reclose either old numeric handle
        finally:
            pipe.close()
            self.assertTrue(kernel.CloseHandle(job))

    def test_setup_failure_after_crt_adoption_retains_protected_writer(self):
        import msvcrt
        from ctypes import wintypes as w
        kernel, job = compat_process._windows_job()
        kernel.SetHandleInformation.argtypes = [w.HANDLE, w.DWORD, w.DWORD]
        kernel.SetHandleInformation.restype = w.BOOL
        real_adopt = msvcrt.open_osfhandle
        owned = []
        class FailEvent:
            def __init__(self, wrapped):
                self.wrapped = wrapped
                self.CreateEventW = mock.Mock(return_value=0)
            def __getattr__(self, name): return getattr(self.wrapped, name)
        def adopt(raw, flags):
            fd = real_adopt(raw, flags)
            if not kernel.SetHandleInformation(raw, 2, 2): raise RuntimeError('protect failed')
            owned.append(raw)
            return fd
        try:
            with mock.patch.object(msvcrt, 'open_osfhandle', side_effect=adopt):
                with self.assertRaises(compat_process.ToolFailure) as caught:
                    compat_process._windows_open_pipe(FailEvent(kernel))
            self.assertTrue(caught.exception.cleanup_failed)
            self.assertEqual(len(owned), 1)
            with compat_process._OWNED_IO_LOCK:
                pipes = [p for p in compat_process._OWNED_IO if p.writer_handle == owned[0]]
            self.assertEqual(len(pipes), 1)
            self.assertTrue(kernel.SetHandleInformation(owned[0], 2, 0))
            compat_process._retry_owned_io(pipes[0])
        finally:
            if owned:
                kernel.SetHandleInformation(owned[0], 2, 0)
                with compat_process._OWNED_IO_LOCK:
                    pipes = [p for p in compat_process._OWNED_IO if p.writer_handle == owned[0]]
                for p in pipes:
                    try: compat_process._retry_owned_io(p)
                    except OSError: pass
            self.assertTrue(kernel.CloseHandle(job))

    def test_protected_writer_is_retained_and_recovered_without_fd_retry(self):
        import ctypes, msvcrt
        from ctypes import wintypes as w
        kernel, job = compat_process._windows_job()
        pipe = compat_process._windows_open_pipe(kernel)
        kernel.SetHandleInformation.argtypes = [w.HANDLE, w.DWORD, w.DWORD]
        kernel.SetHandleInformation.restype = w.BOOL
        kernel.GetHandleInformation.argtypes = [w.HANDLE, ctypes.POINTER(w.DWORD)]
        kernel.GetHandleInformation.restype = w.BOOL
        fd = pipe.writer
        raw = msvcrt.get_osfhandle(fd)
        protected = False
        try:
            self.assertTrue(kernel.SetHandleInformation(raw, 2, 2))
            protected = True
            with self.assertRaises(compat_process.ToolFailure) as caught:
                pipe.close_writer()
            self.assertTrue(caught.exception.cleanup_failed)
            self.assertEqual(pipe.writer_state, 'retired')
            self.assertIsNone(pipe.writer)
            self.assertEqual(pipe.writer_handle, raw)
            with self.assertRaises(OSError): msvcrt.get_osfhandle(fd)
            compat_process._retain_io(pipe)
            time.sleep(.05)
            with compat_process._OWNED_IO_LOCK:
                self.assertIn(pipe, compat_process._OWNED_IO)
            unrelated_r, unrelated_w = os.pipe()
            try:
                self.assertTrue(kernel.SetHandleInformation(raw, 2, 0))
                protected = False
                compat_process._retry_owned_io(pipe)
                msvcrt.get_osfhandle(unrelated_r)
                msvcrt.get_osfhandle(unrelated_w)
            finally:
                os.close(unrelated_r); os.close(unrelated_w)
            flags = w.DWORD()
            self.assertFalse(kernel.GetHandleInformation(raw, ctypes.byref(flags)))
            self.assertIsNone(pipe.writer_witness)
            with compat_process._OWNED_IO_LOCK:
                self.assertNotIn(pipe, compat_process._OWNED_IO)
        finally:
            if protected: kernel.SetHandleInformation(raw, 2, 0)
            try: compat_process._retry_owned_io(pipe)
            finally: self.assertTrue(kernel.CloseHandle(job))

    def test_integrated_protected_writer_reports_cleanup_failure(self):
        import ctypes, msvcrt
        from ctypes import wintypes as w
        real_open = compat_process._windows_open_pipe
        pipes = []
        def open_pipe(kernel):
            pipe = real_open(kernel)
            pipes.append(pipe)
            if len(pipes) == 1:
                kernel.SetHandleInformation.argtypes = [w.HANDLE, w.DWORD, w.DWORD]
                kernel.SetHandleInformation.restype = w.BOOL
                raw = msvcrt.get_osfhandle(pipe.writer)
                if not kernel.SetHandleInformation(raw, 2, 2): raise RuntimeError('protect failed')
            return pipe
        try:
            with mock.patch('compat_process._windows_open_pipe', side_effect=open_pipe):
                with self.assertRaises(compat_process.ToolFailure) as caught:
                    run_bounded([sys.executable, '-c', "print('must-not-run')"], timeout=2)
            self.assertTrue(caught.exception.cleanup_failed)
            self.assertEqual(caught.exception.primary_category, 'tool capture failed')
            self.assertEqual(pipes[0].writer_state, 'retired')
            self.assertIsNone(pipes[0].writer)
            self.assertIsNone(pipes[1].handle)
            self.assertIsNone(pipes[1].event)
            with compat_process._OWNED_IO_LOCK:
                self.assertIn(pipes[0], compat_process._OWNED_IO)
        finally:
            if pipes:
                p = pipes[0]
                if p.writer_handle is not None:
                    p.kernel.SetHandleInformation(p.writer_handle, 2, 0)
                try: compat_process._retry_owned_io(p)
                except OSError: pass

    def test_retained_pipe_after_failed_job_close_has_outer_timeout(self):
        root = str(Path(__file__).resolve().parents[1] / 'tools')
        child = "import subprocess,sys,time;subprocess.Popen([sys.executable,'-c','import time;time.sleep(5)']);time.sleep(5)"
        script = ("import sys;sys.path.insert(0,%r);import compat_process as c;"
                  "real=c._windows_job;\n"
                  "class FailClose:\n"
                  " def __init__(self,k,j): self.k=k;self.j=j\n"
                  " def __getattr__(self,n): return getattr(self.k,n)\n"
                  " def CloseHandle(self,h): return 0 if h==self.j else self.k.CloseHandle(h)\n"
                  " def TerminateJobObject(self,h,x): return 0\n"
                  "def job():\n"
                  " k,j=real();return FailClose(k,j),j\n"
                  "c._windows_job=job\n"
                  "try: c.run_bounded([sys.executable,'-c',%r],timeout=.3)\n"
                  "except c.ToolFailure as e:\n"
                  " print(e.primary_category,e.cleanup_failed,e.containment_unconfirmed)\n"
                  "else: raise RuntimeError('unexpected success')\n") % (root, child)
        script = "import sys;print('inner-opt=%d' % sys.flags.optimize)\n" + script
        args = [sys.executable] + (['-O'] if sys.flags.optimize else []) + ['-c', script]
        result = subprocess.run(args, capture_output=True, timeout=8)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(('inner-opt=%d' % sys.flags.optimize).encode(), result.stdout)
        self.assertIn(b'tool timeout True True', result.stdout)


class WindowsJobSetupModelControls(unittest.TestCase):
    def test_failed_setup_and_raising_close_retain_job(self):
        kernel = mock.Mock()
        kernel.CreateJobObjectW.return_value = 123
        kernel.SetInformationJobObject.side_effect = OSError('PRIVATE_SETUP')
        kernel.CloseHandle.side_effect = OSError('PRIVATE_CLOSE')
        with mock.patch.object(compat_process.ctypes, 'WinDLL', return_value=kernel, create=True), \
             mock.patch('compat_process._retain_job') as retained:
            with self.assertRaises(compat_process.ToolFailure) as caught:
                compat_process._windows_job()
        retained.assert_called_once_with(kernel, 123)
        self.assertTrue(caught.exception.cleanup_failed)
        self.assertTrue(caught.exception.containment_unconfirmed)
        self.assertNotIn('PRIVATE', str(caught.exception))


class WindowsPipeSetupModelControls(unittest.TestCase):
    def test_failure_before_crt_adoption_closes_raw_owner(self):
        import types
        kernel = mock.Mock()
        kernel.CreateNamedPipeW.return_value = 111
        kernel.CreateFileW.return_value = 222
        kernel.CloseHandle.return_value = 1
        msvcrt = types.SimpleNamespace(open_osfhandle=mock.Mock())
        with mock.patch.dict(sys.modules, {'msvcrt': msvcrt}), \
             mock.patch('compat_process._duplicate_writer', side_effect=OSError('witness')):
            with self.assertRaises(OSError): compat_process._windows_open_pipe(kernel)
        msvcrt.open_osfhandle.assert_not_called()
        handles = [call.args[0] for call in kernel.CloseHandle.call_args_list]
        self.assertEqual(handles.count(222), 1)
        self.assertEqual(handles.count(111), 1)

    def test_retired_crt_slot_is_never_retried_and_raw_is_identity_checked(self):
        import types
        kernel = mock.Mock()
        kernel.CloseHandle.return_value = 1
        pipe = object.__new__(compat_process._WindowsPipe)
        pipe.kernel = kernel
        pipe.close_lock = __import__('threading').RLock()
        pipe.writer, pipe.writer_borrowed, pipe.writer_witness = 444, 222, 555
        pipe.writer_handle, pipe.writer_state, pipe.writer_retry = None, 'crt', False
        msvcrt = types.SimpleNamespace(get_osfhandle=mock.Mock(side_effect=OSError('retired')))
        with mock.patch.dict(sys.modules, {'msvcrt': msvcrt}), \
             mock.patch('compat_process.os.close', side_effect=OSError('PRIVATE_CLOSE')) as closefd:
            with self.assertRaises(compat_process.ToolFailure) as caught: pipe.close_writer()
            self.assertTrue(caught.exception.cleanup_failed)
            self.assertNotIn('PRIVATE_CLOSE', str(caught.exception))
            self.assertEqual(pipe.writer_state, 'retired')
            self.assertIsNone(pipe.writer)
            self.assertEqual(pipe.writer_handle, 222)
            with mock.patch('compat_process._same_writer', return_value=True):
                pipe.close_writer()
            closefd.assert_called_once_with(444)
        self.assertEqual([c.args[0] for c in kernel.CloseHandle.call_args_list], [222, 555])
        self.assertEqual(pipe.writer_state, 'closed')
        self.assertIsNone(pipe.writer_witness)

    def test_retired_unknown_identity_is_never_closed(self):
        import types
        kernel = mock.Mock()
        pipe = object.__new__(compat_process._WindowsPipe)
        pipe.kernel = kernel
        pipe.close_lock = __import__('threading').RLock()
        pipe.writer, pipe.writer_borrowed, pipe.writer_witness = None, None, 555
        pipe.writer_handle, pipe.writer_state, pipe.writer_retry = 222, 'retired', False
        with mock.patch.dict(sys.modules, {'msvcrt': types.SimpleNamespace()}), \
             mock.patch('compat_process._same_writer', return_value=False):
            with self.assertRaises(compat_process.ToolFailure): pipe.close_writer()
        kernel.CloseHandle.assert_not_called()
        self.assertEqual(pipe.writer_handle, 222)
        self.assertEqual(pipe.writer_witness, 555)

    def test_interrupt_during_connect_retains_pending_storage(self):
        import types
        kernel = mock.Mock()
        kernel.CreateNamedPipeW.return_value = 111
        kernel.CreateFileW.return_value = 222
        kernel.CreateEventW.return_value = 333
        kernel.ConnectNamedPipe.side_effect = KeyboardInterrupt('FIRST')
        kernel.CancelIoEx.return_value = 1
        msvcrt = types.SimpleNamespace(open_osfhandle=lambda handle, flags: 444)
        with mock.patch.dict(sys.modules, {'msvcrt': msvcrt}), \
             mock.patch.object(compat_process.os, 'O_BINARY', 0, create=True), \
             mock.patch('compat_process.os.close') as closefd, \
             mock.patch('compat_process._duplicate_writer', return_value=555), \
             mock.patch('compat_process._retain_io') as retained:
            with self.assertRaisesRegex(KeyboardInterrupt, 'FIRST'):
                compat_process._windows_open_pipe(kernel)
        closefd.assert_called_once_with(444)
        retained.assert_called_once()
        pipe = retained.call_args.args[0]
        self.assertTrue(pipe.pending)
        self.assertIsNotNone(pipe.overlap)
        self.assertEqual(pipe.handle, 111)
        self.assertEqual(pipe.event, 333)
        kernel.CancelIoEx.assert_called_once()


class ReapOwnershipControls(unittest.TestCase):
    def test_async_reap_retains_owned_object_until_wait_completes(self):
        import threading
        entered, release = threading.Event(), threading.Event()
        process = mock.Mock()
        def wait():
            entered.set()
            if not release.wait(2): raise RuntimeError('test timeout')
            return 0
        process.wait.side_effect = wait
        compat_process._retain_reap(process)
        self.addCleanup(release.set)
        self.assertTrue(entered.wait(1))
        with compat_process._OWNED_REAP_LOCK:
            self.assertIn(process, compat_process._OWNED_REAP)
        release.set()
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            with compat_process._OWNED_REAP_LOCK:
                if process not in compat_process._OWNED_REAP:
                    break
            time.sleep(.01)
        else:
            self.fail('successful reaper did not release ownership')


@unittest.skipIf(os.name == 'nt', 'POSIX anchor controls')
class PosixSupervisorControls(unittest.TestCase):
    def run_tool(self, source, **kwargs):
        return run_bounded([sys.executable, '-c', source], **kwargs)

    def test_anchor_never_reaped_before_single_group_signal(self):
        real_popen = subprocess.Popen
        real_killpg = os.killpg
        state = {'signals': 0, 'waits': 0}
        def spawn(*args, **kwargs):
            proc = real_popen(*args, **kwargs)
            original_wait = proc.wait
            def wait(*args, **kwargs):
                self.assertEqual(state['signals'], 1)
                state['waits'] += 1
                return original_wait(*args, **kwargs)
            proc.wait = wait
            proc.poll = mock.Mock(side_effect=AssertionError('anchor polled'))
            return proc
        def killpg(pid, sig):
            self.assertEqual(state['waits'], 0)
            state['signals'] += 1
            return real_killpg(pid, sig)
        with mock.patch('compat_process.subprocess.Popen', side_effect=spawn), \
             mock.patch('compat_process.os.killpg', side_effect=killpg):
            self.assertEqual(self.run_tool("print('ok')"), b'ok\n')
        self.assertEqual(state, {'signals': 1, 'waits': 1})

    def test_permission_error_retains_primary_and_reaps_anchor(self):
        actual = []
        real_popen = subprocess.Popen
        def spawn(*args, **kwargs):
            proc = real_popen(*args, **kwargs)
            actual.append(proc)
            return proc
        with mock.patch('compat_process.subprocess.Popen', side_effect=spawn), \
             mock.patch('compat_process.os.killpg', side_effect=PermissionError('PRIVATE')):
            with self.assertRaises(compat_process.ToolFailure) as caught:
                self.run_tool("import os;os.write(2,b'x'*8193)", stdout_limit=8192)
        self.assertEqual(caught.exception.primary_category, 'tool output limit exceeded')
        self.assertTrue(caught.exception.cleanup_failed)
        self.assertTrue(caught.exception.containment_unconfirmed)
        self.assertNotIn('PRIVATE', str(caught.exception))
        self.assertIsNotNone(actual[0].returncode)
        self.assertTrue(actual[0].stdout.closed)
        self.assertTrue(actual[0].stderr.closed)

    def test_permission_error_on_success_never_returns_output(self):
        with mock.patch('compat_process.os.killpg', side_effect=PermissionError('PRIVATE')):
            with self.assertRaises(compat_process.ToolFailure) as caught:
                self.run_tool("print('ok')")
        self.assertTrue(caught.exception.cleanup_failed)
        self.assertTrue(caught.exception.containment_unconfirmed)
        self.assertIsNone(caught.exception.primary_category)
        self.assertNotIn('PRIVATE', str(caught.exception))

    def test_wait_failure_and_both_closes_are_sanitized(self):
        actual = []
        real_popen = subprocess.Popen
        def spawn(*args, **kwargs):
            proc = real_popen(*args, **kwargs)
            actual.append(proc)
            real_wait = proc.wait
            def bad_wait(*args, **kwargs):
                real_wait(*args, **kwargs)
                raise subprocess.TimeoutExpired('PRIVATE_ARGV', 1)
            proc.wait = bad_wait
            return proc
        with mock.patch('compat_process.subprocess.Popen', side_effect=spawn):
            with self.assertRaises(compat_process.ToolFailure) as caught:
                self.run_tool("print('ok')")
        self.assertNotIn('PRIVATE_ARGV', str(caught.exception))
        self.assertTrue(caught.exception.cleanup_failed)
        self.assertTrue(actual[0].stdout.closed)
        self.assertTrue(actual[0].stderr.closed)

    def test_wait_first_interrupt_retries_without_second_signal(self):
        real_popen = subprocess.Popen
        real_killpg = os.killpg
        processes, calls = [], []
        def spawn(*args, **kwargs):
            proc = real_popen(*args, **kwargs)
            processes.append(proc)
            original = proc.wait
            def wait(*args, **kwargs):
                calls.append('wait')
                if calls.count('wait') == 1:
                    raise KeyboardInterrupt('FIRST')
                return original(*args, **kwargs)
            proc.wait = wait
            return proc
        def killpg(pid, sig):
            calls.append('killpg')
            return real_killpg(pid, sig)
        with mock.patch('compat_process.subprocess.Popen', side_effect=spawn), \
             mock.patch('compat_process.os.killpg', side_effect=killpg):
            with self.assertRaisesRegex(KeyboardInterrupt, 'FIRST'):
                self.run_tool("print('ok')")
        self.assertEqual(calls, ['killpg', 'wait', 'wait'])
        self.assertIsNotNone(processes[0].returncode)
        self.assertTrue(processes[0].stdout.closed)
        self.assertTrue(processes[0].stderr.closed)

    def test_repeated_wait_interrupt_retains_owner_after_budget(self):
        real_popen = subprocess.Popen
        processes, retained = [], []
        def spawn(*args, **kwargs):
            proc = real_popen(*args, **kwargs)
            processes.append(proc)
            proc.wait = mock.Mock(side_effect=KeyboardInterrupt('FIRST'))
            return proc
        def retain(proc):
            retained.append(proc)
        real_clock = time.monotonic
        ticks = [real_clock()]
        def clock():
            ticks[0] += .3
            return ticks[0]
        with mock.patch('compat_process.subprocess.Popen', side_effect=spawn), \
             mock.patch('compat_process._retain_reap', side_effect=retain), \
             mock.patch('compat_process.time.monotonic', side_effect=clock):
            with self.assertRaisesRegex(KeyboardInterrupt, 'FIRST'):
                self.run_tool("print('ok')", timeout=30)
        self.assertEqual(retained, processes)
        self.assertTrue(processes[0].stdout.closed)
        self.assertTrue(processes[0].stderr.closed)
        # Test owns this real child and reaps it explicitly after the fake.
        subprocess.Popen.wait(processes[0], timeout=5)

    def test_interrupt_during_group_signal_still_reaps_and_closes(self):
        real_popen = subprocess.Popen
        processes = []
        def spawn(*args, **kwargs):
            proc = real_popen(*args, **kwargs)
            processes.append(proc)
            return proc
        with mock.patch('compat_process.subprocess.Popen', side_effect=spawn), \
             mock.patch('compat_process.os.killpg', side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                self.run_tool("print('ok')")
        self.assertIsNotNone(processes[0].returncode)
        self.assertTrue(processes[0].stdout.closed)
        self.assertTrue(processes[0].stderr.closed)

    def test_unexpected_close_exception_does_not_skip_second_close(self):
        real_popen = subprocess.Popen
        closes = []
        class Stream:
            def __init__(self, wrapped, index):
                self.wrapped, self.index = wrapped, index
            def fileno(self):
                return self.wrapped.fileno()
            def close(self):
                closes.append(self.index)
                self.wrapped.close()
                if self.index == 0:
                    raise RuntimeError('PRIVATE')
        def spawn(*args, **kwargs):
            proc = real_popen(*args, **kwargs)
            proc.stdout = Stream(proc.stdout, 0)
            proc.stderr = Stream(proc.stderr, 1)
            return proc
        with mock.patch('compat_process.subprocess.Popen', side_effect=spawn):
            with self.assertRaises(compat_process.ToolFailure) as caught:
                self.run_tool("print('ok')")
        self.assertEqual(closes, [0, 1])
        self.assertNotIn('PRIVATE', str(caught.exception))
        self.assertTrue(caught.exception.cleanup_failed)

    def test_other_group_signal_errors_remain_sanitized(self):
        for error in (ProcessLookupError('PRIVATE'), OSError('PRIVATE')):
            with self.subTest(error=type(error).__name__), \
                 mock.patch('compat_process.os.killpg', side_effect=error):
                with self.assertRaises(compat_process.ToolFailure) as caught:
                    self.run_tool("print('ok')")
                self.assertTrue(caught.exception.containment_unconfirmed)
                self.assertNotIn('PRIVATE', str(caught.exception))

    def test_individual_close_failure_still_attempts_both_streams(self):
        real_popen = subprocess.Popen
        for broken in (0, 1):
            closes = []
            class Stream:
                def __init__(self, wrapped, index):
                    self.wrapped, self.index = wrapped, index
                def fileno(self):
                    return self.wrapped.fileno()
                def close(self):
                    closes.append(self.index)
                    self.wrapped.close()
                    if self.index == broken:
                        raise OSError('PRIVATE')
            def spawn(*args, **kwargs):
                proc = real_popen(*args, **kwargs)
                proc.stdout = Stream(proc.stdout, 0)
                proc.stderr = Stream(proc.stderr, 1)
                return proc
            with self.subTest(broken=broken), mock.patch('compat_process.subprocess.Popen', side_effect=spawn):
                with self.assertRaises(compat_process.ToolFailure) as caught:
                    self.run_tool("print('ok')")
            self.assertEqual(closes, [0, 1])
            self.assertTrue(caught.exception.cleanup_failed)
            self.assertNotIn('PRIVATE', str(caught.exception))

    def test_reader_failure_preserves_category_and_cleans_up(self):
        real_popen = subprocess.Popen
        real_read = os.read
        out_fd = []
        processes = []
        def spawn(*args, **kwargs):
            proc = real_popen(*args, **kwargs)
            out_fd.append(proc.stdout.fileno())
            processes.append(proc)
            return proc
        def read(fd, length):
            if fd in out_fd:
                raise OSError('PRIVATE')
            return real_read(fd, length)
        with mock.patch('compat_process.subprocess.Popen', side_effect=spawn), \
             mock.patch('compat_process.os.read', side_effect=read):
            with self.assertRaises(compat_process.ToolFailure) as caught:
                self.run_tool("print('ok')")
        self.assertEqual(caught.exception.primary_category, 'tool capture failed')
        self.assertTrue(processes[0].stdout.closed)
        self.assertTrue(processes[0].stderr.closed)
        self.assertIsNotNone(processes[0].returncode)
        self.assertNotIn('PRIVATE', str(caught.exception))

    def test_anchor_launch_failure_is_sanitized(self):
        with mock.patch('compat_process.subprocess.Popen', side_effect=OSError('PRIVATE')):
            with self.assertRaises(compat_process.ToolFailure) as caught:
                self.run_tool('pass')
        self.assertEqual(str(caught.exception), 'tool launch failed')

    def test_unexpected_supervisor_death_is_failure(self):
        real_popen = subprocess.Popen
        def spawn(*args, **kwargs):
            proc = real_popen(*args, **kwargs)
            os.kill(proc.pid, 9)
            return proc
        with mock.patch('compat_process.subprocess.Popen', side_effect=spawn):
            with self.assertRaises(compat_process.ToolFailure):
                self.run_tool('pass', timeout=2)

    def test_supervisor_exit_after_status_is_not_success(self):
        real_popen = subprocess.Popen
        def spawn(args, **kwargs):
            fd = int(args[4])
            descendant = "import os,time;time.sleep(.3);os.close(%d);os.close(1);os.close(2);time.sleep(3)" % fd
            code = ("import os,subprocess,sys,struct;"
                    "subprocess.Popen([sys.executable,'-c',%r],pass_fds=(%d,));"
                    "os.write(%d,b'R'+struct.pack('!i',0));os.close(%d);os.close(1);os.close(2)") % (descendant, fd, fd, fd)
            return real_popen([sys.executable, '-c', code], **kwargs)
        with mock.patch('compat_process.subprocess.Popen', side_effect=spawn):
            with self.assertRaises(compat_process.ToolFailure) as caught:
                self.run_tool('pass', timeout=2)
        self.assertTrue(caught.exception.containment_unconfirmed)

    def test_supervisor_sigkill_after_status_is_not_success(self):
        real_popen = subprocess.Popen
        def spawn(args, **kwargs):
            fd = int(args[4])
            child = "import os,time;time.sleep(.3);os.close(1);os.close(2);time.sleep(3)"
            code = ("import os,subprocess,sys,struct,signal;"
                    "subprocess.Popen([sys.executable,'-c',%r]);"
                    "os.write(%d,b'R'+struct.pack('!i',0));os.close(%d);"
                    "os.kill(os.getpid(),signal.SIGKILL)") % (child, fd, fd)
            return real_popen([sys.executable, '-c', code], **kwargs)
        with mock.patch('compat_process.subprocess.Popen', side_effect=spawn):
            with self.assertRaises(compat_process.ToolFailure):
                self.run_tool('pass', timeout=2)

    def test_control_protocol_rejects_malformed_and_oversize(self):
        real_popen = subprocess.Popen
        for data in (b'', b'Q', b'Rxx', b'R' + b'x'*8):
            def spawn(args, **kwargs):
                fd = int(args[4])
                code = ("import os,signal;os.write(%d,%r);os.close(%d);"
                        "os.close(1);os.close(2);signal.pause()") % (fd, data, fd)
                return real_popen([sys.executable, '-c', code], **kwargs)
            with self.subTest(data=data), mock.patch('compat_process.subprocess.Popen', side_effect=spawn):
                with self.assertRaisesRegex(compat_process.ToolFailure, 'containment unavailable'):
                    self.run_tool('pass', timeout=2)

    def test_control_fd_not_inherited_by_tool(self):
        script = ("import os; found=[]\n"
                  "for fd in range(3,256):\n"
                  " try: os.fstat(fd);found.append(fd)\n"
                  " except OSError: pass\n"
                  "print(found)")
        self.assertEqual(self.run_tool(script), b'[]\n')

    def test_failed_group_signal_does_not_block_on_inherited_pipe(self):
        # Run under an outer hard timeout: a BufferedReader.close regression can
        # otherwise hang the suite even after its reader join timed out.
        root = str(Path(__file__).resolve().parents[1] / 'tools')
        script = ("import sys,time;sys.path.insert(0,%r);import compat_process as c;"
                  "c.os.killpg=lambda *a: (_ for _ in ()).throw(PermissionError());"
                  "start=time.monotonic();\n"
                  "try: c.run_bounded([sys.executable,'-c',%r],timeout=.1)\n"
                  "except c.ToolFailure as e: print(e.primary_category,e.cleanup_failed,round(time.monotonic()-start,1))\n") % (root, "import subprocess,sys;subprocess.Popen([sys.executable,'-c','import time;time.sleep(3)'])")
        result = subprocess.run([sys.executable, '-c', script], capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b'tool timeout True', result.stdout)


class IndependentReviewControls(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
  self.root=SyntheticTree(pathlib.Path(self.tmp.name)).write()
  a,p=cq.inventory(self.root);self.matrix=cq.build_matrix(a,p)
 def test_valid_control(self): cq.validate_matrix(self.matrix,self.root)
 def test_forged_identity_metrics_refused(self):
  r=self.matrix['rows'][1]
  for c in r['comparisons']:
   c['metrics']={'psnr_db':None,'max_error':0,'mismatches':0,'samples':32768};c['verdict']='IDENTICAL'
  r['status']='QUALIFIED_IDENTITY'
  with self.assertRaisesRegex(ValueError,'comparison metrics mismatch'):cq.validate_matrix(self.matrix,self.root)
 def test_foreign_packet_capture_refused(self):
  self.matrix['rows'][0]['comparisons'][0]['capture']['artifact']='standalone/gradient.p10'
  with self.assertRaisesRegex(ValueError,'packet linkage mismatch'):cq.validate_matrix(self.matrix,self.root)
 def test_identity_bool_scope_refused(self):
  self.matrix['rows'][0]['scope']['gop']=True
  with self.assertRaisesRegex(ValueError,'invalid scope type'):cq.validate_matrix(self.matrix,self.root)
 def test_unsupported_direction_refused(self):
  self.matrix['rows'][0]['direction']='hypercine-packet-to-hypercine-decoder'
  with self.assertRaisesRegex(ValueError,'unmeasured compatibility direction'):cq.validate_matrix(self.matrix,self.root)
 def test_packet_manifest_commit_relabel_refused(self):
  self.matrix['rows'][0]['packet']['source_commit']='1'*40
  with self.assertRaisesRegex(ValueError,'packet source commit mismatch'):cq.validate_matrix(self.matrix,self.root)
 def local_row(self):
  r=self.matrix['rows'][1];c=copy.deepcopy(r['comparisons'][0]);c['basis']='local_decoder'
  c['decoder']={'id':'ffmpeg-cfhd','version':'recorded','binary_sha256':'1'*64}
  c['capture']={'artifact':'local/ffmpeg-cfhd/canonical.p10','sha256':'2'*64}
  c['metrics']={'psnr_db':None,'max_error':0,'mismatches':0,'samples':32768};c['verdict']='IDENTICAL'
  r['comparisons']=[c];r['status']='QUALIFIED_IDENTITY'
  return c
 def test_local_fake_role_domain_psnr_refused(self):
  for field,value,reason in [('decoder','not-ffmpeg','local decoder role'),('samples',1,'sample domain'),('psnr_db',{'arbitrary':'json'},'PSNR type')]:
   c=self.local_row()
   if field=='decoder':c['decoder']['id']=value
   else:c['metrics'][field]=value
   with self.assertRaisesRegex(ValueError,reason):cq.validate_matrix(self.matrix,self.root)
 def test_local_recorded_not_replay_qualified(self):
  self.local_row();out=cq.validate_matrix(self.matrix,self.root);r=out['rows'][1]
  self.assertEqual(r['historical_status'],'QUALIFIED_IDENTITY')
  self.assertEqual(r['status'],'NOT_OBSERVABLE')
  self.assertEqual(r['replay_status'],'NOT_REPLAY_QUALIFIED')
  self.assertEqual(r['comparisons'][0]['replay_status'],'NOT_REPLAY_QUALIFIED')
 def test_cli_nonobject_evidence_sanitized(self):
  f=self.root/'PRIVATE_REVIEW_SENTINEL.json';f.write_text('[]')
  self.cli_refused(f)
 def cli_refused(self,f):
  command=[sys.executable]+(['-O'] if sys.flags.optimize else [])+[str(Path(cq.__file__)), '--verify',str(f),'--fixtures',str(self.root)]
  p=subprocess.run(command,capture_output=True,text=True,timeout=10)
  self.assertEqual(p.returncode,2,p.stderr)
  self.assertEqual(p.stdout,'')
  self.assertEqual(p.stderr,'compat qualification failed: invalid evidence or bounded tool failure\n')
 def test_nested_manifest_sanitized(self):
  for bad in [[],{'files':[None]}, {'samples':{'bad':'shape'}}, {'files':['private']}]:
   (self.root/'standalone/manifest.json').write_text(json.dumps(bad))
   f=self.root/'PRIVATE_REVIEW_SENTINEL.json';f.write_text(json.dumps(self.matrix))
   self.cli_refused(f)
 def test_psnr_semantics_and_types(self):
  for value in [True,{},[],float('nan'),float('inf'),1.0]:
   c=self.local_row();c['metrics']['psnr_db']=value
   with self.assertRaisesRegex(ValueError,'PSNR'):cq.validate_matrix(self.matrix,self.root)
 def test_known_manifest_provenance(self):
  f=self.root/'standalone/manifest.json';d=json.loads(f.read_text());d['encoder_commit']='a'*40;f.write_text(json.dumps(d))
  a,p=cq.inventory(self.root);m=cq.build_matrix(a,p);cq.validate_matrix(m,self.root)
  m['rows'][0]['packet']['source_commit']=None
  with self.assertRaisesRegex(ValueError,'source commit mismatch'):cq.validate_matrix(m,self.root)
 def test_local_difference_scope_refused(self):
  c=self.local_row();c['metrics']={'psnr_db':90.0,'max_error':1,'mismatches':1,'samples':32768};c['verdict']='DIFFERENT'
  self.matrix['rows'][1]['status']='MEASURED_DIFFERENCE'
  self.matrix['rows'][1]['scope']['geometry']='1920x1080'
  with self.assertRaisesRegex(ValueError,'outside the admitted scope'):cq.validate_matrix(self.matrix,self.root)
 def test_cli_machine_readable_replay(self):
  self.local_row();f=self.root/'matrix.json';f.write_text(json.dumps(self.matrix))
  command=[sys.executable]+(['-O'] if sys.flags.optimize else [])+[str(Path(cq.__file__)), '--verify',str(f),'--fixtures',str(self.root)]
  p=subprocess.run(command,capture_output=True,text=True,timeout=10)
  self.assertEqual(p.returncode,0,p.stderr);self.assertEqual(p.stderr,'')
  report=json.loads(p.stdout);r=report['rows'][1]
  self.assertEqual(r['historical_status'],'QUALIFIED_IDENTITY')
  self.assertEqual(r['replay_qualification'],'NOT_OBSERVABLE')
  self.assertEqual(r['comparisons'],['NOT_REPLAY_QUALIFIED'])
  self.assertNotIn(str(self.root),p.stdout)
 def test_cli_nested_row_shapes_sanitized(self):
  mutations=[('rows',[[]]),('rows',[{'id':'bad'}])]
  for field,value in mutations:
   m=copy.deepcopy(self.matrix);m[field]=value
   f=self.root/'PRIVATE_REVIEW_SENTINEL.json';f.write_text(json.dumps(m));self.cli_refused(f)
  for field in ['decoder','capture','against','metrics']:
   m=copy.deepcopy(self.matrix);m['rows'][0]['comparisons'][0][field]=[]
   f=self.root/'PRIVATE_REVIEW_SENTINEL.json';f.write_text(json.dumps(m));self.cli_refused(f)
  for field in ['artifact','sha256','role','producer','source_commit']:
   m=copy.deepcopy(self.matrix);m['rows'][0]['packet'][field]=[]
   f=self.root/'PRIVATE_REVIEW_SENTINEL.json';f.write_text(json.dumps(m));self.cli_refused(f)
 def test_producer_relabel_refused(self):
  self.matrix['rows'][0]['packet']['producer']='GoPro encoder'
  with self.assertRaisesRegex(ValueError,'packet producer mismatch'):cq.validate_matrix(self.matrix,self.root)
 def test_finite_mixed_output(self):
  self.assertEqual(run_bounded([sys.executable,'-c',"import os;os.write(1,b'x'*8192);os.write(2,b'y'*8192)"],stdout_limit=8192),b'x'*8192)
 def test_stderr_over_limit(self):
  with self.assertRaisesRegex(ValueError,'output limit'):run_bounded([sys.executable,'-c',"import os;os.write(2,b'x'*8193)"],stdout_limit=8192)
 def test_closed_pipe_descendant_cleanup(self):
  f=self.root/'sentinel';child="import time,pathlib;time.sleep(.4);pathlib.Path(%r).write_text('x')"%str(f)
  run_bounded([sys.executable,'-c',"import subprocess,sys;subprocess.Popen([sys.executable,'-c',%r],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)"%child],timeout=1)
  time.sleep(.6);self.assertFalse(f.exists())


class Metrics(unittest.TestCase):
    def test_identity_is_sample_level_not_byte_level(self):
        result = cq.diff_metrics(FRAME, FRAME, cq.SAMPLE_COUNT)
        self.assertEqual(result, {'psnr_db': None, 'max_error': 0,
                                  'mismatches': 0, 'samples': cq.SAMPLE_COUNT})

    def test_single_code_value_difference_is_one_mismatch(self):
        altered = bytearray(FRAME)
        altered[0] = (altered[0] + 1) % 256
        result = cq.diff_metrics(bytes(altered), FRAME, cq.SAMPLE_COUNT)
        self.assertEqual(result['mismatches'], 1)
        self.assertEqual(result['max_error'], 1)

    def test_length_mismatch_is_refused(self):
        with self.assertRaises(ValueError) as caught:
            cq.diff_metrics(FRAME[:-1], FRAME, cq.SAMPLE_COUNT)
        self.assertEqual(str(caught.exception), 'p10 buffer length differs')

    def test_wrong_sample_count_is_refused(self):
        with self.assertRaises(ValueError) as caught:
            cq.diff_metrics(FRAME, FRAME, 65536)
        self.assertEqual(str(caught.exception), 'unexpected p10 sample count')


if __name__ == '__main__':
    unittest.main()
