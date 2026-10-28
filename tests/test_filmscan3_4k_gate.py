# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Portable negative controls; these tests do not execute or qualify Metal."""
import copy
import json
import re
import subprocess
import sys
from unittest.mock import patch
from pathlib import Path
import tempfile
import unittest

import validate_filmscan3_4k_ffmpeg as gate


class ParityGateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.cpu, self.metal = self.root / 'cpu', self.root / 'metal'
        self.frame = dict(frame_index=0, fs_rate_limiter=0, previous_sample_bytes=8,
                          sample_bytes=8, quant=[1]*36, scale=[1]*36,
                          coefficient_hash='full', coefficient_fnv64=['0'*16]*30)
        for directory in (self.cpu, self.metal):
            directory.mkdir()
            (directory / 'frame_0000.cfhd').write_bytes(b'CFHDtest')
            self.trace(directory, [self.frame])

    def trace(self, directory, frames):
        (directory / 'trace.json').write_text(json.dumps(dict(profile='FilmScan3', frames=frames)))

    def test_actual_cmake_registration_requires_metal_and_cpu(self):
        source = (Path(__file__).resolve().parents[1] / 'CMakeLists.txt').read_text()
        block = re.search(r'(?s)add_test\(NAME filmscan3_metal_4k_ffmpeg_gate\b.*?'
                          r'set_tests_properties\(filmscan3_metal_4k_ffmpeg_gate\b.*?\)', source)
        self.assertIsNotNone(block, '4K gate registration missing')
        project = self.root / 'cmake'
        project.mkdir()
        (project / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.25)\nproject(GateContract NONE)\nenable_testing()\n'
            'set(Python3_EXECUTABLE "' + Path(sys.executable).as_posix() + '")\n'
            'foreach(name hypercine_encode_filmscan3 hypercine_encode_filmscan3_metal)\n'
            'add_executable(${name} IMPORTED)\n'
            'set_target_properties(${name} PROPERTIES IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/${name}")\n'
            'endforeach()\n' + block.group(0))
        build = project / 'build'
        subprocess.run(['cmake', '-S', str(project), '-B', str(build)],
                       check=True, capture_output=True)
        document = json.loads(subprocess.check_output(
            ['ctest', '--test-dir', str(build), '-C', 'Release', '--show-only=json-v1']))
        test = document['tests'][0]
        command = test['command']
        self.assertEqual(Path(command[command.index('--encoder') + 1]).name,
                         'hypercine_encode_filmscan3_metal')
        self.assertEqual(Path(command[command.index('--cpu-encoder') + 1]).name,
                         'hypercine_encode_filmscan3')
        self.assertIn('--require-metal', command)
        self.assertEqual(command[command.index('--frames') + 1], '16')
        properties = {p['name']: p['value'] for p in test['properties']}
        self.assertIn('gpu-required', properties['LABELS'])
        self.assertTrue(properties['RUN_SERIAL'])
        self.assertNotIn('SKIP_RETURN_CODE', properties)

    def test_fixture_cycle_is_deterministic_and_changes_complexity(self):
        with patch.object(gate, 'WIDTH', 32), patch.object(gate, 'HEIGHT', 8):
            sources = gate.make_inputs(self.root, 16)
            first = [p.read_bytes() for p in sources]
            self.assertEqual(len(sources), 16)
            self.assertEqual(len(set(first)), 4)
            self.assertTrue(all(len(data) == 32*8*4 for data in first))
            self.assertEqual(first[:4], first[4:8])
            self.assertEqual(first, [p.read_bytes() for p in gate.make_inputs(self.root, 16)])

    def test_required_metal_rejects_missing_or_same_cpu_tool_before_encoding(self):
        base = ['gate', '--encoder', 'tool', '--ffmpeg', 'ffmpeg',
                '--ffprobe', 'ffprobe', '--require-metal']
        for options in ([], ['--cpu-encoder', 'tool']):
            with self.subTest(options=options), patch.object(sys, 'argv', base + options), \
                    patch.object(gate, 'make_inputs') as inputs, self.assertRaises(ValueError):
                gate.main()
            inputs.assert_not_called()

    def test_cpu_substitution_cannot_satisfy_required_metal(self):
        argv = ['gate', '--encoder', 'cpu-renamed-as-metal', '--cpu-encoder', 'cpu',
                '--ffmpeg', 'ffmpeg', '--ffprobe', 'ffprobe', '--require-metal']
        with patch.object(sys, 'argv', argv), \
                patch.object(gate, 'make_inputs', return_value=[self.root / 'input']), \
                patch.object(gate.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, stdout='')) as run, \
                self.assertRaisesRegex(ValueError, 'mandatory Metal device missing'):
            gate.main()
        self.assertEqual(run.call_count, 1)  # refuse before CPU comparison/decode

    def test_matching_complete_packet_and_trace(self):
        gate.compare_outputs(self.cpu, self.metal, 1)

    def test_packet_difference_reports_frame_and_first_byte(self):
        for packet, offset in ((b'CFHDtesX', 7), (b'CFHDtestX', 8), (b'', 0)):
            with self.subTest(packet=packet):
                (self.metal / 'frame_0000.cfhd').write_bytes(packet)
                with self.assertRaisesRegex(ValueError, 'frame 0.*byte %d' % offset):
                    gate.compare_outputs(self.cpu, self.metal, 1)

    def test_every_state_table_and_band_field_is_binding(self):
        for field in self.frame:
            changed = copy.deepcopy(self.frame)
            if field == 'coefficient_hash':
                changed[field] = 'off'
            elif isinstance(changed[field], list):
                changed[field][0] = '1'*16 if field == 'coefficient_fnv64' else 2
            else:
                changed[field] += 1
            self.trace(self.metal, [changed])
            with self.subTest(field=field), self.assertRaises(ValueError):
                gate.compare_outputs(self.cpu, self.metal, 1)

    def test_identical_malformed_traces_never_pass(self):
        mutations = [dict(frame_index=True), dict(fs_rate_limiter=0.0),
                     dict(previous_sample_bytes=False), dict(sample_bytes=9),
                     dict(quant=[1]*35), dict(scale=[True]*36),
                     dict(coefficient_hash='off'), dict(coefficient_fnv64=['bad']*30)]
        for mutation in mutations:
            changed = dict(self.frame, **mutation)
            for directory in (self.cpu, self.metal):
                self.trace(directory, [changed])
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                gate.compare_outputs(self.cpu, self.metal, 1)

    def test_missing_reordered_and_duplicate_frames_fail(self):
        second = dict(self.frame, frame_index=1)
        for frames in ([], [second], [self.frame, self.frame], [second, self.frame]):
            for directory in (self.cpu, self.metal):
                self.trace(directory, frames)
            with self.subTest(frames=frames), self.assertRaises(ValueError):
                gate.compare_outputs(self.cpu, self.metal, 2)

    def test_duplicate_json_keys_fail(self):
        path = self.metal / 'trace.json'
        path.write_text('{"profile":"FilmScan3","frames":[],"frames":' + json.dumps([self.frame]) + '}')
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            gate.compare_outputs(self.cpu, self.metal, 1)

    def test_missing_packet_fails(self):
        (self.metal / 'frame_0000.cfhd').unlink()
        with self.assertRaises((ValueError, OSError)):
            gate.compare_outputs(self.cpu, self.metal, 1)

    def test_metal_audit_must_cover_every_frame_and_level(self):
        valid = 'Required Metal device: Test GPU\nMetal device: Test GPU; audited frames=16 levels=144 upload_ms=1\n'
        gate.require_metal_audit(valid, 16)
        for invalid in ('', 'Required Metal device: Test GPU',
                        valid.replace('frames=16', 'frames=1'),
                        valid.replace('levels=144', 'levels=143')):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                gate.require_metal_audit(invalid, 16)

    def test_wrong_decode_geometry_format_and_storage_fail(self):
        gate.check_decode(3840, 2160, 'yuv422p10le', gate.EXPECTED_BYTES)
        for values in ((1920, 1080, 'yuv422p10le', gate.EXPECTED_BYTES),
                       (3840, 2160, 'yuv422p', gate.EXPECTED_BYTES),
                       (3840, 2160, 'yuv422p10le', 0)):
            with self.subTest(values=values), self.assertRaises(ValueError):
                gate.check_decode(*values)


if __name__ == '__main__':
    unittest.main()
