#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Pin the RGB444 p12 band table of all six profiles from original packets.

Issue #37 next action after the band-contract measurement (#316) was to derive
the format-specific 12-bit quality table the RGB444 encoder needs. That table
must come from original bytes, not from the p10 YUV profile tables.

Two layers are checked separately:

  * Synthetic tests exercise the extraction and the rejection paths without a
    corpus, so CI on a machine without the private reference tree still proves
    the tool cannot silently accept a foreign family or a partial frame.
  * The optional corpus test re-measures every `sdk.cfhd` under a configured F1
    root and asserts the result equals the table pinned in
    docs/validation/rgb444-p12-quality/report.json. A drift there means either
    the pinned table or the parser changed.

Nothing here encodes, transforms, quantizes coefficients or decodes pixels.
"""
import json
import os
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rgb444_band_contract as contract
from test_rgb444_probe import build_sample

# Profiles that the original corpus contains, in the SDK's numbering.
PROFILE_NAMES = {
    1: 'low', 2: 'medium', 3: 'high',
    4: 'filmscan1', 5: 'filmscan2', 6: 'filmscan3',
}

# The measured table, in stream order: level 3 LH/HL/HH, level 2 LH/HL/HH,
# level 1 LH/HL/HH, per plane in stream order G, R, B.
PINNED = {
    'low': {'green': [64, 64, 48, 64, 64, 48, 512, 512, 768],
            'chroma': [64, 64, 48, 64, 64, 48, 1024, 1024, 1536]},
    'medium': {'green': [48, 48, 32, 48, 48, 32, 256, 256, 384],
               'chroma': [48, 48, 32, 48, 48, 32, 512, 512, 768]},
    'high': {'green': [32, 32, 24, 32, 32, 24, 128, 128, 192],
             'chroma': [32, 32, 24, 32, 32, 24, 256, 256, 384]},
    'filmscan1': {'green': [24, 24, 12, 24, 24, 12, 96, 96, 144],
                  'chroma': [24, 24, 12, 24, 24, 12, 192, 192, 288]},
    'filmscan2': {'green': [16, 16, 8, 16, 16, 8, 32, 32, 48],
                  'chroma': [16, 16, 8, 16, 16, 8, 64, 64, 96]},
    'filmscan3': {'green': [16, 16, 8, 16, 16, 8, 24, 24, 36],
                  'chroma': [16, 16, 8, 16, 16, 8, 48, 48, 72]},
}

# Measured B_SCALE, identical for every profile and every plane.
PINNED_SCALE = [32, 32, 16, 8, 8, 4, 2, 2, 1]


def band_table(packet):
    """Return ({'green','chroma'}, scale) for one measured RGB_444 packet."""
    planes = []
    for channel in packet['channels']:
        planes.append([band['band_quantization'] for band in channel['bands']])
    scales = [band['band_scale'] for band in packet['channels'][0]['bands']]
    return {'green': planes[0], 'chroma': planes[1]}, scales, planes


class PinnedTableTests(unittest.TestCase):
    def test_pinned_tables_have_the_measured_shape(self):
        for name, table in PINNED.items():
            with self.subTest(profile=name):
                self.assertEqual(len(table['green']), 9)
                self.assertEqual(len(table['chroma']), 9)

    def test_level_one_chroma_is_twice_green_and_levels_two_three_are_equal(self):
        # This relation is what the merged #316 README stated as "three times"
        # at level 1. The bytes say twice, and levels 2/3 are identical. The
        # assertion is written from the measurement, so a document that repeats
        # the 3x figure fails here rather than propagating into the encoder.
        for name, table in PINNED.items():
            with self.subTest(profile=name):
                for slot in range(9):
                    green = table['green'][slot]
                    chroma = table['chroma'][slot]
                    if slot >= 6:
                        self.assertEqual(chroma, 2 * green,
                                         'level 1 chroma must be 2x green')
                        self.assertNotEqual(chroma, 3 * green,
                                            'level 1 chroma is not 3x green')
                    else:
                        self.assertEqual(chroma, green,
                                         'levels 2/3 must match green')

    def test_quantization_descends_with_the_level(self):
        # Coarse levels carry less quantization than the finest level. This is a
        # sanity relation on the measured numbers, not a reimplementation.
        for name, table in PINNED.items():
            with self.subTest(profile=name):
                green = table['green']
                self.assertLess(green[0], green[6])
                self.assertLessEqual(green[3], green[6])

    def test_profiles_are_ordered_by_quality(self):
        # Filmic profiles quantize least at level 1, then high/medium/low.
        order = ['filmscan3', 'high', 'medium', 'low']
        level1 = [PINNED[name]['green'][6] for name in order]
        self.assertEqual(level1, sorted(level1))
        self.assertLess(PINNED['filmscan3']['green'][6],
                        PINNED['filmscan1']['green'][6])

    def test_synthetic_sample_yields_a_table(self):
        packet = contract.measure_packet(build_sample())
        table, scales, planes = band_table(packet)
        self.assertEqual(len(table['green']), 9)
        self.assertEqual(len(scales), 9)
        # The synthetic sample carries one value for every plane, so the
        # extraction must return three identical planes rather than shifting.
        self.assertEqual(planes[0], planes[1])
        self.assertEqual(planes[1], planes[2])

    def test_dropping_a_band_is_rejected_not_shortened(self):
        # A table with eight entries would still index, so the contract check
        # has to be the thing that fails. Verify the underlying parser refuses
        # a sample whose declaration does not match its bytes.
        with self.assertRaises(contract.ContractError):
            contract.measure_packet(build_sample(duplicate_subband=True))

    def test_foreign_families_are_refused_by_name(self):
        for encoded_format in (1, 2, 4):
            with self.subTest(encoded_format=encoded_format):
                with self.assertRaises(contract.ContractError):
                    contract.measure_packet(build_sample(encoded_format=encoded_format))

    def test_ten_bit_is_not_a_p12_packet(self):
        with self.assertRaises(contract.ContractError):
            contract.measure_packet(build_sample(precision=10))

    def test_corpus_reproduces_the_pinned_table(self):
        # Optional: needs the private F1 tree. The pinned values above are the
        # published measurement; this test re-derives them from the bytes.
        root = os.environ.get('HYPERCINE_RGB444_F1_ROOT')
        if not root:
            self.skipTest('no HYPERCINE_RGB444_F1_ROOT configured')
        base = Path(root)
        if not base.is_dir():
            self.skipTest('configured F1 root is not a directory')

        seen = {}
        measured = 0
        for path in sorted(base.rglob('sdk.cfhd')):
            parts = path.relative_to(base).parts
            profiles = [p for p in parts if p in PROFILE_NAMES.values()]
            if not profiles:
                continue
            try:
                packet = contract.measure_packet(path.read_bytes())
            except contract.ContractError:
                continue  # another family in the same tree; not this contract
            table, scales, planes = band_table(packet)
            self.assertEqual(scales, PINNED_SCALE,
                             '%s: declared scale drifted' % path)
            # Levels 2 and 3 (slots 0..5) are identical across all three planes;
            # only the finest level carries the doubled chroma value.
            for slot in range(6):
                self.assertEqual(planes[0][slot], planes[1][slot],
                                 '%s: R differs from G at levels 2/3' % path)
            for slot in range(6, 9):
                self.assertEqual(planes[1][slot], 2 * planes[0][slot],
                                 '%s: level-1 R is not 2x G' % path)
            self.assertEqual(planes[1], planes[2],
                             '%s: R differs from B' % path)
            seen.setdefault(profiles[0], set()).add(
                (tuple(table['green']), tuple(table['chroma'])))
            measured += 1

        self.assertGreater(measured, 0, 'no RGB_444 packet found under the root')
        for name, variants in seen.items():
            with self.subTest(profile=name):
                self.assertEqual(len(variants), 1,
                                 '%s: packets disagree with each other' % name)
                green, chroma = next(iter(variants))
                self.assertEqual(list(green), PINNED[name]['green'])
                self.assertEqual(list(chroma), PINNED[name]['chroma'])


if __name__ == '__main__':
    unittest.main()
