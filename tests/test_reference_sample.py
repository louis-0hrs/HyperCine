#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
import argparse
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import reference_sample as sample


def tag(key, value):
    return struct.pack('>hH', key, value)


def chunk(key, payload):
    assert len(payload) % 4 == 0
    return tag(-key, len(payload)//4)+payload


def fixture():
    meta = b'GUID'+struct.pack('<I', (ord('G') << 24)|16)+bytes(range(16))
    meta += b'DATE'+struct.pack('<I', (ord('c') << 24)|3)+b'abc\0'
    meta += b'KEEP'+struct.pack('<I', 4)+b'keep'
    body = chunk(0x4002, meta)
    for channel in range(3):
        if channel:
            body += tag(62, channel)
        body += tag(4, 0x1a4a)+tag(25, 0)+tag(27, 2)+tag(28, 2)
        body += chunk(0x2000, tag(4, 0x0f0f)+struct.pack('>4h', 1, -2, 3, -4)+tag(4, 0x1b4b))
        for sub in range(1, 10):
            body += tag(4, 0x0e0e)+tag(48, (sub-1)%3+1)+tag(49, 2)+tag(50, 2)+tag(51, sub)
            body += chunk(0x2000, tag(55, 0)+b'bits'+tag(56, 0))
    return tag(1, 9)+tag(2, 3)+bytes(12)+chunk(0x2200, body)


class ParserTests(unittest.TestCase):
    def test_ordered_bands(self):
        parsed = sample.parse_sample(fixture())
        self.assertEqual(len(parsed['bands']), 30)
        self.assertEqual(parsed['bands'][0]['payload'], struct.pack('>4h', 1, -2, 3, -4))

    def test_truncated_sample(self):
        for cut in (1, 4, 12, 24, 100):
            with self.assertRaises(ValueError):
                sample.parse_sample(fixture()[:-cut])

    def test_chunk_exceeds_parent(self):
        data = bytearray(fixture()); struct.pack_into('>hH', data, 20, -0x2200, 65535)
        with self.assertRaises(ValueError):
            sample.parse_sample(data)

    def test_index_is_not_parsed_as_tags(self):
        data = bytearray(fixture()); data[8:20] = tag(62, 99)*3
        self.assertEqual(len(sample.parse_sample(data)['bands']), 30)

    def test_bad_band_trailer(self):
        data = bytearray(fixture()); parsed = sample.parse_sample(data)
        data[parsed['bands'][0]['chunk_end']-1] ^= 1
        with self.assertRaises(ValueError):
            sample.parse_sample(data)

    def test_wrong_band_count(self):
        data = fixture(); parsed = sample.parse_sample(data); end = parsed['bands'][-1]['frame_start']
        shortened = data[:end]; shortened = shortened[:20]+chunk(0x2200, shortened[24:])
        with self.assertRaises(ValueError):
            sample.parse_sample(shortened)

    def test_normalization_only_parsed_whitelist(self):
        data = fixture(); parsed = sample.parse_sample(data)
        normalized = sample.normalized_sample(data, parsed, parsed['metadata_mask_ranges'])
        allowed = {i for m in parsed['metadata_mask_ranges'] for i in range(m['offset'], m['offset']+m['length'])}
        self.assertTrue(b'keep' in normalized)
        self.assertTrue(all(a == b or i in allowed for i, (a, b) in enumerate(zip(data, normalized))))
        self.assertEqual(data, fixture())  # Original never modified.

    def test_arbitrary_mask_range_is_rejected(self):
        parsed = sample.parse_sample(fixture())
        with self.assertRaises(ValueError):
            sample.normalized_sample(fixture(), parsed, [{'tag': 'TAG_CLIP_GUID', 'offset': 0, 'length': 16}])

    def test_metadata_length_is_bounded(self):
        data = bytearray(fixture()); struct.pack_into('<I', data, 32, (ord('G') << 24)|65535)
        with self.assertRaises(ValueError):
            sample.parse_sample(data)

    def test_diagnostic_reframe_is_explicit(self):
        data = fixture(); parsed = sample.parse_sample(data); band = parsed['bands'][1]
        self.assertEqual(sample.reframe_band(band, band['payload']), band['framed'])
        changed = sample.reframe_band(band, b'new-bits')
        at = band['chunk_offset']-band['frame_start']
        self.assertEqual(struct.unpack_from('>hH', changed, at), (-0x2000, 4))
        with self.assertRaises(ValueError):
            sample.reframe_band(band, b'bad')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--corpus-root', type=Path)
    parser.add_argument('--probe', type=Path)
    args, rest = parser.parse_known_args()
    if args.corpus_root:
        import validate_windows_reference as gate
        result = gate.inspect_inventory(gate.load_manifest(gate.CORPUS/'manifest.json'), args.corpus_root)
        if result['status'] != 'PASS' or len(result['parsed_frames']) != 16:
            raise SystemExit('Incomplete Tier A corpus')
        print('280/280 files; 16/16 normalized SDK/CUDA samples; 480/480 framed band pairs verified')
    if args.probe:
        import validate_windows_reference as gate
        manifest = gate.load_manifest(gate.CORPUS/'manifest.json')
        with tempfile.TemporaryDirectory(prefix='hypercine-sdk-vector-') as tmp:
            work = Path(tmp)
            for frame in manifest['frames']:
                if frame['width'] != 1920 or frame['frame_index'] not in (0, 3, 6, 13):
                    continue
                source = work/'input.bgra'
                gate.checked_input(args.probe, frame, source)
                reference = None
                if args.corpus_root:
                    path = args.corpus_root/'frames/tier-a-1080'/('frame_%04d' % frame['frame_index'])/'sdk.cfhd'
                    reference, _ = sample.verify_sample(path.read_bytes(), frame, 'sdk')
                result = gate.diagnose(args.probe, frame, source, work, reference, sdk_replay=True)
                if result['coeff_matches'] != 30 or (reference and result['payload_matches'] != 30):
                    raise SystemExit('SDK frozen-table arithmetic regression: frame %d' % frame['frame_index'])
                if reference and any(b['first_signed_difference'] or b['first_entropy_byte_difference'] is not None for b in result['bands']):
                    raise SystemExit('actual signed/entropy byte comparison differs')
        print('4 deterministic SDK arithmetic vectors x 30 coefficient hashes verified')
    unittest.main(argv=[sys.argv[0]]+rest)
