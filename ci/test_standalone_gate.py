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
                            required_tests, validate_architecture, validate_compile_architecture, invalidate_results, main)


class StandaloneGateTests(unittest.TestCase):
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

    def test_metal_enabled_requires_all_existing_metal_gates(self):
        cache = {'HYPERCINE_ENABLE_METAL': 'ON'}
        required = required_tests(cache)
        metal = {'filmscan3_metal_required', 'filmscan3_metal_autonomous_manifest',
                 'filmscan3_metal_sequence_manifest', 'filmscan3_metal_external_decode'}
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

    def test_unit_only_inventory_fails(self):
        with self.assertRaises(ValueError):
            registered_tests({'tests': [{'name': 'standalone_color'}]})

    def test_missing_external_decode_fails(self):
        with self.assertRaises(ValueError):
            registered_tests({'tests': [{'name': n} for n in REQUIRED if n != 'standalone_external_decode']})

    def test_complete_inventory(self):
        self.assertEqual(registered_tests({'tests': [{'name': n} for n in REQUIRED]}), REQUIRED)

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


    def test_optional_corpus_truth_matches_actual_cmake(self):
        keys = ('HYPERCINE_REFERENCE_CORPUS_ROOT', 'HYPERCINE_FS3_SEQUENCE_ROOT')
        values = ('', 'OFF', 'false', '0', 'NOTFOUND', 'corpus-NOTFOUND', 'corpus', 'corpus-notfound')
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / 'truth.cmake'
            script.write_text('if(ROOT)\nmessage(STATUS "ENABLED")\nendif()\n')
            for key in keys:
                for value in values:
                    with self.subTest(key=key,value=value):
                        result = subprocess.run(['cmake', '-DROOT:STRING='+value, '-P', str(script)],
                            check=True, capture_output=True, text=True)
                        enabled = 'ENABLED' in result.stdout
                        required = required_tests({key:value})
                        self.assertEqual(required != REQUIRED, enabled)
                        if not enabled:
                            registered_tests({'tests':[{'name':n} for n in REQUIRED]}, required)

    def test_target_override_cannot_bypass_native_apple_gate(self):
        good = [{'command': 'clang++ -arch arm64 -c unit.cpp'}]
        validate_compile_architecture(good, 'Darwin', 'arm64')
        validate_compile_architecture([{'arguments': ['clang++', '-arch', 'arm64', '-c', 'a b.cpp']}], 'Darwin', 'arm64')
        validate_compile_architecture([{'command': 'c++ -c unit.cpp'}], 'Linux', 'x86_64')
        for command in ('clang++ -arch x86_64 -c unit.cpp',
                        'clang++ -arch arm64 -arch x86_64 -c unit.cpp',
                        'clang++ -c unit.cpp', 'clang++ -arch',
                        'clang++ -arch arm64 --target=x86_64-apple-darwin -c unit.cpp'):
            with self.subTest(command=command), self.assertRaises(ValueError):
                validate_compile_architecture([{'command': command}], 'Darwin', 'arm64')

if __name__ == '__main__':
    unittest.main()
