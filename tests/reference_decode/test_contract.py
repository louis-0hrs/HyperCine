# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Synthetic vectors only; no native decoder result is represented here."""
import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path
import unittest
import contract


def observations():
    return [dict(process=p, ordinal=i, decoder_status=0, output_valid=True,
                 pixel_sha256='a' * 64, sample_sha256='b' * 64, psnr_db=44.47)
            for p in (1, 2) for i in (1, 2)]


def evidence():
    return dict(schema_version=1, sample_sha256='b' * 64,
                encoder_packets_equal=True, calls=observations())


class DecodeContract(unittest.TestCase):
    def test_packet_mismatch_always_fails_even_with_bad_decoder(self):
        for calls in ([], observations()):
            if calls:
                calls[0]['decoder_status'] = 3
                calls[1]['pixel_sha256'] = 'c' * 64
            self.assertEqual(contract.classify(False, calls), 'ENCODE_BITSTREAM_MISMATCH')

    def test_api_error_is_failure(self):
        calls = observations()
        calls[-1]['decoder_status'] = 7
        self.assertEqual(contract.classify(True, calls), 'REFERENCE_DECODE_ERROR')

    def test_successful_api_with_invalid_output_is_failure(self):
        calls = observations()
        calls[0]['output_valid'] = False
        self.assertEqual(contract.classify(True, calls), 'REFERENCE_DECODE_ERROR')

    def test_identical_packet_with_different_pixels_is_unresolved(self):
        doc = evidence()
        doc['calls'][1].update(pixel_sha256='c' * 64, psnr_db=12.8)
        result = contract.report(doc)
        self.assertEqual(result['classification'], 'REFERENCE_DECODER_NONDETERMINISTIC')
        self.assertTrue(result['release_blocked'])
        self.assertEqual(result['encoder_gate'], 'UNRESOLVED')
        self.assertEqual(result['encoder_packet_comparison'], 'MATCH')

    def test_stable_bad_quality_remains_failure(self):
        for psnr in (12.8, 35.0):
            calls = observations()
            for call in calls:
                call['psnr_db'] = psnr
            self.assertEqual(contract.classify(True, calls), 'DECODE_QUALITY_FAILURE_STABLE')

    def test_real_observed_gates_can_pass(self):
        self.assertEqual(contract.classify(True, observations()), 'PASS')

    def test_missing_encoder_or_repeat_evidence_never_passes(self):
        for parity, calls in ((None, observations()), (True, []),
                              (True, observations()[:1]), (True, observations()[:2])):
            self.assertEqual(contract.classify(parity, calls), 'REFERENCE_EVIDENCE_INCOMPLETE')
        for field in ('output_valid', 'pixel_sha256', 'psnr_db'):
            calls = observations()
            calls[0].pop(field)
            self.assertEqual(contract.classify(True, calls), 'REFERENCE_EVIDENCE_INCOMPLETE')

    def test_per_call_packet_change_always_fails(self):
        doc = evidence()
        doc['calls'][2]['sample_sha256'] = 'c' * 64
        self.assertEqual(contract.report(doc)['classification'], 'ENCODE_BITSTREAM_MISMATCH')

    def test_unavailable_alternate_is_explicit_and_release_blocking(self):
        result = contract.report(evidence())
        self.assertEqual(result['alternate_decoder']['status'], 'UNAVAILABLE')
        self.assertTrue(result['release_blocked'])

    def test_alternate_provider_strings_are_not_echoed(self):
        doc = evidence()
        doc['alternate_decoder'] = {'status': 'MEASURED', 'path': 'private-provider-path'}
        result = contract.report(doc)
        self.assertNotIn('private-provider-path', json.dumps(result))
        self.assertEqual(result['alternate_decoder']['acceptance'], 'UNREVIEWED')
        self.assertTrue(result['release_blocked'])

    def test_missing_evidence_file_is_generic_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'missing.json'
            run = subprocess.run([sys.executable, str(Path(contract.__file__)), str(path)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 2)
            self.assertEqual(json.loads(run.stdout)['classification'], 'INVALID_EVIDENCE')
            self.assertNotIn(str(path), run.stdout + run.stderr)

    def test_invalid_or_fabricated_measurement_shape_is_rejected(self):
        for value in (float('nan'), float('inf'), '44.47', True):
            calls = observations()
            calls[0]['psnr_db'] = value
            with self.assertRaises(ValueError):
                contract.classify(True, calls)
        calls = observations()
        calls.append(copy.deepcopy(calls[0]))
        with self.assertRaises(ValueError):
            contract.classify(True, calls)
        with self.assertRaises(ValueError):
            contract.classify(True, observations(), 34.99)
        with self.assertRaises(ValueError):
            contract.classify(True, observations() * 40)

    def test_exact_source_equality_has_portable_json_representation(self):
        calls = observations()
        for call in calls:
            call.update(psnr_db=None, source_pixels_exact=True)
        self.assertEqual(contract.classify(True, calls), 'PASS')

    def test_ambiguous_json_is_invalid_without_echoing_input(self):
        clean = json.dumps(evidence())
        documents = [clean.replace('"encoder_packets_equal": true',
                                  '"encoder_packets_equal": false, "encoder_packets_equal": true'),
                     clean.replace('"decoder_status": 0',
                                   '"decoder_status": 7, "decoder_status": 0', 1)]
        # Non-standard constants must be rejected even in unused metadata.
        documents += [clean[:-1] + ', "unused": ' + value + '}'
                      for value in ('NaN', 'Infinity', '-Infinity', '1e999')]
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'synthetic.json'
            for document in documents:
                with self.subTest(document=documents.index(document)):
                    path.write_text(document)
                    run = subprocess.run([sys.executable, str(Path(contract.__file__)), str(path)],
                                         capture_output=True, text=True)
                    self.assertEqual(run.returncode, 2)
                    self.assertEqual(json.loads(run.stdout)['classification'], 'INVALID_EVIDENCE')
                    self.assertNotIn(str(path), run.stdout + run.stderr)

    def test_cli_quarantine_and_malformed_evidence_are_nonzero(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'synthetic.json'
            malformed_calls = evidence()
            malformed_calls['calls'] = [None]
            for doc, code in ((evidence(), 0), ({'schema_version': 2}, 2),
                              (None, 2), ([], 2), (malformed_calls, 2)):
                path.write_text(json.dumps(doc))
                run = subprocess.run([sys.executable, str(Path(contract.__file__)), str(path)],
                                     capture_output=True, text=True)
                self.assertEqual(run.returncode, code)
                self.assertTrue(json.loads(run.stdout)['release_blocked'])
            doc = evidence()
            doc['calls'][0]['pixel_sha256'] = 'c' * 64
            path.write_text(json.dumps(doc))
            run = subprocess.run([sys.executable, str(Path(contract.__file__)), str(path)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 1)
            self.assertEqual(json.loads(run.stdout)['classification'], 'REFERENCE_DECODER_NONDETERMINISTIC')


if __name__ == '__main__':
    unittest.main()
