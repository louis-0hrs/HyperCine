# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""SDK/tool-free adversarial controls for the bounded #120 experiment."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import observe_ffmpeg_1080 as v


class Observation(unittest.TestCase):
    def test_incomplete_repeat_evidence_rejected(self):
        for hashes in ([], ['a'], ['a'] * 4, ['a'] * 6):
            with self.subTest(hashes=hashes), self.assertRaises(ValueError):
                v.stable(hashes)

    def test_one_bad_repeat_is_not_selected_away(self):
        with self.assertRaises(ValueError):
            v.stable(['a', 'a', 'b', 'a', 'a'])

    def test_stability_does_not_claim_qualification(self):
        result = v.stable(['a'] * 5)
        self.assertEqual(result['distinct'], 1)
        self.assertNotIn('qualified', result)

    def test_corrupt_original_refused_before_tool_execution(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            packet = root / 'packet'
            packet.write_bytes(b'bad')
            with patch.object(v, 'command') as cmd, self.assertRaises(ValueError):
                v.experiment(packet, root / 'source', root / 'ffmpeg', root / 'ffprobe', root / 'work')
            cmd.assert_not_called()
            self.assertFalse((root / 'work').exists())

    def test_same_size_original_tamper_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'input'
            path.write_bytes(b'abd')
            with self.assertRaises(ValueError):
                v.bound(path, 3, v.sha(b'abc'))

    def test_absent_candidate_is_explicitly_unavailable(self):
        with tempfile.TemporaryDirectory() as temporary, self.assertRaises(v.CandidateUnavailable):
            v.tool_identity(Path(temporary) / 'missing', 'ffmpeg')

    def test_unrecognized_banner_is_unavailable(self):
        with tempfile.TemporaryDirectory() as temporary:
            candidate = Path(temporary) / 'candidate'
            candidate.write_bytes(b'program')
            result = v.subprocess.CompletedProcess([], 0, stdout=b'not FFmpeg', stderr=b'')
            with patch.object(v, 'command', return_value=result), self.assertRaises(v.CandidateUnavailable):
                v.tool_identity(candidate, 'ffmpeg')

    def test_successful_mutation_with_truncated_pixels_is_fatal(self):
        with self.assertRaises(ValueError):
            v.mutation_result(0, b'bad', b'baseline')

    def test_successful_mutation_with_out_of_range_pixels_is_fatal(self):
        with patch.object(v, 'WIDTH', 2), patch.object(v, 'HEIGHT', 1):
            with self.assertRaises(ValueError):
                v.mutation_result(0, b'\xff\xff' * 4, b'baseline')

    def test_ignored_mutation_is_fatal(self):
        with patch.object(v, 'WIDTH', 2), patch.object(v, 'HEIGHT', 1):
            with self.assertRaises(ValueError):
                v.mutation_result(0, b'\0' * 8, b'\0' * 8)

    def test_real_nonzero_exit_is_decoder_rejection(self):
        self.assertEqual(v.mutation_result(1, None, b'baseline'), 'DECODER_REJECTED')

    def test_different_valid_pixels_discriminate(self):
        with patch.object(v, 'WIDTH', 2), patch.object(v, 'HEIGHT', 1):
            self.assertEqual(v.mutation_result(0, b'\1\0' * 4, b'\0' * 8), 'PIXELS_DIFFER')

    def test_source_orientation_hypotheses_are_separate(self):
        source = bytes([0, 0, 0, 255, 10, 10, 10, 255])
        with patch.object(v, 'PIXELS', len(source)):
            self.assertTrue(v.metrics(source, source)['rgb_exact'])
            self.assertFalse(v.metrics(source, source[4:] + source[:4])['rgb_exact'])

    def test_failed_control_remains_failure_despite_stable_positive(self):
        report = {'oracle_qualified': False, 'release_gate': 'BLOCKED',
                  'state': 'CONTROL_NOT_DISCRIMINATED'}
        self.assertEqual(v.observation_exit(report), 1)
        report['state'] = 'BLOCKED_ORACLE_NOT_QUALIFIED'
        self.assertEqual(v.observation_exit(report), 77)

    def test_observation_cannot_promote_oracle(self):
        for report in ({'oracle_qualified': True, 'release_gate': 'PASS',
                        'state': 'BLOCKED_ORACLE_NOT_QUALIFIED'},
                       {'oracle_qualified': False, 'release_gate': 'BLOCKED', 'state': 'PASS'}):
            with self.assertRaises(ValueError):
                v.observation_exit(report)


if __name__ == '__main__':
    unittest.main()
