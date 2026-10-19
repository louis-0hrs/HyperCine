#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Unit checks for the RCA-160 band-0 evidence parser and verdict rules."""
import unittest

import band0_1080 as protocol


def arm(corrupt=0, waits=0, matched=None, entries=None, failures=0, timeouts=0):
    calls = 120
    if matched is None:
        matched = waits
    if entries is None:
        entries = calls * protocol.EXPECTED_GATE_ENTRIES
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
        'publishes_band0': calls * protocol.EXPECTED_PUBLISH_BAND0,
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


if __name__ == '__main__':
    unittest.main()
