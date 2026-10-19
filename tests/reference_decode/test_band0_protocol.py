#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Unit checks for the RCA-160 band-0 evidence parser and verdict rules."""
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import analyze_rca160_correction as correction
import band0_1080 as protocol


def arm(corrupt=0, waits=0, matched=None, entries=None, failures=0, timeouts=0,
        publish_counts=None):
    calls = 120
    if matched is None:
        matched = waits
    if entries is None:
        entries = calls * protocol.EXPECTED_GATE_ENTRIES
    if publish_counts is None:
        publish_counts = {'11': calls}
    return {
        'calls': calls,
        'valid_outputs': calls - failures,
        'decode_failures': failures,
        'timeouts': 0,
        'distinct_outputs': 1,
        'modal_sha256': '0' * 64,
        'output_counts': {},
        'corrupt': corrupt,
        'gate_entries': entries,
        'premature_entries': waits,
        'real_waits': waits,
        'gate_timeouts': timeouts,
        'waits_matched': matched,
        'publishes_band0': sum(int(k) * v for k, v in publish_counts.items()),
        'publish_calls_band0': sum(int(k) * v for k, v in publish_counts.items()),
        'publish_call_counts': publish_counts,
        'transition_counts': publish_counts,
        'transition_pointer_counts': {'9': calls},
        'entropy_transitions_band0': calls * 3,
    }


def arms(**overrides):
    values = {'O': arm(corrupt=4), 'B': arm(waits=60), 'AP': arm(corrupt=1)}
    values.update(overrides)
    return values


class ParseTest(unittest.TestCase):
    def test_hex_flags_are_not_octal(self):
        record = protocol.parse_record(
            'GATE_ENTRY site=spatial ch=1 idx=1 strict=1 wptr=0000020B4E293E30 '
            'valid=0000000e started=0000000e num_bands=4 tid=10088 t_ns=255')
        self.assertEqual(record['valid'], 0xE)
        self.assertEqual(record['started'], 0xE)
        self.assertEqual(record['wptr'], 0x0000020B4E293E30)
        self.assertEqual(record['ch'], 1)


class CoverageTest(unittest.TestCase):
    def setUp(self):
        self.events = [
            protocol.parse_record(
                'GATE_ENTRY site=spatial ch=1 idx=1 strict=1 wptr=0000000000000100 '
                'valid=0000000e started=0000000e num_bands=4 tid=1 t_ns=100'),
            protocol.parse_record(
                'BAND_PUBLISH wptr=0000000000000100 band=0 old_valid=0000000e '
                'old_started=0000000e new_valid=0000000f new_started=0000000f tid=2 t_ns=150'),
            protocol.parse_record(
                'GATE_ADMIT site=spatial ch=1 idx=1 strict=1 wptr=0000000000000100 '
                'spins=5 elapsed_ms=0 final_valid=0000000f final_started=0000000f tid=1 t_ns=200'),
        ]

    def test_publication_inside_window_is_matched(self):
        coverage = protocol.producer_coverage(self.events)
        self.assertEqual(len(coverage['waits']), 1)
        self.assertTrue(coverage['coverage'][0]['producer_publish_matched'])

    def test_publication_after_admission_is_not_matched(self):
        self.events[1]['t_ns'] = 250
        coverage = protocol.producer_coverage(self.events)
        self.assertFalse(coverage['coverage'][0]['producer_publish_matched'])

    def test_other_image_is_not_matched(self):
        self.events[1]['wptr'] = 0x200
        coverage = protocol.producer_coverage(self.events)
        self.assertFalse(coverage['coverage'][0]['producer_publish_matched'])

    def test_already_valid_publication_is_not_a_transition(self):
        self.events[1]['old_valid'] = 0xF
        coverage = protocol.producer_coverage(self.events)
        self.assertFalse(coverage['coverage'][0]['producer_publish_matched'])

    def test_premature_entry_counted(self):
        coverage = protocol.producer_coverage(self.events)
        self.assertEqual(len(coverage['premature_entries']), 1)

    def test_publication_calls_and_transitions_are_counted_separately(self):
        noop = protocol.parse_record(
            'BAND_PUBLISH wptr=0000000000000100 band=0 old_valid=0000000f '
            'old_started=0000000f new_valid=0000000f new_started=0000000f tid=2 t_ns=160')
        coverage = protocol.producer_coverage(self.events + [noop])
        self.assertEqual(coverage['publish_calls_band0'], 2)
        self.assertEqual(coverage['transitions_band0'], 1)
        self.assertEqual(coverage['transition_pointers_band0'], 1)


class VerdictTest(unittest.TestCase):
    def test_registered_pass(self):
        verdict, reasons = protocol.classify(arms())
        self.assertEqual(verdict, 'CROSS_HOST_CONFIRMATION_PASS', reasons)

    def test_original_not_corrupt_is_inconclusive(self):
        verdict, reasons = protocol.classify(arms(O=arm(corrupt=0)))
        self.assertEqual(verdict, 'INCONCLUSIVE')
        self.assertTrue(any('O did not reproduce' in reason for reason in reasons))

    def test_return_control_not_corrupt_is_inconclusive(self):
        verdict, reasons = protocol.classify(arms(AP=arm(corrupt=0)))
        self.assertEqual(verdict, 'INCONCLUSIVE')
        self.assertTrue(any("A-prime did not reproduce" in reason for reason in reasons))

    def test_gate_failure_is_inconclusive(self):
        verdict, reasons = protocol.classify(arms(B=arm(corrupt=1, waits=60)))
        self.assertEqual(verdict, 'INCONCLUSIVE')
        self.assertTrue(any('still corrupted' in reason for reason in reasons))

    def test_no_wait_is_inconclusive(self):
        verdict, reasons = protocol.classify(arms(B=arm(waits=0)))
        self.assertEqual(verdict, 'INCONCLUSIVE')
        self.assertTrue(any('never actually waited' in reason for reason in reasons))

    def test_unmatched_wait_is_inconclusive(self):
        verdict, reasons = protocol.classify(arms(B=arm(waits=60, matched=59)))
        self.assertEqual(verdict, 'INCONCLUSIVE')
        self.assertTrue(any('no matched band-0 publication' in reason for reason in reasons))

    def test_incomplete_gate_entry_coverage_is_inconclusive(self):
        verdict, reasons = protocol.classify(arms(B=arm(waits=60, entries=1079)))
        self.assertEqual(verdict, 'INCONCLUSIVE')
        self.assertTrue(any('gate-entry coverage' in reason for reason in reasons))

    def test_decode_failure_is_inconclusive(self):
        verdict, reasons = protocol.classify(arms(B=arm(waits=60, failures=1)))
        self.assertEqual(verdict, 'INCONCLUSIVE')
        self.assertTrue(any('decode failure' in reason for reason in reasons))

    def test_missing_probe_log_in_one_call_is_inconclusive(self):
        records = [{'gate': {'gate_entries': protocol.EXPECTED_GATE_ENTRIES}}
                   for _ in range(119)]
        records.append({'gate': {'gate_entries': 0}})
        verdict, reasons = protocol.classify(arms(), records)
        self.assertEqual(verdict, 'INCONCLUSIVE')
        self.assertTrue(any('missing in at least one call' in reason
                            for reason in reasons))

    def test_per_call_entry_shortfall_is_inconclusive(self):
        records = [{'gate': {'gate_entries': protocol.EXPECTED_GATE_ENTRIES}}
                   for _ in range(119)]
        records.append({'gate': {'gate_entries': 8}})
        verdict, reasons = protocol.classify(arms(), records)
        self.assertEqual(verdict, 'INCONCLUSIVE')
        self.assertTrue(any('per-call gate-entry coverage' in reason
                            for reason in reasons))

    def test_registered_six_publication_rule_is_inconclusive(self):
        observed = {'9': 40, '10': 40, '11': 30, '12': 10}
        verdict, reasons = protocol.classify_registered_publication_rule(
            arms(O=arm(corrupt=4, publish_counts=observed),
                 B=arm(waits=60, publish_counts=observed),
                 AP=arm(corrupt=1, publish_counts=observed)))
        self.assertEqual(verdict, 'INCONCLUSIVE')
        self.assertEqual(len(reasons), 3)
        self.assertTrue(all('expected 6' in reason for reason in reasons))

    def test_corrected_rule_ignores_publication_count(self):
        observed = {'9': 40, '10': 40, '11': 30, '12': 10}
        verdict, reasons = protocol.classify(
            arms(O=arm(corrupt=4, publish_counts=observed),
                 B=arm(waits=60, publish_counts=observed),
                 AP=arm(corrupt=1, publish_counts=observed)))
        self.assertEqual(verdict, 'CROSS_HOST_CONFIRMATION_PASS', reasons)


def ledger_record(arm, call, digest=None, valid=True):
    record = {
        'arm': arm,
        'call': call,
        'timed_out': False,
        'output_valid': valid,
        'gate': {
            'gate_entries': 0, 'premature_entries': 0, 'waits': 0,
            'timeouts': 0, 'publishes_band0': 11, 'publish_calls_band0': 11,
            'transitions_band0': 11, 'transition_pointers_band0': 9,
            'waits_matched': 0, 'entropy_transitions_band0': 3,
        },
    }
    if digest is not None:
        record['output'] = {'sha256': digest, 'bytes': 1}
    return record


class CorrectionToolTest(unittest.TestCase):
    def run_tool(self, ledger, root):
        ledger_path = root / 'ledger.json'
        ledger_path.write_text(json.dumps(ledger), encoding='utf-8')
        json_path = root / 'correction.json'
        argv = ['analyze_rca160_correction.py',
                '--ledger', str(ledger_path),
                '--output-root', str(root / 'retained'),
                '--probe-root', str(root / 'retained'),
                '--json', str(json_path)]
        with mock.patch.object(sys, 'argv', argv):
            code = correction.main()
        self.assertTrue(json_path.is_file(), 'tool must write JSON on failure')
        return code, json.loads(json_path.read_text(encoding='utf-8'))

    def test_invalid_output_before_modal_record_does_not_crash(self):
        # The old generator scanned every record for output.sha256 and raised
        # KeyError on the leading digest-less record before reaching the modal.
        ledger = {'ledger': [
            ledger_record('O', 1, valid=False),
            ledger_record('O', 2, 'a' * 64),
            ledger_record('B', 1, 'b' * 64),
            ledger_record('AP', 1, 'c' * 64),
        ]}
        with tempfile.TemporaryDirectory() as directory:
            code, evidence = self.run_tool(ledger, Path(directory))
        self.assertEqual(code, 1)
        self.assertEqual(evidence['corrected_rule']['verdict'], 'INCONCLUSIVE')

    def test_modal_output_ignores_digest_less_record(self):
        records = [ledger_record('O', 1, valid=False),
                   ledger_record('O', 2, 'a' * 64),
                   ledger_record('O', 3, 'a' * 64)]
        modal = correction.modal_outputs(records, protocol.ARM_ORDER)
        self.assertEqual(modal['O'], ('a' * 64, 2))

    def test_missing_corrupt_artifact_still_writes_inconclusive(self):
        ledger = {'ledger': [
            ledger_record('O', 1, 'a' * 64),
            ledger_record('O', 2, 'd' * 64),
            ledger_record('B', 1, 'b' * 64),
            ledger_record('AP', 1, 'c' * 64),
            ledger_record('AP', 2, 'e' * 64),
        ]}
        with tempfile.TemporaryDirectory() as directory:
            code, evidence = self.run_tool(ledger, Path(directory))
        self.assertEqual(code, 1)
        self.assertEqual(evidence['corrected_rule']['verdict'], 'INCONCLUSIVE')
        self.assertTrue(any('artifact missing' in problem
                            for problem in evidence['artifacts']['problems']))
        self.assertEqual({entry['lane_identity_status']
                          for entry in evidence['corrupt_calls']},
                         {correction.ARTIFACT_MISSING})

    def test_worker_digest_is_not_reported_as_source_head(self):
        ledger = {'ledger': [], 'checks': {'worker_sha256': 'f' * 64}}
        with tempfile.TemporaryDirectory() as directory:
            code, evidence = self.run_tool(ledger, Path(directory))
        self.assertEqual(code, 1)
        self.assertEqual(evidence['worker_sha256'], 'f' * 64)
        self.assertIsNone(evidence['source_head'])
        self.assertNotIn('source_ledger_head', evidence)
        self.assertEqual(evidence['schema'],
                         'rca160_windows_coverage_correction_v2')


if __name__ == '__main__':
    unittest.main()
