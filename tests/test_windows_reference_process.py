#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
import tempfile
import json
from pathlib import Path
import unittest
from unittest.mock import patch
import validate_windows_reference_process as v


class ProcessContract(unittest.TestCase):
    def test_matrix_separates_axes_and_is_bounded(self):
        cells = v.matrix(0xf)
        self.assertEqual(len(cells), 6)
        self.assertEqual([c['threads'] for c in cells[:4]], [1, 2, 4, 8])
        self.assertEqual({c['affinity'] for c in cells[:4]}, {'f'})
        self.assertEqual({c['threads'] for c in cells[4:]}, {2})
        self.assertEqual([c['affinity'] for c in cells[4:]], ['1', '2'])

    def test_single_allowed_cpu_does_not_invent_second_affinity(self):
        self.assertEqual(len(v.matrix(8)), 5)
        with self.assertRaises(ValueError):
            v.matrix(0)

    def test_mismatched_sample_never_starts_decoder(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(v.subprocess, 'run') as child:
            sample = Path(directory) / 'sample'
            sample.write_bytes(b'immutable')
            with self.assertRaises(ValueError):
                v.run(Path('missing-probe'), sample, '0' * 64)
            child.assert_not_called()

    def test_unstable_hash_is_blocked_and_not_best_of_three(self):
        rows = [dict(axis='thread_request', threads=2, affinity='f', status='PASS', output_sha256=h)
                for h in ('a', 'b', 'a')]
        self.assertEqual(v.classify(rows), 'BLOCKED_NONDETERMINISTIC_REFERENCE')

    def test_failure_dominates_instability(self):
        rows = [dict(axis='thread_request', threads=2, affinity='f', status='PASS', output_sha256=h)
                for h in ('a', 'b')]
        self.assertEqual(v.classify(rows + [dict(status='FAIL')]), 'FAIL')

    def test_different_configuration_outputs_do_not_become_global_reference(self):
        rows = [dict(axis='thread_request', threads=n, affinity='f', status='PASS', output_sha256=h)
                for n, h in ((1, 'a'), (2, 'b')) for _ in range(3)]
        self.assertEqual(v.classify(rows), 'BLOCKED_CONFIGURATION_DEPENDENT_REFERENCE')

    def test_repeatable_is_not_pixel_correctness_pass(self):
        rows = [dict(axis='thread_request', threads=2, affinity='f', status='PASS', output_sha256='a') for _ in range(3)]
        self.assertEqual(v.classify(rows), 'REPEATABLE_OBSERVATION_ONLY')

    def test_missing_or_partial_plan_is_inconclusive(self):
        self.assertEqual(v.classify([]), 'INCONCLUSIVE')
        row = dict(axis='thread_request', threads=2, affinity='f', status='PASS', output_sha256='a')
        self.assertEqual(v.classify([row]), 'INCONCLUSIVE')
        self.assertEqual(v.classify([row]*3, v.matrix(15)), 'INCONCLUSIVE')

    def test_affinity_plan_distinguishes_smt_from_two_physical_cores(self):
        cores = [dict(logical_mask='3'), dict(logical_mask='c')]
        cells = v.matrix(15, cores)
        self.assertEqual([c['affinity'] for c in cells[4:]], ['1', '5', '3'])
        self.assertEqual(len(cells), 7)

    def test_duplicate_repeat_ids_cannot_satisfy_complete_plan(self):
        cells = v.matrix(1)
        rows = [dict(cell, repeat=0, status='PASS', output_sha256='a')
                for cell in cells for _ in range(3)]
        self.assertEqual(v.classify(rows, cells), 'INCONCLUSIVE')
        for i, row in enumerate(rows):
            row['repeat'] = i % 3
        self.assertEqual(v.classify(rows, cells), 'REPEATABLE_OBSERVATION_ONLY')

    def test_observation_metadata_missing_or_failed_cannot_pass(self):
        cell = dict(threads=2, affinity='3')
        good = dict(status='PASS', requested_threads=2, process_affinity='3', pixel_format='YU64',
                    width=1920, height=1080, pitch=7680, observed_decoder_threads=None,
                    thread_observation='INCONCLUSIVE: request only', sdk_api_header_version='header label',
                    sdk_source_baseline='11574d0295771edccadd17af14af74a539f924c7',
                    decoder_sdk_version_packed=0x10000, decoder_sdk_version_query_status=0)
        self.assertTrue(v.valid_observation(good, cell))
        for field in good:
            mutated = dict(good); del mutated[field]
            self.assertFalse(v.valid_observation(mutated, cell), field)
        for field, value in [('decoder_sdk_version_query_status', 1),
                             ('decoder_sdk_version_query_status', False),
                             ('decoder_sdk_version_packed', 0), ('observed_decoder_threads', 2),
                             ('requested_threads', '2'), ('sdk_source_baseline', 'other')]:
            self.assertFalse(v.valid_observation(dict(good, **{field: value}), cell), field)
        self.assertFalse(v.valid_observation([], cell))

    def test_timeouts_are_recorded_without_retry(self):
        inventory = v.subprocess.CompletedProcess([], 0, json.dumps(dict(status='PASS', cpu_isa={}, process_affinity='3')))
        with tempfile.TemporaryDirectory() as directory:
            sample = Path(directory) / 'sample'; sample.write_bytes(b'immutable')
            probe = Path(directory) / 'probe'; probe.write_bytes(b'probe')
            calls = [inventory] + [v.subprocess.TimeoutExpired('private-path', 1) for _ in range(18)]
            with patch.object(v.subprocess, 'run', side_effect=calls) as child:
                result = v.run(probe, sample, v.sha(sample), 1)
            self.assertEqual(child.call_count, 19)
            self.assertEqual(result['status'], 'FAIL')
            self.assertEqual(len(result['records']), 18)
            self.assertTrue(all(r['timed_out'] for r in result['records']))
            self.assertNotIn('private-path', json.dumps(result))

    def test_malformed_probe_does_not_publish_raw_output(self):
        inventory = v.subprocess.CompletedProcess([], 0, json.dumps(dict(status='PASS', cpu_isa={}, process_affinity='1')))
        invalid = v.subprocess.CompletedProcess([], 0, 'private-machine-path', 'private-user')
        with tempfile.TemporaryDirectory() as directory:
            sample = Path(directory) / 'sample'; sample.write_bytes(b'immutable')
            probe = Path(directory) / 'probe'; probe.write_bytes(b'probe')
            with patch.object(v.subprocess, 'run', side_effect=[inventory]+[invalid]*15):
                result = v.run(probe, sample, v.sha(sample))
            self.assertEqual(result['status'], 'FAIL')
            self.assertEqual(len(result['records']), 15)
            self.assertNotIn('private-', json.dumps(result))


if __name__ == '__main__':
    unittest.main()
