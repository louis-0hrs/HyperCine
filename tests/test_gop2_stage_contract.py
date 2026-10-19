#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Fail-closed stage-contract tests; no private corpus or native pass is inferred."""
import copy
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import gop2_stage_contract as contract
import prepare_gop2_stage_sdk as observer


class StageContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.indices = [json.loads((ROOT / 'docs/validation' /
            f'gop2_issue189_phaseb_{g}_2026-10-19.json').read_text())
            for g in ('1920x1080', '3840x2160')]

    def test_actual_index_observation_shapes(self):
        for index in self.indices:
            self.assertEqual(len(contract.validate_observations(index['observations'])), 57)

    def test_missing_or_duplicate_original_bands_fail(self):
        for mode in ('missing', 'duplicate'):
            report = copy.deepcopy(self.indices[0]['observations'])
            if mode == 'missing':
                report['pre_inverse_bands'].pop()
            else:
                report['pre_inverse_bands'][-1] = report['pre_inverse_bands'][0]
            with self.assertRaises(ValueError):
                contract.validate_observations(report)

    def test_missing_signed_plane_or_wrong_storage_fails(self):
        report = copy.deepcopy(self.indices[0]['observations'])
        report['signed_planes'].pop()
        with self.assertRaises(ValueError):
            contract.validate_observations(report)
        report = copy.deepcopy(self.indices[0]['observations'])
        report['signed_planes'][0]['pitch'] += 2
        with self.assertRaises(ValueError):
            contract.validate_observations(report)

    def test_alternate_sdk_path_must_have_default_output_control(self):
        for field, value in (('alternate_original_path_checks', 0), ('neutrality_frame_checks', 0),
                             ('neutrality_resident_checks', 197), ('instrumentation_neutrality', 'FAIL'),
                             ('stage_observations_per_band_or_row', 1), ('sample_reencoded', True),
                             ('signed_path', 'candidate_generated'), ('allowed_cpus', 8)):
            report = copy.deepcopy(self.indices[0]['observations'])
            report[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                contract.validate_observations(report)

    def test_comparison_matrix_cannot_drop_or_duplicate_cases(self):
        index = self.indices[0]
        report = {'schema': 'hypercine.gop2.stage_comparison/1', 'candidate_sha': contract.CANDIDATE,
                  'comparisons': index['comparisons'], 'compared': 90, 'pre_inverse_band_comparisons': 84,
                  'signed_plane_comparisons': 6, 'unique_pre_inverse_band_oracles': 51}
        contract.validate_comparison(report)
        report['comparisons'] = [report['comparisons'][0]] * 90
        with self.assertRaises(ValueError):
            contract.validate_comparison(report)

    def test_observer_anchor_drift_fails(self):
        for patcher in (observer.instrument_decoder, observer.instrument_wavelet):
            with self.assertRaises(ValueError):
                patcher(b'no frozen SDK anchors')

    def test_observer_cannot_write_tracked_upstream(self):
        with self.assertRaises(ValueError):
            observer.prepare(ROOT, ROOT / 'upstream/private-observer-forbidden')

    def binding_fixture(self):
        index = copy.deepcopy(self.indices[0])
        observations = copy.deepcopy(index['observations'])
        report = {'schema': 'hypercine.gop2.stage_comparison/1', 'candidate_sha': contract.CANDIDATE,
                  'width': 1920, 'height': 1080, 'compared': 90, 'mismatched': 0,
                  'pre_inverse_band_comparisons': 84, 'signed_plane_comparisons': 6,
                  'unique_pre_inverse_band_oracles': 51,
                  'comparisons': [{k: v for k, v in item.items() if k not in ('actual', 'expected')}
                                  for item in index['comparisons']]}
        return index, observations, report, {'geometry': [1920, 1080]}

    def test_actual_stage_binding_control(self):
        contract.validate_binding(*self.binding_fixture())

    def test_reject_relabelled_candidate_or_geometry(self):
        for field, value in (('candidate_sha', '0'*40), ('geometry', [3840, 2160]),
                             ('geometry', [1920.0, 1080.0])):
            values = self.binding_fixture()
            values[0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                contract.validate_binding(*values)

    def test_reject_detached_observation_and_source_provenance(self):
        values = self.binding_fixture()
        values[0]['observations']['neutrality_frame_checks'] = 11
        with self.assertRaises(ValueError):
            contract.validate_binding(*values)
        values = self.binding_fixture()
        values[0]['original_stages'][0]['input_packet']['sha256'] = '0'*64
        with self.assertRaises(ValueError):
            contract.validate_binding(*values)

    def test_reject_resealed_false_equal_or_report_disagreement(self):
        values = self.binding_fixture()
        values[0]['comparisons'][0]['actual']['sha256'] = '0'*64
        with self.assertRaises(ValueError):
            contract.validate_binding(*values)
        values = self.binding_fixture()
        values[2]['comparisons'][0]['equal'] = False
        with self.assertRaises(ValueError):
            contract.validate_binding(*values)


if __name__ == '__main__':
    unittest.main()
