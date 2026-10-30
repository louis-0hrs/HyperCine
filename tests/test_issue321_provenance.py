# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Corpus-free adversarial controls for retained-artifact provenance checks."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import verify_issue321_provenance as v


class Provenance(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.binary = self.root / 'capture.exe'
        self.binary.write_bytes(b'retained artifact')
        self.digest = hashlib.sha256(self.binary.read_bytes()).hexdigest()

    def test_actual_bytes_required(self):
        self.assertEqual(v.bound_file(self.binary, self.digest)['bytes'], 17)
        self.binary.write_bytes(b'modified artifact')
        with self.assertRaises(ValueError):
            v.bound_file(self.binary, self.digest)

    def test_missing_binary_is_not_provenance(self):
        with self.assertRaises(ValueError):
            v.bound_file(self.root / 'missing.exe', self.digest)

    def records(self):
        compiler = self.root / 'CMakeFiles/3.31/CMakeCXXCompiler.cmake'
        compiler.parent.mkdir(parents=True)
        compiler.write_text('set(CMAKE_CXX_COMPILER_ID "MSVC")\n'
                            'set(CMAKE_CXX_COMPILER_VERSION "19.44.35229.0")\n')
        directory = self.root / 'capture.dir/Release/capture.tlog'
        directory.mkdir(parents=True)
        (directory / 'CL.command.1.tlog').write_text(
            '/O2 /Ob2 /MD /std:c++17 /I"PRIVATE_LOCATION"\n', encoding='utf-16')
        (directory / 'link.command.1.tlog').write_text('PRIVATE_LOCATION', encoding='utf-16')
        return compiler, directory

    def test_report_emits_selected_flags_not_private_commands(self):
        self.records()
        report = v.build_record(self.root, 'capture')
        self.assertEqual(report['compiler_version'], '19.44.35229.0')
        self.assertEqual(report['selected_compile_flags'], ['/O2', '/Ob2', '/MD', '/std:c++17'])
        self.assertNotIn('PRIVATE_LOCATION', json.dumps(report))
        self.assertIn('not_attestation', report['evidence_class'])

    def test_missing_link_record_rejected(self):
        _, directory = self.records()
        (directory / 'link.command.1.tlog').unlink()
        with self.assertRaises(ValueError):
            v.build_record(self.root, 'capture')

    def test_ambiguous_compiler_rejected(self):
        compiler, _ = self.records()
        duplicate = self.root / 'CMakeFiles/other/CMakeCXXCompiler.cmake'
        duplicate.parent.mkdir()
        duplicate.write_bytes(compiler.read_bytes())
        with self.assertRaises(ValueError):
            v.build_record(self.root, 'capture')

    def test_unknown_compiler_version_rejected(self):
        compiler, _ = self.records()
        compiler.write_text('set(CMAKE_CXX_COMPILER_ID "MSVC")\n')
        with self.assertRaises(ValueError):
            v.build_record(self.root, 'capture')

    def test_wrong_link_input_rejected(self):
        self.records()
        with self.assertRaises(ValueError):
            v.build_record(self.root, 'capture', library=self.binary)

    def test_wrong_compile_input_rejected(self):
        self.records()
        with self.assertRaises(ValueError):
            v.build_record(self.root, 'capture', source=self.binary)

    def test_link_input_normalizes_parent_components(self):
        _, directory = self.records()
        linked = str(self.root / 'unused/../capture.exe')
        (directory / 'link.command.1.tlog').write_text('"' + linked + '"', encoding='utf-16')
        v.build_record(self.root, 'capture', library=self.binary)

    def test_packet_divergence_is_fatal(self):
        # Parser behavior has its own corpus contract; exercise this caller's
        # decision on distinct normalized packets, with raw bytes also distinct.
        with patch.object(v, 'parse_packet', side_effect=[
                {'normalized_sha256': 'a'}, {'normalized_sha256': 'b'}]):
            with self.assertRaises(ValueError):
                v.compare_packet(b'candidate', b'original')

    def test_metadata_difference_is_reported_separately(self):
        with patch.object(v, 'parse_packet', return_value={'normalized_sha256': 'a'}):
            report = v.compare_packet(b'candidate', b'original')
        self.assertTrue(report['normalized_equal'])
        self.assertFalse(report['raw_equal'])

    def test_bad_input_hash_prevents_execution(self):
        data = self.root / 'frame.bgra'
        data.write_bytes(b'pixels')
        sessions = [{'profile': 'low', 'frames': [{'input': 'frame.bgra',
                     'input_sha256': '0' * 64}]}]
        with patch.object(v.subprocess, 'run') as process:
            with self.assertRaises(ValueError):
                v.replay(self.binary, self.root, sessions, self.root / 'out', False)
            process.assert_not_called()

    def test_replayed_state_and_order_are_fatal_gates(self):
        data = self.root / 'frame.bgra'
        data.write_bytes(b'pixels')
        packet = self.root / 'original.cfhd'
        packet.write_bytes(b'packet')
        frame = {'input': data.name, 'input_sha256': v.file_sha(data),
                 'sdk_packet': packet.name, 'sdk_sha256': v.file_sha(packet),
                 'sdk_overbitrate_after_encode': 2, 'sdk_rate_limiter_after_encode': 0,
                 'sdk_lastgopbitcount_after_encode': 48}
        for index, trace in enumerate(('2\t0\t48\t0\t6\tframe_0.cfhd',
                                      '1\t0\t48\t0\t6\tframe_0.cfhd',
                                      '2\t0\t48\t1\t6\tframe_0.cfhd',
                                      '2\t0\t48\t0\t7\tframe_0.cfhd')):
            def capture(argv, **kwargs):
                out = Path(argv[2])
                out.mkdir(parents=True)
                (out / 'frame_0.cfhd').write_bytes(b'packet')
                (out / 'trace.tsv').write_text(trace + '\n')
                return v.subprocess.CompletedProcess(argv, 0)
            with self.subTest(trace=index), patch.object(v.subprocess, 'run', capture), \
                    patch.object(v, 'compare_packet', return_value={'normalized_equal': True}):
                args = (self.binary, self.root, [{'profile': 'low', 'frames': [frame]}],
                        self.root / str(index), True)
                if index == 0:
                    self.assertEqual(v.replay(*args)[0]['sdk_state'], [2, 0, 48])
                else:
                    with self.assertRaises(ValueError):
                        v.replay(*args)


if __name__ == '__main__':
    unittest.main()
