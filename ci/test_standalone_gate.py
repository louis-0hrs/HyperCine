# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Regression checks for false-green CI results, independent of a codec build."""
from pathlib import Path
import tempfile
import hashlib
import json
import subprocess
import unittest
from unittest.mock import patch
from run_standalone import (REQUIRED, registered_tests, completed_tests,
                            required_tests, validate_architecture, invalidate_results, main)


class StandaloneGateTests(unittest.TestCase):
    def test_internal_backend_session_gates_are_binding(self):
        for name in ('encoder_backend_selection_contract', 'encoder_backend_session_packets'):
            self.assertIn(name, REQUIRED)
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                registered_tests({'tests': [{'name': n} for n in REQUIRED if n != name]})

    def test_missing_optimized_acceptance_fails(self):
        name = 'optimized_standalone_acceptance'
        self.assertIn(name, REQUIRED)
        with self.assertRaisesRegex(ValueError, name):
            registered_tests({'tests': [{'name': n} for n in REQUIRED if n != name]})

    def test_rgb444_plane_allocation_failure_is_mandatory(self):
        name = 'rgb444_allocation_failure'
        self.assertIn(name, REQUIRED)
        with self.assertRaisesRegex(ValueError, name):
            registered_tests({'tests': [{'name': n} for n in REQUIRED if n != name]})

    def test_profile_enter_contract_is_mandatory(self):
        # #46: the shared six-profile calculator is the encoder's only table
        # producer; dropping the registration must not stay green.
        name = 'profile_enter_contract'
        self.assertIn(name, REQUIRED)
        with self.assertRaisesRegex(ValueError, name):
            registered_tests({'tests': [{'name': n} for n in REQUIRED if n != name]})

    def test_missing_reference_decode_contract_fails(self):
        self.assertIn('reference_decode_contract', REQUIRED)
        with self.assertRaises(ValueError):
            registered_tests({'tests': [{'name': n} for n in REQUIRED
                                       if n != 'reference_decode_contract']})

    def test_each_simd_registration_is_mandatory(self):
        for missing in ('wavelet_simd_exact', 'wavelet_simd_packets'):
            self.assertIn(missing, REQUIRED)
            with self.subTest(missing=missing), self.assertRaisesRegex(ValueError, missing):
                registered_tests({'tests': [{'name': name} for name in REQUIRED
                                            if name != missing]})

    def test_metal_cache_truth_matches_cmake(self):
        # CMake's variable truth rules also apply to BOOL cache entries.
        values = ('ON', 'TRUE', 'YES', 'Y', '1', '2', '0.0', 'custom',
                  'on', 'true', 'yes', '', '0', 'OFF', 'NO', 'FALSE',
                  'N', 'IGNORE', 'NOTFOUND', 'device-NOTFOUND', 'off',
                  'false', 'device-notfound')
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / 'truth.cmake'
            script.write_text('if(HYPERCINE_ENABLE_METAL)\n'
                              'message(STATUS "METAL_ENABLED")\n'
                              'else()\nmessage(STATUS "METAL_DISABLED")\nendif()\n')
            for value in values:
                with self.subTest(value=value):
                    result = subprocess.run(['cmake',
                        '-DHYPERCINE_ENABLE_METAL:BOOL=' + value, '-P', str(script)],
                        check=True, capture_output=True, text=True)
                    enabled = 'METAL_ENABLED' in result.stdout
                    required = required_tests({'HYPERCINE_ENABLE_METAL': value})
                    self.assertEqual('filmscan3_metal_required' in required, enabled)
                    inventory = {'tests': [{'name': name} for name in REQUIRED]}
                    if enabled:
                        with self.assertRaises(ValueError):
                            registered_tests(inventory, required)
                    else:
                        self.assertEqual(registered_tests(inventory, required), REQUIRED)

    def test_every_baseline_registration_is_binding(self):
        for omitted in REQUIRED:
            with self.subTest(omitted=omitted), self.assertRaises(ValueError):
                registered_tests({'tests': [{'name': n} for n in REQUIRED - {omitted}]})

    def test_encoder_capability_and_isa_gates_are_mandatory(self):
        gates = ('encoder_capability_contract', 'filmscan3_geometry_budget',
                 'wavelet_isa_session_policy', 'wavelet_isa_parallel_sessions',
                 'neon_wavelet_bitexact', 'neon_concurrency_contract')
        for missing in gates:
            self.assertIn(missing, REQUIRED)
            with self.subTest(missing=missing), self.assertRaisesRegex(ValueError, missing):
                registered_tests({'tests': [{'name': name} for name in REQUIRED
                                            if name != missing]})

    def test_metal_4k_gate_invokes_mandatory_metal_binary(self):
        # Reviewable source guard: otherwise a green FFmpeg 4K decode could be CPU-only.
        cmake = (Path(__file__).resolve().parent.parent / 'CMakeLists.txt').read_text()
        gate = cmake.split('add_test(NAME filmscan3_metal_4k_ffmpeg_gate', 1)[1]
        gate = gate.split('set_tests_properties(filmscan3_metal_4k_ffmpeg_gate', 1)[0]
        self.assertIn('--encoder $<TARGET_FILE:hypercine_encode_filmscan3_metal>', gate)
        self.assertNotIn('--encoder $<TARGET_FILE:hypercine_encode_filmscan3>', gate)
        self.assertIn('HYPERCINE_METAL_AUDIT=1', cmake)

    def test_cuda_session_gate_is_gpu_required_and_inside_cuda_block(self):
        # #259 step 2. The strict standalone inventory deliberately refuses a
        # CUDA-on tree, so this registration cannot be expressed as a REQUIRED
        # entry: it is checked at the source instead. Losing the gate, moving it
        # out of the CUDA block, or making it skippable must fail here.
        cmake = (Path(__file__).resolve().parent.parent / 'CMakeLists.txt').read_text()
        registration = 'add_test(NAME encoder_cuda_session_packets COMMAND test_encoder_cuda_session)'
        self.assertIn(registration, cmake)
        properties = cmake.split('set_tests_properties(encoder_cuda_session_packets', 1)[1]
        properties = properties.split(')', 1)[0]
        self.assertIn('gpu-required', properties)
        self.assertIn('RUN_SERIAL TRUE', properties)
        self.assertNotIn('SKIP_RETURN_CODE', properties)
        # The gate must sit in the CUDA branch, not in the shared standalone block.
        cuda_block = cmake.split('# Issue #259 step 2: the same capability-gated session', 1)
        self.assertEqual(len(cuda_block), 2, 'CUDA session gate lost its CUDA guard')
        self.assertTrue(cuda_block[0].rstrip().endswith('if(HYPERCINE_ENABLE_CUDA)'))
        block = cuda_block[1].split('endif()', 1)[0]
        self.assertIn(registration, block)
        profile = 'add_test(NAME encoder_cuda_profile_matrix COMMAND test_encoder_cuda_profile_matrix)'
        self.assertIn(profile, block)
        profile_properties = cmake.split('set_tests_properties(encoder_cuda_profile_matrix', 1)[1]
        profile_properties = profile_properties.split(')', 1)[0]
        self.assertIn('gpu-required', profile_properties)
        self.assertIn('RUN_SERIAL TRUE', profile_properties)
        self.assertNotIn('SKIP_RETURN_CODE', profile_properties)

    def test_metal_enabled_requires_all_existing_metal_gates(self):
        cache = {'HYPERCINE_ENABLE_METAL': 'ON'}
        required = required_tests(cache)
        metal = {'filmscan3_metal_required', 'filmscan3_metal_autonomous_manifest',
                 'filmscan3_metal_sequence_manifest', 'filmscan3_metal_external_decode',
                 'filmscan3_metal_4k_ffmpeg_gate', 'filmscan3_metal_4k_encoder',
                 'filmscan3_metal_multisession', 'filmscan3_metal_geometry_matrix',
                 'encoder_metal_session_packets'}
        self.assertEqual(required - REQUIRED, metal)
        for omitted in metal:
            with self.subTest(omitted=omitted), self.assertRaises(ValueError):
                registered_tests({'tests': [{'name': n} for n in required - {omitted}]}, required)
        self.assertEqual(required_tests({'HYPERCINE_ENABLE_METAL': 'OFF'}), REQUIRED)

    def test_configured_original_corpora_are_binding(self):
        for key, expected in (
            ('HYPERCINE_REFERENCE_CORPUS_ROOT', {'windows_reference_binaries', 'filmscan3_actual_tier_a'}),
            ('HYPERCINE_FS3_SEQUENCE_ROOT', {'filmscan3_actual_sequence'}),
        ):
            cache = {key: 'corpus'}
            required = required_tests(cache)
            self.assertEqual(required - REQUIRED, expected)
            with self.assertRaises(ValueError):
                registered_tests({'tests': [{'name': n} for n in REQUIRED]}, required)
            cache['HYPERCINE_ENABLE_METAL'] = 'ON'
            metal_required = required_tests(cache)
            for name in expected - {'windows_reference_binaries'}:
                self.assertIn(name.replace('filmscan3_', 'filmscan3_metal_'), metal_required)
        self.assertEqual(required_tests({}), REQUIRED)

    def test_apple_target_must_match_declared_native_architecture(self):
        for architecture in ('', 'x86_64', 'arm64;x86_64', 'arm64e'):
            with self.subTest(architecture=architecture), self.assertRaises(ValueError):
                validate_architecture({'CMAKE_OSX_ARCHITECTURES': architecture}, 'Darwin', 'arm64')
        validate_architecture({'CMAKE_OSX_ARCHITECTURES': 'arm64'}, 'Darwin', 'arm64')
        validate_architecture({}, 'Linux', 'x86_64')
        validate_architecture({}, 'Windows', 'x86_64')

    def test_failed_preflight_removes_stale_pass_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            names = ('standalone-gate.json', 'standalone-results.xml', 'registered-tests.json')
            for name in names:
                (build / name).write_text('stale PASS')
            (build / 'diagnostic.log').write_text('retain diagnostics')
            with patch('sys.argv', ['run_standalone.py', '--build-dir', str(build),
                                    '--system', 'Darwin', '--machine', 'arm64']), \
                 patch('run_standalone.platform.system', return_value='Linux'), \
                 self.assertRaises(ValueError):
                main()
            self.assertTrue(all(not (build / name).exists() for name in names))
            self.assertTrue((build / 'diagnostic.log').exists())
            invalidate_results(build)  # Also safe before any results exist.

    def test_windows_qualification_contracts_are_required(self):
        for name in ('qualification_report_contract', 'cuda_output_dir_platform_contract',
                     'windows_reference_process_contract'):
            self.assertIn(name, REQUIRED)
            with self.assertRaises(ValueError):
                registered_tests({'tests': [{'name': n} for n in REQUIRED if n != name]})

    def test_unit_only_inventory_fails(self):
        with self.assertRaises(ValueError):
            registered_tests({'tests': [{'name': 'standalone_color'}]})

    def test_missing_external_decode_fails(self):
        with self.assertRaises(ValueError):
            registered_tests({'tests': [{'name': n} for n in REQUIRED if n != 'standalone_external_decode']})

    def test_missing_gop2_integrity_fails(self):
        self.assertIn('gop2_reference_inventory_integrity', REQUIRED)
        with self.assertRaises(ValueError):
            registered_tests({'tests': [{'name': n} for n in REQUIRED
                                       if n != 'gop2_reference_inventory_integrity']})

    def test_missing_gop2_stage_contract_fails(self):
        self.assertIn('gop2_original_stage_contract', REQUIRED)
        with self.assertRaises(ValueError):
            registered_tests({'tests': [{'name': n} for n in REQUIRED
                                       if n != 'gop2_original_stage_contract']})
    def test_rgb444_probe_is_mandatory(self):
        # A green full suite must never silently omit the structural RGB444 gate.
        name = 'rgb444_packet_probe'
        self.assertIn(name, REQUIRED)
        with self.assertRaisesRegex(ValueError, name):
            registered_tests({'tests': [{'name': n} for n in REQUIRED if n != name]})

    def test_rgb444_p12_quality_registrations_are_mandatory(self):
        # #37: losing the measured p12 table would let the encoder fall back to
        # the p10 YUV quantizers without any failing test.
        for name in ('rgb444_p12_quality_table', 'rgb444_p12_quality_pin'):
            self.assertIn(name, REQUIRED)
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                registered_tests({'tests': [{'name': n} for n in REQUIRED if n != name]})

    def test_both_geometry_checks_must_be_registered(self):
        for name in ('extreme_geometry_c0_arithmetic', 'extreme_geometry_c0_report'):
            self.assertIn(name, REQUIRED)
            with self.subTest(name=name), self.assertRaises(ValueError):
                registered_tests({'tests': [{'name': n} for n in REQUIRED if n != name]})

    def test_complete_inventory(self):
        self.assertEqual(registered_tests({'tests': [{'name': n} for n in REQUIRED]}), REQUIRED)

    def test_rg48_contract_registrations_are_mandatory(self):
        for missing in ('rgb444_frame_storage', 'rgb444_frame_rejection',
                        'rgb444_rg48_import', 'rgb444_rg48_rejection',
                        'rgb444_rg48_replay_contract',
                        'rgb444_rg48_allocation_failure'):
            self.assertIn(missing, REQUIRED)
            with self.subTest(missing=missing), self.assertRaises(ValueError):
                registered_tests({'tests': [{'name': n} for n in REQUIRED if n != missing]})

    def test_merged_scalar_and_gop2_inventory_is_mandatory(self):
        # A merge that drops either lane must fail even if its other tests pass.
        merged = {'gop2_original_stage_contract', 'gop2_reference_inventory_integrity',
                  'scalar_inverse_arithmetic', 'scalar_inverse_rejection',
                  'scalar_inverse_vectors', 'scalar_inverse_replay_contract'}
        merged |= {'scalar_inverse_graph_workers_' + str(w) for w in (1, 2, 4, 16)}
        self.assertTrue(merged <= REQUIRED)
        for missing in sorted(merged):
            with self.subTest(missing=missing), self.assertRaises(ValueError):
                registered_tests({'tests': [{'name': n} for n in REQUIRED if n != missing]})

    def test_each_shared_contract_registration_is_mandatory(self):
        # These four are registered through hypercine_register_sdk_free_contracts()
        # or the merged P4 geometry block; dropping any single registration must
        # fail the gate, not shrink it back to the old green inventory.
        for missing in ('test_reference_release_inventory', 'test_compare_sdk_oracle',
                        'test_rca159_witness_contract',
                        'filmscan3_geometry_reference_p10'):
            self.assertIn(missing, REQUIRED)
            with self.subTest(missing=missing), self.assertRaises(ValueError):
                registered_tests({'tests': [{'name': n} for n in REQUIRED if n != missing]})

    def check_xml(self, xml, names, expect_error):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'result.xml'
            path.write_text(xml)
            if expect_error:
                with self.assertRaises(ValueError):
                    completed_tests(path, names)
            else:
                self.assertEqual(completed_tests(path, names), len(names))

    def test_skipped_disabled_failed_or_missing_results_fail(self):
        for body in ('<skipped/>', '<failure/>', '<error/>'):
            self.check_xml('<testsuite><testcase name="decode" status="run">'+body+'</testcase></testsuite>', {'decode'}, True)
        self.check_xml('<testsuite><testcase name="decode" status="notrun"/></testsuite>', {'decode'}, True)
        self.check_xml('<testsuite/>', {'decode'}, True)
        self.check_xml('<testsuite><testcase name="decode" status="run"/><testcase name="decode" status="run"/></testsuite>', {'decode'}, True)

    def test_windows_autocrlf_preserves_pinned_reference_bytes(self):
        root = Path(__file__).resolve().parents[1]
        provenance = json.loads((root/'tests/reference_corpus/import_provenance.json').read_text())
        # load_manifest verifies these two imported manifests and the generator.
        # The schema has since evolved for the quality exporter (PR #27).
        manifests = {'tests/reference_corpus/manifest.json',
                     'tests/reference_corpus/phase3a_1080p_120_manifest.json'}
        files = [f for f in provenance['files'] if f['path'] in manifests]
        self.assertEqual({f['path'] for f in files}, manifests)
        files += [provenance['portable_generator']]
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory).as_posix() + '/'
            subprocess.run(['git', '-c', 'core.autocrlf=true', 'checkout-index',
                            '--prefix=' + prefix, '--'] + [f['path'] for f in files],
                           cwd=root, check=True)
            for record in files:
                data = (Path(directory)/record['path']).read_bytes()
                self.assertEqual(hashlib.sha256(data).hexdigest(), record['sha256'], record['path'])

    def test_completed_pass(self):
        self.check_xml('<testsuite><testcase name="decode" status="run"/></testsuite>', {'decode'}, False)


if __name__ == '__main__':
    unittest.main()
