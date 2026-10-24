#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Deterministic tests for the isolated RGB444 p12 packet probe."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rgb444_probe as probe

ROOT = Path(__file__).resolve().parents[2]
FROZEN_AVI = ROOT/'upstream/Example/videos/CFHDsample444.AVI'
FROZEN_422_AVI = ROOT/'upstream/Example/videos/CFHDsample422.AVI'
FROZEN_AVI_SHA256 = 'ce018f56f9b1aa29d8844722be506315837d948eaa5da3d5c7e4cbd53818c5ea'


def tag(key, value):
    return struct.pack('>hH', key, value)


def chunk(key, payload):
    assert len(payload) % 4 == 0
    words = len(payload)//4
    high = (key & 0xff00) | (words >> 16)
    return struct.pack('>hH', -high, words & 0xffff) + payload


def halve(value):
    return (value + 1)//2


def scaled(width, height, levels):
    for _ in range(levels):
        width, height = halve(width), halve(height)
    return width, height


def build_sample(channels=3, wavelets=3, width=1920, height=1080,
                 precision=12, encoded_format=3, num_frames=1,
                 channel_geometry=None, channel_tag=None,
                 duplicate_subband=False, omit_lowpass_channel=None,
                 bad_trailer=False):
    """Build a source-shaped synthetic sample; never a real codec output."""
    subbands = 1 + 3*wavelets
    lowpass_width, lowpass_height = scaled(width, height, wavelets)
    body = bytearray()
    for channel in range(channels):
        if channel:
            body += tag(62, (channel_tag or {}).get(channel, channel))
        geometry = (channel_geometry or {}).get(channel)
        if channel != omit_lowpass_channel:
            low_w, low_h = geometry['lowpass'] if geometry else (lowpass_width, lowpass_height)
            payload = bytes([0x10 + channel])*(low_w*low_h*2)
            trailer = (4, 0x1b4b) if not bad_trailer else (4, 0x1b4c)
            body += tag(4, 0x1a4a)+tag(25, 0)+tag(26, wavelets)
            body += tag(27, low_w)+tag(28, low_h)
            body += chunk(0x2000, tag(4, 0x0f0f)+payload+tag(*trailer))
        for level in range(wavelets, 0, -1):
            if geometry:
                high_w, high_h = geometry['levels'][level]
            else:
                high_w, high_h = scaled(width, height, level)
            for band in probe.SPATIAL_BANDS:
                subband = 1 if duplicate_subband and channel == 1 else \
                          band + 3*(wavelets - level)
                body += tag(4, 0x0e0e)
                body += tag(37, 3)+tag(38, band)+tag(39, level)+tag(40, 4)
                body += tag(41, high_w)+tag(42, high_h)
                body += tag(48, band)+tag(49, high_w)+tag(50, high_h)+tag(51, subband)
                body += tag(52, 3)+tag(53, 48)+tag(54, 32)+tag(72, 0)
                payload = struct.pack('>I', 0x01020304 + channel*16 + level*4 + band)
                body += chunk(0x2000, tag(55, 0)+payload+tag(56, 0))
    header = tag(1, 9)+tag(2, 3)+struct.pack('>3I', 1000, 1200, 1400)
    header += tag(10, 0)+tag(11, num_frames)+tag(12, channels)
    header += tag(71, 120)+tag(84, encoded_format)
    header += tag(13, wavelets)+tag(14, subbands)+tag(15, 2)+tag(16, 3)
    header += tag(20, width)+tag(21, height)+tag(70, precision)
    return header + chunk(0x2200, bytes(body))


def riff_chunk(chunk_id, payload):
    data = chunk_id + struct.pack('<I', len(payload)) + payload
    return data + (b'\x00' if len(payload) & 1 else b'')


def build_avi(*packets):
    movi = b'movi' + b''.join(riff_chunk(b'00dc', packet) for packet in packets)
    body = riff_chunk(b'LIST', movi)
    return b'RIFF' + struct.pack('<I', len(body) + 4) + b'AVI ' + body


class FrozenSampleTests(unittest.TestCase):
    def test_frozen_avi_is_the_expected_original_sample(self):
        self.assertTrue(FROZEN_AVI.is_file())
        result = probe.probe_avi(FROZEN_AVI)
        self.assertEqual(result['container_sha256'], FROZEN_AVI_SHA256)
        self.assertEqual(len(result['packets']), 24)
        self.assertEqual(result['packet_bytes'][:2], [890036, 238268])

    def test_all_packets_are_rgb444_p12_with_three_full_channels(self):
        result = probe.probe_avi(FROZEN_AVI)
        for packet in result['packets']:
            self.assertEqual(packet['encoded_format'], probe.ENCODED_FORMAT_RGB_444)
            self.assertEqual(packet['encoded_format_name'], 'RGB_444')
            self.assertEqual(packet['input_format'], 120)
            self.assertEqual(packet['precision'], 12)
            self.assertEqual(packet['width'], 1920)
            self.assertEqual(packet['height'], 1080)
            self.assertEqual(packet['num_channels'], 3)
            self.assertEqual(packet['num_wavelets'], 3)
            self.assertEqual(packet['num_subbands'], 10)
            self.assertEqual(packet['band_count'], 30)
            self.assertEqual([channel['plane'] for channel in packet['channels']],
                             ['G', 'R', 'B'])
            for channel in packet['channels']:
                self.assertEqual(len(channel['highpass']), 9)
                self.assertEqual([band['subband'] for band in channel['highpass']],
                                 list(range(1, 10)))
                self.assertEqual(channel['lowpass_geometry'], (240, 135))
                self.assertEqual(channel['level_geometry'],
                                 {'1': (960, 540), '2': (480, 270), '3': (240, 135)})
                self.assertEqual(channel['lowpass']['payload_bytes'], 240*135*2)
                self.assertTrue(all(band['payload_bytes'] % 4 == 0
                                    for band in channel['highpass']))

    def test_first_packet_index_and_payload_evidence(self):
        packet = probe.probe_avi(FROZEN_AVI)['packets'][0]
        self.assertEqual(packet['index_sizes'], [203628, 493076, 192532])
        first = packet['channels'][0]
        self.assertEqual(first['lowpass']['payload_sha256'],
                         'c9a2c5b37499be763b8860eec5dc348fb9f557b9bba148fd0ee1cb7adf3a5cfb')
        self.assertEqual(first['highpass'][0]['payload_sha256'],
                         '4d317463dbcbada627800f866d782a821c81884b61f8af3e8d7e77cd0985d746')

    def test_band_mapping_is_derived_not_hardcoded_30(self):
        packet = probe.probe_avi(FROZEN_AVI)['packets'][0]
        expected = sorted((level, band, band + 3*(3-level))
                          for level in (1, 2, 3) for band in (1, 2, 3))
        actual = sorted((band['level'], band['band'], band['subband'])
                        for band in packet['channels'][0]['highpass'])
        self.assertEqual(actual, expected)

    def test_four_level_layout_is_accepted(self):
        packet = probe.parse_rgb444_sample(build_sample(wavelets=4))
        self.assertEqual(packet['num_wavelets'], 4)
        self.assertEqual(packet['num_subbands'], 13)
        self.assertEqual(packet['band_count'], 39)
        for channel in packet['channels']:
            self.assertEqual(len(channel['highpass']), 12)
            self.assertEqual([band['subband'] for band in channel['highpass']],
                             list(range(1, 13)))

    def test_synthetic_three_level_layout_is_accepted(self):
        packet = probe.parse_rgb444_sample(build_sample())
        self.assertEqual(packet['band_count'], 30)


class RejectionTests(unittest.TestCase):
    def test_rejects_yuv422(self):
        with self.assertRaises(ValueError):
            probe.parse_rgb444_sample(build_sample(encoded_format=1))

    def test_rejects_frozen_yuv422_sample(self):
        packets = probe.avi_video_chunks(FROZEN_422_AVI.read_bytes())
        with self.assertRaises(ValueError):
            probe.parse_rgb444_sample(packets[0])

    def test_rejects_four_channels(self):
        with self.assertRaises(ValueError):
            probe.parse_rgb444_sample(build_sample(channels=4))

    def test_rejects_ten_bit(self):
        with self.assertRaises(ValueError):
            probe.parse_rgb444_sample(build_sample(precision=10))

    def test_rejects_gop_length(self):
        with self.assertRaises(ValueError):
            probe.parse_rgb444_sample(build_sample(num_frames=2))

    def test_rejects_channel_index_overflow(self):
        with self.assertRaises(ValueError):
            probe.parse_rgb444_sample(build_sample(channel_tag={1: 3}))

    def test_rejects_half_resolution_channel(self):
        packet = build_sample(channel_geometry={
            2: {'lowpass': (240, 135), 'levels': {
                1: (960, 540), 2: (480, 270), 3: (120, 68)}},
        })
        with self.assertRaises(ValueError):
            probe.parse_rgb444_sample(packet)

    def test_rejects_duplicate_subband(self):
        with self.assertRaises(ValueError):
            probe.parse_rgb444_sample(build_sample(duplicate_subband=True))

    def test_rejects_missing_lowpass(self):
        with self.assertRaises(ValueError):
            probe.parse_rgb444_sample(build_sample(omit_lowpass_channel=1))

    def test_rejects_bad_trailer(self):
        with self.assertRaises(ValueError):
            probe.parse_rgb444_sample(build_sample(bad_trailer=True))

    def test_rejects_truncation_and_misalignment(self):
        for packet in (build_sample()[:-1], build_sample()[:-4]):
            with self.assertRaises(ValueError):
                probe.parse_rgb444_sample(packet)

    def test_rejects_non_intra_signature(self):
        packet = bytearray(build_sample())
        packet[1] = 2
        with self.assertRaises(ValueError):
            probe.parse_rgb444_sample(packet)


class AviTests(unittest.TestCase):
    def test_returns_all_video_chunks(self):
        packets = probe.avi_video_chunks(build_avi(b'abcd', b'efgh'))
        self.assertEqual(packets, [b'abcd', b'efgh'])

    def test_handles_odd_chunk_padding(self):
        packets = probe.avi_video_chunks(build_avi(b'abcde', b'fg'))
        self.assertEqual(packets, [b'abcde', b'fg'])

    def test_rejects_non_avi(self):
        with self.assertRaises(ValueError):
            probe.avi_video_chunks(b'not an avi container')

    def test_rejects_truncated_container(self):
        data = build_avi(b'abcd')
        with self.assertRaises(ValueError):
            probe.avi_video_chunks(data[:-2])

    def test_rejects_empty_video_chunk(self):
        with self.assertRaises(ValueError):
            probe.avi_video_chunks(build_avi(b''))


if __name__ == '__main__':
    unittest.main()
