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
from run_standalone import REQUIRED, registered_tests, completed_tests


class StandaloneGateTests(unittest.TestCase):
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
    def test_complete_inventory(self):
        self.assertEqual(registered_tests({'tests': [{'name': n} for n in REQUIRED]}), REQUIRED)

    def test_rg48_registrations_required(self):
        # RGB444 (this branch): each RG48 registration is mandatory.
        for missing in ('rgb444_rg48_import', 'rgb444_rg48_rejection', 'rgb444_rg48_replay_contract'):
            with self.subTest(missing=missing), self.assertRaises(ValueError):
                registered_tests({'tests': [{'name': n} for n in REQUIRED if n != missing]})

    def test_merged_scalar_and_gop2_inventory_is_mandatory(self):
        # main: a merge that drops either lane must fail even if its other tests pass.
        merged = {'gop2_original_stage_contract', 'gop2_reference_inventory_integrity',
                  'scalar_inverse_arithmetic', 'scalar_inverse_rejection',
                  'scalar_inverse_vectors', 'scalar_inverse_replay_contract'}
        merged |= {'scalar_inverse_graph_workers_' + str(w) for w in (1, 2, 4, 16)}
        self.assertTrue(merged <= REQUIRED)
        for missing in sorted(merged):
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
