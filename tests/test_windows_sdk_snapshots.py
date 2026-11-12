# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Deterministic verifier I/O races on tiny synthetic evidence, never a corpus.

The packet builder targets only the verifier's declared bounded parser dialect.
It is not an SDK encoder or a decoder/conformance oracle. Mutation callbacks run
at actual hashing, descriptor-read and publication boundaries, without sleeps.
"""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import p6_output_paths as paths

SPEC = importlib.util.spec_from_file_location('snapshot_verifier', TOOLS / 'verify_windows_sdk_corpus.py')
v = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = v
SPEC.loader.exec_module(v)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def packet(ordinal=0, quality=1, marker=0x31):
    def tag(number, value):
        return struct.pack('>hH', number, value)

    records = b'UFRM' + struct.pack('<I', (ord('L') << 24) | 4) + struct.pack('<I', ordinal)
    data = tag(1, 9) + tag(-69, ordinal + 1) + tag(-80, quality)
    data += tag(-0x4002, len(records) // 4) + records
    for channel in range(3):
        if channel:
            data += tag(62, channel)
        for band in range(10):
            if band == 0:
                data += tag(25, 0) + tag(27, 1) + tag(28, 2)
                body = tag(4, 0x0f0f) + bytes([marker]) * 4 + tag(4, 0x1b4b)
            else:
                data += (tag(48, 1) + tag(51, band) + tag(49, 1) + tag(50, 2) +
                         tag(53, 4) + tag(54, 2))
                body = tag(55, 0) + bytes([marker]) * 4 + tag(56, 0)
            data += tag(-0x2000, len(body) // 4) + body
    return data


def write(root, name, data):
    target = root / name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    return target


def index(root):
    lines = []
    for target in sorted(root.rglob('*')):
        if target.is_file() and target.name != 'TRANSFER_SHA256.tsv':
            data = target.read_bytes()
            lines.append('%s\t%d\t%s\n' % (target.relative_to(root).as_posix(), len(data), digest(data)))
    data = ''.join(lines).encode('utf-8')
    write(root, 'TRANSFER_SHA256.tsv', data)
    return len(lines), digest(data)


class SnapshotIntegrity(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name).resolve()
        self.root = self.base / 'corpus'
        self.root.mkdir()
        self.data = packet()
        self.name = 'packets/frame.cfhd'
        self.target = write(self.root, self.name, self.data)
        write(self.root, 'inputs/frame.bgra', b'BGRA')
        write(self.root, 'manifest.json', b'{"note":"original"}')
        write(self.root, 'low/trace.tsv', b'00\t4\t0\t0\t0\tframe_0.cfhd\n')
        self.count, self.pin = index(self.root)
        self.frame = {'input': 'inputs/frame.bgra', 'input_byte_length': 4,
                      'input_sha256': digest(b'BGRA'), 'sdk_packet': self.name,
                      'sdk_byte_length': len(self.data), 'sdk_sha256': digest(self.data)}

    def rows(self):
        return v.verify_index(self.root, self.count, self.pin)

    def checked(self, rows):
        return v.checked_frame(self.root, rows, self.frame, 'sdk_packet')

    @contextlib.contextmanager
    def probe_reads(self, leaf, before_first=None, chunk=None, force_eof=False):
        """Wrap the real opened descriptor, retaining real fstat/EOF semantics."""
        original = paths.Directory.file
        state = {'requests': [], 'returned': 0, 'opened': 0}

        @contextlib.contextmanager
        def controlled(directory, name, create=False):
            with original(directory, name, create=create) as stream:
                if create or name != leaf:
                    yield stream
                    return
                state['opened'] += 1

                class Reader:
                    first = True

                    def fileno(inner):
                        return stream.fileno()

                    def read(inner, size=-1):
                        self.assertGreaterEqual(size, 0, 'verifier issued an unbounded read')
                        state['requests'].append(size)
                        if inner.first:
                            inner.first = False
                            if before_first:
                                before_first()
                        data = b'' if force_eof else stream.read(min(size, chunk) if chunk else size)
                        state['returned'] += len(data)
                        return data

                yield Reader()

        with patch.object(paths.Directory, 'file', controlled):
            yield state

    def test_valid_packet_uses_actual_parser_and_exact_bytes(self):
        rows = self.rows()
        with patch.object(v, 'parse_packet', wraps=v.parse_packet) as parser:
            data, parsed = self.checked(rows)
        self.assertEqual(data, self.data)
        parser.assert_called_once_with(self.data)
        self.assertEqual(len(parsed['bands']), 30)
        self.assertEqual(v.observe_order(parsed, 0, 'low')['ufrm'], 0)

    def test_same_length_packet_swap_after_index_is_rejected_before_parser(self):
        rows = self.rows()
        changed = packet(marker=0x32)
        self.assertEqual(len(changed), len(self.data))
        self.target.write_bytes(changed)
        with patch.object(v, 'parse_packet', wraps=v.parse_packet) as parser:
            with self.assertRaises(ValueError):
                self.checked(rows)
        parser.assert_not_called()
        self.assertEqual(self.target.read_bytes(), changed)

    def test_index_hash_then_parse_swap_cannot_authorize_replacement(self):
        original_index = (self.root / 'TRANSFER_SHA256.tsv').read_bytes()
        changed = packet(marker=0x32)
        changed_index = original_index.replace(digest(self.data).encode(), digest(changed).encode())
        self.assertEqual(len(original_index), len(changed_index))
        real_sha256 = hashlib.sha256
        fired = []

        class HashAtBoundary:
            def __init__(inner, data=b'', *args, **kwargs):
                inner.hash = real_sha256(data, *args, **kwargs)
                inner.bytes = bytearray(data)

            def update(inner, data):
                inner.bytes.extend(data)
                inner.hash.update(data)

            def hexdigest(inner):
                value = inner.hash.hexdigest()
                if bytes(inner.bytes) == original_index and not fired:
                    fired.append(True)
                    (self.root / 'TRANSFER_SHA256.tsv').write_bytes(changed_index)
                    self.target.write_bytes(changed)
                return value

        with patch.object(v.hashlib, 'sha256', HashAtBoundary):
            with self.assertRaises(ValueError):
                v.verify_index(self.root, self.count, self.pin)
        self.assertEqual(fired, [True], 'index-hash mutation boundary was not exercised')

    def test_manifest_and_trace_replacements_require_index_binding(self):
        rows = self.rows()
        for name in ('manifest.json', 'low/trace.tsv'):
            with self.subTest(name=name):
                target = self.root / name
                before = target.read_bytes()
                target.write_bytes(bytes([before[0] ^ 1]) + before[1:])
                with self.assertRaises(ValueError):
                    v.read_verified(self.root, name, rows, len(before))
                target.write_bytes(before)
                self.assertEqual(v.read_verified(self.root, name, rows, len(before)), before)

    def test_short_reads_accumulate_and_exact_limit_probes_eof(self):
        rows = self.rows()
        with self.probe_reads('frame.cfhd', chunk=7) as state:
            data = v.read_verified(self.root, self.name, rows, len(self.data))
        self.assertEqual(data, self.data)
        self.assertEqual(state['opened'], 1)
        self.assertGreater(len(state['requests']), 2)
        self.assertEqual(state['requests'][-1], 1)
        self.assertEqual(state['returned'], len(self.data))
        self.assertLessEqual(max(state['requests']), len(self.data))

    def test_growth_after_descriptor_stat_is_rejected_before_parser(self):
        rows = self.rows()

        def grow():
            with self.target.open('ab') as stream:
                stream.write(b'!')

        with self.probe_reads('frame.cfhd', before_first=grow) as state:
            with patch.object(v, 'parse_packet', wraps=v.parse_packet) as parser:
                with self.assertRaises(ValueError):
                    self.checked(rows)
        parser.assert_not_called()
        self.assertEqual(state['returned'], len(self.data) + 1)
        self.assertEqual(state['requests'][-1], 1)
        self.assertLessEqual(max(state['requests']), len(self.data))

    def test_inventory_hash_growth_stops_after_single_overflow_byte(self):
        def grow():
            with self.target.open('ab') as stream:
                stream.write(b'!' * 37)

        # Inventory hashing streams data separately from read_verified's byte
        # snapshot. Verify its read bound as well as its eventual rejection.
        with self.probe_reads('frame.cfhd', before_first=grow) as state:
            with self.assertRaises(ValueError):
                self.rows()
        self.assertEqual(state['opened'], 1)
        self.assertEqual(state['returned'], len(self.data) + 1)
        self.assertEqual(state['requests'][-1], 1)
        self.assertLessEqual(max(state['requests']), len(self.data))

    def test_truncation_after_descriptor_stat_is_rejected_before_parser(self):
        rows = self.rows()
        with self.probe_reads('frame.cfhd', before_first=lambda: self.target.write_bytes(b'cut')) as state:
            with patch.object(v, 'parse_packet', wraps=v.parse_packet) as parser:
                with self.assertRaises(ValueError):
                    self.checked(rows)
        parser.assert_not_called()
        self.assertEqual(state['returned'], 3)
        self.assertGreaterEqual(len(state['requests']), 2)

    def test_unexpected_empty_read_is_rejected(self):
        rows = self.rows()
        with self.probe_reads('frame.cfhd', force_eof=True) as state:
            with self.assertRaises(ValueError):
                v.read_verified(self.root, self.name, rows, len(self.data))
        self.assertEqual(len(state['requests']), 1)
        self.assertEqual(state['returned'], 0)

    def test_oversize_packet_is_refused_without_reading_it(self):
        rows = self.rows()
        with self.probe_reads('frame.cfhd') as state:
            with patch.object(v, 'MAX_PACKET_BYTES', len(self.data) - 1):
                with patch.object(v, 'parse_packet', wraps=v.parse_packet) as parser:
                    with self.assertRaises(ValueError):
                        self.checked(rows)
        parser.assert_not_called()
        self.assertEqual(state['requests'], [])

    def test_oversize_index_is_refused_without_reading_it(self):
        size = (self.root / 'TRANSFER_SHA256.tsv').stat().st_size
        with self.probe_reads('TRANSFER_SHA256.tsv') as state:
            with patch.object(v, 'MAX_INDEX_BYTES', size - 1):
                with self.assertRaises(ValueError):
                    self.rows()
        self.assertEqual(state['requests'], [])

    def test_inventory_repeated_name_is_rejected(self):
        # Directory iteration under concurrent changes need not be a stable
        # listing. A duplicate permitted name must not extend an unbounded walk
        # or silently count as a distinct verified inventory entry.
        real_scandir = os.scandir
        root_stat = self.root.stat()
        duplicated = []

        @contextlib.contextmanager
        def repeated_root(path):
            info = os.fstat(path) if isinstance(path, int) else os.stat(path)
            with real_scandir(path) as entries:
                if (info.st_dev, info.st_ino) != (root_stat.st_dev, root_stat.st_ino):
                    yield entries
                    return
                actual = list(entries)

                def repeat_once():
                    yield from actual
                    duplicated.append(True)
                    yield actual[0]

                yield repeat_once()

        with patch.object(v.os, 'scandir', repeated_root):
            with self.assertRaises(ValueError):
                self.rows()
        self.assertEqual(duplicated, [True], 'repeated directory entry was not consumed')

    @unittest.skipUnless(os.name == 'posix', 'POSIX descriptor-relative ancestor race')
    def test_intermediate_symlink_swap_before_open_is_rejected(self):
        rows = self.rows()
        outside = self.base / 'outside'
        write(outside, 'frame.cfhd', packet(marker=0x39))
        original_child = paths.Directory.child
        fired = []

        def replace_parent(directory, name, create=False):
            if name == 'packets' and not create and not fired:
                fired.append(True)
                (self.root / 'packets').rename(self.root / 'packets-held')
                (self.root / 'packets').symlink_to(outside, target_is_directory=True)
            return original_child(directory, name, create=create)

        with patch.object(paths.Directory, 'child', replace_parent):
            with patch.object(v, 'parse_packet', wraps=v.parse_packet) as parser:
                with self.assertRaises((OSError, ValueError)):
                    self.checked(rows)
        self.assertEqual(fired, [True])
        parser.assert_not_called()
        self.assertEqual((outside / 'frame.cfhd').read_bytes(), packet(marker=0x39))

    @unittest.skipUnless(os.name == 'posix', 'POSIX open-descriptor ancestor replacement')
    def test_intermediate_swap_after_open_never_reads_replacement(self):
        rows = self.rows()
        outside = self.base / 'outside'
        changed = packet(marker=0x39)
        write(outside, 'frame.cfhd', changed)
        original_child = paths.Directory.child
        fired = []

        def replace_after_pin(directory, name, create=False):
            child = original_child(directory, name, create=create)
            if name == 'packets' and not create and not fired:
                fired.append(True)
                (self.root / 'packets').rename(self.root / 'packets-held')
                (self.root / 'packets').symlink_to(outside, target_is_directory=True)
            return child

        with patch.object(paths.Directory, 'child', replace_after_pin):
            with patch.object(v, 'parse_packet', wraps=v.parse_packet) as parser:
                try:
                    data, _ = self.checked(rows)
                except (OSError, ValueError):
                    parser.assert_not_called()
                else:
                    self.assertEqual(data, self.data)
                    parser.assert_called_once_with(self.data)
        self.assertEqual(fired, [True])
        self.assertEqual((outside / 'frame.cfhd').read_bytes(), changed)


class VerifierBoundaries(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name).resolve()
        self.golden = self.base / 'golden'
        self.golden.mkdir()
        self.reports = self.base / 'reports'
        self.reports.mkdir()
        self.output = self.reports / 'evidence.json'
        self.tree = 'f' * 40

    def group_fixture(self, group='group-a-sequences', manifest=None):
        root = self.golden / v.WINDOWS / group
        root.mkdir(parents=True)
        if manifest is None:
            manifest = json.dumps({'sessions': [], 'upstream_tree': self.tree, 'note': 'first'}).encode()
        write(root, 'manifest.json', manifest)
        linux = self.golden / v.LINUX
        write(linux, 'provenance/sdk_packet_capture.cpp', b'// synthetic provenance\n')
        write(linux, 'manifest.json', json.dumps({
            'frozen_source_sha256': {},
            'capture_source_sha256': digest(b'// synthetic provenance\n'),
        }).encode())
        return root, {group: index(root), 'linux-sdk-gaps': index(linux)}

    def main(self, run=None, indexes=None):
        with contextlib.ExitStack() as stack:
            if run is not None:
                stack.enter_context(patch.object(v, 'run', side_effect=run))
            if indexes is not None:
                stack.enter_context(patch.object(v, 'INDEXES', indexes))
            stack.enter_context(patch.object(sys, 'argv', ['verify', '--golden-root', str(self.golden),
                                                          '--output', str(self.output)]))
            stdout, stderr = io.StringIO(), io.StringIO()
            stack.enter_context(contextlib.redirect_stdout(stdout))
            stack.enter_context(contextlib.redirect_stderr(stderr))
            result = v.main()
        self.assertNotIn(str(self.base), stdout.getvalue() + stderr.getvalue())
        return result, stdout.getvalue(), stderr.getvalue()

    @staticmethod
    def report(*unused):
        return {'indexes': {'synthetic': {'files_ok': 1}}, 'scope': 'synthetic publication boundary'}

    def swap_after_index(self, affected_root, target, changed):
        # The public helper preserves its original rows-only API; run also
        # retains the digest of the exact index snapshot for its report.
        boundary = '_verify_index_snapshot' if hasattr(v, '_verify_index_snapshot') else 'verify_index'
        original = getattr(v, boundary)
        fired = []

        def verify_then_swap(root, *args, **kwargs):
            rows = original(root, *args, **kwargs)
            path = Path(getattr(root, 'path', root))
            if path == affected_root and not fired:
                fired.append(True)
                target.write_bytes(changed)
            return rows

        return boundary, verify_then_swap, fired

    def test_run_rejects_manifest_swap_before_json_parser(self):
        root, indexes = self.group_fixture()
        manifest = root / 'manifest.json'
        changed = manifest.read_bytes().replace(b'first', b'other')
        boundary, swap, fired = self.swap_after_index(root, manifest, changed)
        with patch.object(v, 'INDEXES', indexes), patch.object(v, boundary, side_effect=swap):
            with patch.object(v.json, 'loads', wraps=json.loads) as parser:
                with self.assertRaises(ValueError):
                    v.run(self.golden, TOOLS.parent)
        self.assertEqual(fired, [True])
        parser.assert_not_called()

    def test_run_rejects_oversize_manifest_before_json_parser(self):
        root, indexes = self.group_fixture()
        maximum = (root / 'manifest.json').stat().st_size - 1
        with patch.object(v, 'INDEXES', indexes), patch.object(v, 'MAX_METADATA_BYTES', maximum):
            with patch.object(v.json, 'loads', wraps=json.loads) as parser:
                with self.assertRaises(ValueError):
                    v.run(self.golden, TOOLS.parent)
        parser.assert_not_called()

    def test_run_rejects_trace_swap_before_any_packet_parser(self):
        root, _ = self.group_fixture('group-c-sdk-state')
        sessions = []
        for profile, quality, amplitude in [('low', 1, 13), ('medium', 2, 15), ('high', 3, 10)]:
            frames, trace = [], []
            previous = None
            for ordinal in range(7):
                name = ('inputs/sequence-120f/frame_0006.bgra' if ordinal < 2 else
                        ('inputs/sequence-120f/frame_0000.bgra' if ordinal > 4 else
                         'inputs/hold-ramp/amplitude_%03d.bgra' % amplitude))
                write(root, name, b'BGRA')
                data = packet(ordinal, quality)
                sample_name = '%s/frame_%d.cfhd' % (profile, ordinal)
                write(root, sample_name, data)
                parsed = v.parse_packet(data)
                frames.append({'frame_index': ordinal, 'input': name, 'input_byte_length': 4,
                    'input_sha256': digest(b'BGRA'), 'sdk_packet': sample_name,
                    'sdk_byte_length': len(data), 'sdk_sha256': digest(data),
                    'previous_sdk_sample_bytes': previous,
                    'previous_bitrate_30fps': None if previous is None else previous * 8 * 30,
                    'packet_band_quant_scale': [{k: b[k] for k in ('channel', 'subband', 'quant_53', 'scale_54')}
                                                for b in parsed['bands']],
                    'sdk_overbitrate_after_encode': 0, 'sdk_rate_limiter_after_encode': 4,
                    'sdk_lastgopbitcount_after_encode': len(data) * 8})
                trace.append('00\t4\t%d\t%d\t%d\tframe_%d.cfhd\n' %
                             (len(data) * 8, ordinal, len(data), ordinal))
                previous = len(data)
            write(root, profile + '/trace.tsv', ''.join(trace).encode())
            sessions.append({'profile': profile, 'frames': frames, 'limit_bits_per_second': 1000000})
        write(root, 'manifest.json', json.dumps({'sessions': sessions, 'upstream_tree': self.tree}).encode())
        indexes = {'group-c-sdk-state': index(root), 'linux-sdk-gaps': index(self.golden / v.LINUX)}
        trace = root / 'low/trace.tsv'
        changed = trace.read_bytes().replace(b'00\t', b' 0\t', 1)
        boundary, swap, fired = self.swap_after_index(root, trace, changed)
        with patch.object(v, 'INDEXES', indexes), patch.object(v, boundary, side_effect=swap):
            with patch.object(v.subprocess, 'check_output', return_value=self.tree + '\n'):
                with patch.object(v, 'parse_packet', wraps=v.parse_packet) as parser:
                    with self.assertRaises(ValueError):
                        v.run(self.golden, TOOLS.parent)
        self.assertEqual(fired, [True])
        parser.assert_not_called()

    def test_deep_metadata_is_a_clean_cli_failure_without_report(self):
        # The C JSON parser may exceed the Python recursion limit. Ten thousand
        # levels exercise its nesting failure while still occupying only 20 KiB.
        data = b'[' * 10000 + b'0' + b']' * 10000
        _, indexes = self.group_fixture(manifest=data)
        with patch.object(v.json, 'loads', wraps=json.loads) as parser:
            code, stdout, stderr = self.main(indexes=indexes)
        parser.assert_called_once_with(data.decode('utf-8'))
        self.assertEqual(code, 1)
        self.assertNotIn('PASS', stdout)
        self.assertIn('FAIL corpus verification', stderr)
        self.assertNotIn('Traceback', stderr)
        self.assertFalse(self.output.exists())

    def test_report_success_publishes_complete_json(self):
        code, stdout, stderr = self.main(run=self.report)
        self.assertEqual(code, 0, stderr)
        self.assertIn('PASS integrity', stdout)
        self.assertEqual(json.loads(self.output.read_text()), self.report())

    def test_report_destination_created_during_run_is_preserved(self):
        sentinel = b'previous independently published evidence\n'

        def create_destination(*unused):
            self.output.write_bytes(sentinel)
            return self.report()

        code, stdout, _ = self.main(run=create_destination)
        self.assertEqual(code, 1)
        self.assertNotIn('PASS', stdout)
        self.assertEqual(self.output.read_bytes(), sentinel)

    @unittest.skipUnless(os.name == 'posix', 'POSIX symlink publication race')
    def test_report_destination_symlink_race_preserves_target(self):
        victim = self.base / 'retained.json'
        sentinel = b'previous evidence outside the destination\n'
        victim.write_bytes(sentinel)

        def create_symlink(*unused):
            self.output.symlink_to(victim)
            return self.report()

        code, _, _ = self.main(run=create_symlink)
        self.assertEqual(code, 1)
        self.assertTrue(self.output.is_symlink())
        self.assertEqual(victim.read_bytes(), sentinel)

    def test_destination_race_at_publication_preserves_other_writer(self):
        sentinel = b'concurrent completed report\n'
        operation = 'rename' if os.name == 'nt' else 'link'
        original = getattr(paths.os, operation)
        fired = []

        def concurrent_report(*args, **kwargs):
            if Path(args[1]).name == self.output.name and not fired:
                fired.append(True)
                self.output.write_bytes(sentinel)
            return original(*args, **kwargs)

        with patch.object(paths.os, operation, side_effect=concurrent_report):
            code, _, _ = self.main(run=self.report)
        self.assertEqual(fired, [True], 'atomic publication boundary was not exercised')
        self.assertEqual(code, 1)
        self.assertEqual(self.output.read_bytes(), sentinel)

    @unittest.skipUnless(os.name == 'posix', 'POSIX held-directory replacement')
    def test_report_parent_replacement_cannot_redirect_publication(self):
        moved = self.base / 'reports-held'

        def replace_parent(*unused):
            self.reports.rename(moved)
            self.reports.mkdir()
            return self.report()

        code, _, _ = self.main(run=replace_parent)
        self.assertEqual(code, 1)
        self.assertFalse(self.output.exists())
        self.assertFalse((moved / self.output.name).exists())

    @unittest.skipUnless(os.name == 'posix', 'POSIX same-inode directory relocation')
    def test_report_parent_moved_into_corpus_is_rejected(self):
        moved = self.golden / 'relocated-reports'
        sentinel = write(self.golden, 'retained', b'corpus evidence')

        def move_parent_into_corpus(*unused):
            self.reports.rename(moved)
            self.reports.symlink_to(moved, target_is_directory=True)
            return self.report()

        code, _, _ = self.main(run=move_parent_into_corpus)
        self.assertEqual(code, 1)
        self.assertTrue(self.reports.is_symlink())
        self.assertFalse((moved / self.output.name).exists())
        self.assertEqual(sentinel.read_bytes(), b'corpus evidence')

    @unittest.skipUnless(os.name == 'posix', 'POSIX corpus-parent substitution')
    def test_corpus_parent_replacement_invalidates_publication(self):
        write(self.golden, 'sentinel', b'retained corpus')
        held = self.base / 'golden-held'
        outside = self.base / 'substitute'
        write(outside, 'sentinel', b'replacement corpus')

        def replace_corpus(*unused):
            self.golden.rename(held)
            self.golden.symlink_to(outside, target_is_directory=True)
            return self.report()

        code, _, _ = self.main(run=replace_corpus)
        self.assertEqual(code, 1)
        self.assertFalse(self.output.exists())
        self.assertEqual((held / 'sentinel').read_bytes(), b'retained corpus')
        self.assertEqual((outside / 'sentinel').read_bytes(), b'replacement corpus')


if __name__ == '__main__':
    unittest.main()
