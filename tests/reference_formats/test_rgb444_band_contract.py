#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Deterministic tests for the RGB444 p12 band-contract measurement.

The suite separates two things a naive probe conflates: accepting a
source-shaped synthetic sample (a parser test), and the measured contract of an
original packet (a corpus test). Nothing here encodes, transforms or decodes.
"""
import os
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rgb444_band_contract as contract
from test_rgb444_probe import build_sample, tag, chunk


class BandContractTests(unittest.TestCase):
    def test_synthetic_sample_is_measured_not_rejected(self):
        measured = contract.measure_packet(build_sample())
        self.assertEqual(measured['encoded_format'], contract.ENCODED_FORMAT_RGB_444)
        self.assertEqual(measured['num_channels'], 3)
        self.assertEqual(measured['num_subbands'], 10)
        self.assertEqual(measured['measured_band_total'], 30)
        self.assertEqual(measured['declared_band_total'], 30)
        for channel in measured['channels']:
            self.assertEqual(len(channel['bands']), 9)
            self.assertEqual({b['orientation'] for b in channel['bands']},
                             {'LH', 'HL', 'HH'})
            self.assertEqual({b['level'] for b in channel['bands']}, {1, 2, 3})

    def test_channel_planes_are_green_red_blue(self):
        measured = contract.measure_packet(build_sample())
        self.assertEqual([c['plane'] for c in measured['channels']], ['G', 'R', 'B'])

    def test_lowpass_does_not_inherit_highpass_band_tags(self):
        # Regression: the first channel's lowpass must not report the highpass
        # quantization/scale of a band that appeared earlier in the stream. That
        # would silently attribute one channel's values to another's lowpass.
        measured = contract.measure_packet(build_sample())
        for channel in measured['channels']:
            self.assertIsNone(channel['lowpass']['lowpass_quantization'],
                              'synthetic lowpass carries no QUANTIZATION tag')
            self.assertIsNone(channel['lowpass']['pixel_depth'])
            for band in channel['bands']:
                self.assertIsNotNone(band['band_quantization'])

    def test_quant_table_places_highpass_at_channel_major_slots(self):
        measured = contract.measure_packet(build_sample())
        table = contract.quant_table_from_contract(measured)
        self.assertEqual(len(table), 36)
        # Slot 0 of each channel block is the lowpass, which declares no
        # highpass quantization.
        for channel in range(3):
            self.assertIsNone(table[channel * 12])
            for level in range(3):
                for band in range(1, 4):
                    self.assertIsNotNone(table[channel * 12 + level * 4 + band])

    def test_wrong_encoded_format_is_rejected(self):
        for encoded_format in (1, 2, 4):
            with self.subTest(encoded_format=encoded_format):
                with self.assertRaises(contract.ContractError):
                    contract.measure_packet(build_sample(encoded_format=encoded_format))

    def test_wrong_precision_and_gop_are_rejected(self):
        with self.assertRaises(contract.ContractError):
            contract.measure_packet(build_sample(precision=10))
        with self.assertRaises(contract.ContractError):
            contract.measure_packet(build_sample(num_frames=2))

    def test_channel_count_must_be_three(self):
        with self.assertRaises(contract.ContractError):
            contract.measure_packet(build_sample(channels=2))

    def test_missing_band_must_not_be_silently_accepted(self):
        # A declared subband count that the bytes do not carry is a rejection,
        # never a partial PASS with fewer bands. Two distinct corruptions are
        # checked because they fail at different validation layers: a duplicated
        # subband breaks the 1..n-1 sequence, a dropped lowpass breaks the
        # one-lowpass-per-channel rule.
        with self.assertRaises(contract.ContractError):
            contract.measure_packet(build_sample(duplicate_subband=True))
        with self.assertRaises(contract.ContractError):
            contract.measure_packet(build_sample(wavelets=3, omit_lowpass_channel=1))

    def test_non_word_aligned_packet_is_rejected(self):
        with self.assertRaises(contract.ContractError):
            contract.measure_packet(b'not a cfhd sample at all')

    def test_original_corpus_contract_when_available(self):
        # Optional: only runs where a private F1 corpus is configured. The real
        # measured values are pinned in docs/validation/rgb444-band-contract/.
        root = os.environ.get('HYPERCINE_RGB444_F1_ROOT')
        if not root:
            self.skipTest('no HYPERCINE_RGB444_F1_ROOT configured')
        packet = Path(root) / 'RGB_444/f1/1920x1080/RG48/filmscan1/flat/sdk.cfhd'
        if not packet.is_file():
            self.skipTest('pinned F1 packet not present')
        measured = contract.measure_packet(packet.read_bytes())
        self.assertEqual(measured['encoded_format_name'], 'RGB_444')
        self.assertEqual((measured['width'], measured['height']), (1920, 1080))
        self.assertEqual(measured['num_subbands'], 10)
        self.assertEqual(measured['measured_band_total'], 30)
        # One lowpass per channel plus nine highpass bands, not a 40-band scheme.
        for channel in measured['channels']:
            self.assertEqual(len(channel['bands']), 9)


if __name__ == '__main__':
    unittest.main()
