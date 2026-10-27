# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Bounded structural probe for original CineForm RGB444 p12 samples.

This is research code, not a decoder. It reads CFHD framing, validates the
three-full-resolution-channel contract, and hashes the opaque band payloads.
No entropy decode, pixel reconstruction or production codec path is used.

Source anchors (frozen upstream tree, read-only):
  * upstream/Codec/codec.h:197-341      codec tag numbering
  * upstream/Codec/codec.h:465-489      internal ENCODED_FORMAT values
  * upstream/Codec/frame.c:6334-6337    planes 0/1/2 store G/R/B
  * upstream/Codec/convert.c:6747-6753  inverse mapping G/R/B -> packed RGB
  * upstream/Codec/wavelet.h:312-315    LH=1, HL=2, HH=3
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

SAMPLE_TYPE_INTRA = 9
TRANSFORM_TYPE_SPATIAL = 0
ENCODED_FORMAT_YUV_422 = 1
ENCODED_FORMAT_BAYER = 2
ENCODED_FORMAT_RGB_444 = 3
ENCODED_FORMAT_RGBA_4444 = 4
PRECISION_P12 = 12

# Planes 0/1/2 are G/R/B in the original encoder (frame.c) and the original
# decoder maps them back in the same order (convert.c). This ordering is
# source-derived, not proven here by a pixel comparison.
CHANNEL_ORDER = ('G', 'R', 'B')

# Spatial wavelet orientations (wavelet.h).
BAND_LH, BAND_HL, BAND_HH = 1, 2, 3
SPATIAL_BANDS = (BAND_LH, BAND_HL, BAND_HH)

HEADER_TAGS = {
    1: 'sample_type', 10: 'transform_type', 11: 'num_frames',
    12: 'num_channels', 13: 'num_wavelets', 14: 'num_subbands',
    15: 'num_spatial', 16: 'first_wavelet', 20: 'width', 21: 'height',
    70: 'precision', 71: 'input_format', 84: 'encoded_format',
}
BAND_TAGS = {26: 'num_levels', 27: 'lowpass_width', 28: 'lowpass_height',
             35: 'pixel_depth', 37: 'wavelet_type', 38: 'wavelet_number',
             39: 'wavelet_level', 40: 'num_bands', 41: 'highpass_width',
             42: 'highpass_height', 48: 'band_number', 49: 'band_width',
             50: 'band_height', 51: 'band_subband'}


def avi_video_chunks(data):
    """Return video packet bytes from the first RIFF/AVI ``movi`` payloads."""
    data = bytes(data)
    if len(data) < 12 or data[:4] != b'RIFF' or data[8:12] != b'AVI ':
        raise ValueError('not a RIFF/AVI file')
    riff_size = struct.unpack_from('<I', data, 4)[0]
    end = 8 + riff_size
    if end > len(data):
        raise ValueError('truncated RIFF container')
    packets = []

    def walk(begin, limit):
        at = begin
        while at < limit:
            if limit - at < 8:
                raise ValueError('truncated RIFF chunk header')
            chunk_id = data[at:at+4]
            size = struct.unpack_from('<I', data, at+4)[0]
            payload = at + 8
            if payload + size > limit:
                raise ValueError('RIFF chunk exceeds parent')
            if chunk_id == b'LIST':
                if size < 4:
                    raise ValueError('short LIST chunk')
                walk(payload + 4, payload + size)
            elif chunk_id in (b'00dc', b'00db'):
                if size == 0:
                    raise ValueError('empty video chunk')
                packets.append(bytes(data[payload:payload+size]))
            at = payload + size + (size & 1)

    walk(12, end)
    if not packets:
        raise ValueError('no compressed video chunk')
    return packets


def parse_rgb444_sample(data):
    """Validate one progressive intra RGB444 p12 sample and return its map."""
    data = bytes(data)
    if len(data) % 4 or len(data) < 24 or data[:4] != b'\x00\x01\x00\x09':
        raise ValueError('unsupported/truncated RGB444 sample')
    header = {}
    index_sizes = []
    chunks = []
    state = {'channel': 0, 'lowpass': False}
    geometry = {}

    def pair(offset):
        return struct.unpack_from('>hH', data, offset)

    def walk(begin, end, depth=0):
        if depth > 5:
            raise ValueError('excessive chunk nesting')
        at = begin
        while at < end:
            if end - at < 4:
                raise ValueError('truncated tag')
            tag, value = pair(at)
            code = -tag if tag < 0 else tag
            group = code & 0xff00
            if tag < 0 and group in (0x2000, 0x2100, 0x2200):
                limit = at + 4 + (((code & 255) << 16) | value) * 4
                if limit > end or limit < at + 4:
                    raise ValueError('size chunk exceeds parent')
                if group == 0x2000:
                    if limit - at < 12:
                        raise ValueError('short subband chunk')
                    lowpass = state.get('lowpass', False)
                    expected = ((4, 0x0f0f), (4, 0x1b4b)) if lowpass else \
                               ((55, 0), (56, 0))
                    if pair(at + 4) != expected[0] or pair(limit - 4) != expected[1]:
                        raise ValueError('unexpected band header/trailer')
                    payload = data[at + 8:limit - 4]
                    if lowpass:
                        width = state.get(27)
                        height = state.get(28)
                        if not width or not height:
                            raise ValueError('missing lowpass geometry')
                        if len(payload) != width * height * 2:
                            raise ValueError('lowpass storage length mismatch')
                    else:
                        width = state.get(49)
                        height = state.get(50)
                        if not width or not height:
                            raise ValueError('missing band geometry')
                        if len(payload) % 4:
                            raise ValueError('highpass payload not word aligned')
                    chunks.append({
                        'channel': state.get('channel', 0),
                        'lowpass': lowpass,
                        'width': width, 'height': height,
                        'band': state.get(48), 'subband': state.get(51),
                        'level': state.get(39), 'wavelet_number': state.get(38),
                        'num_levels': state.get(26),
                        'payload_bytes': len(payload),
                        'payload_sha256': hashlib.sha256(payload).hexdigest(),
                        'chunk_offset': at, 'chunk_end': limit,
                    })
                else:
                    walk(at + 4, limit, depth + 1)
                at = limit
                continue
            if tag == -0x4002:
                limit = at + 4 + value * 4
                if limit > end:
                    raise ValueError('metadata chunk exceeds parent')
                at = limit
                continue
            if depth == 0 and tag == 2:
                # Index entries are big-endian 32-bit compressed channel sizes.
                if value != 3 or at + 4 + 4 * value > end:
                    raise ValueError('invalid three-channel index')
                index_sizes.extend(struct.unpack_from('>%dI' % value, data, at + 4))
                at += 4 + 4 * value
                continue
            if depth == 0 and code in HEADER_TAGS:
                header[HEADER_TAGS[code]] = value
            if code in BAND_TAGS:
                state[code] = value
            if tag == 62:
                state['channel'] = value
            elif tag == 25:
                state['lowpass'] = True
            elif tag == 48:
                state['lowpass'] = False
            at += 4

    walk(0, len(data))
    required = {'sample_type', 'transform_type', 'num_frames', 'num_channels',
                'num_wavelets', 'num_subbands', 'width', 'height', 'precision',
                'encoded_format'}
    missing = required - set(header)
    if missing:
        raise ValueError('missing header tags: ' + ', '.join(sorted(missing)))
    if header['sample_type'] != SAMPLE_TYPE_INTRA:
        raise ValueError('not an intra sample')
    if header['transform_type'] != TRANSFORM_TYPE_SPATIAL:
        raise ValueError('not a progressive spatial transform')
    if header['num_frames'] != 1:
        raise ValueError('not a GOP1 sample')
    if header['encoded_format'] != ENCODED_FORMAT_RGB_444:
        raise ValueError('not internal RGB_444 (encoded_format=%d)' %
                         header['encoded_format'])
    if header['num_channels'] != 3:
        raise ValueError('RGB_444 requires exactly three channels')
    if header['precision'] != PRECISION_P12:
        raise ValueError('not a p12 sample (precision=%d)' % header['precision'])
    if header['width'] <= 0 or header['height'] <= 0:
        raise ValueError('invalid frame geometry')
    if header['num_wavelets'] <= 0 or header['num_subbands'] < 2:
        raise ValueError('invalid wavelet/subband count')
    if len(index_sizes) != header['num_channels'] or min(index_sizes) <= 0:
        raise ValueError('invalid channel index sizes')

    by_channel = {index: [] for index in range(header['num_channels'])}
    for chunk in chunks:
        channel = chunk['channel']
        if channel not in by_channel:
            raise ValueError('channel index outside declared channel count')
        by_channel[channel].append(chunk)

    channels = []
    reference_lowpass = None
    reference_levels = {}
    reference_map = None
    for index in range(header['num_channels']):
        items = by_channel[index]
        low = [item for item in items if item['lowpass']]
        high = [item for item in items if not item['lowpass']]
        if len(low) != 1:
            raise ValueError('channel %d must have exactly one lowpass band' % index)
        if len(high) != header['num_subbands'] - 1:
            raise ValueError('channel %d has %d highpass bands, expected %d' %
                             (index, len(high), header['num_subbands'] - 1))
        subbands = sorted(item['subband'] for item in high)
        if subbands != list(range(1, header['num_subbands'])):
            raise ValueError('channel %d highpass subbands are not 1..%d' %
                             (index, header['num_subbands'] - 1))
        levels = {}
        mapping = []
        for item in high:
            if item['band'] not in SPATIAL_BANDS:
                raise ValueError('unsupported wavelet orientation')
            if item['level'] is None or not 1 <= item['level'] <= header['num_wavelets']:
                raise ValueError('wavelet level outside declared count')
            if (item['level'], item['band']) in levels:
                raise ValueError('duplicate level/orientation in channel')
            levels[(item['level'], item['band'])] = (item['width'], item['height'])
            mapping.append((item['level'], item['band'], item['subband']))
        for level in range(1, header['num_wavelets'] + 1):
            present = {band for (declared, band) in levels if declared == level}
            if present != set(SPATIAL_BANDS):
                raise ValueError('channel %d level %d lacks LH/HL/HH' % (index, level))
        mapping.sort()
        if reference_map is None:
            reference_map = mapping
        elif mapping != reference_map:
            raise ValueError('level/band/subband mapping differs between channels')
        low_geometry = (low[0]['width'], low[0]['height'])
        if reference_lowpass is None:
            reference_lowpass = low_geometry
        elif low_geometry != reference_lowpass:
            raise ValueError('channel lowpass geometry differs (not RGB444)')
        level_geometry = {}
        for (level, _band), geometry_value in levels.items():
            if level in level_geometry and level_geometry[level] != geometry_value:
                raise ValueError('level geometry differs between orientations')
            level_geometry[level] = geometry_value
        for level, geometry_value in level_geometry.items():
            if level in reference_levels and reference_levels[level] != geometry_value:
                raise ValueError('channel highpass geometry differs (not RGB444)')
            reference_levels[level] = geometry_value
        channels.append({
            'index': index,
            'plane': CHANNEL_ORDER[index],
            'lowpass': low[0],
            'highpass': sorted(high, key=lambda item: item['subband']),
            'lowpass_geometry': low_geometry,
            'level_geometry': {str(level): level_geometry[level]
                               for level in sorted(level_geometry)},
        })

    return {
        'sample_type': header['sample_type'],
        'transform_type': header['transform_type'],
        'num_frames': header['num_frames'],
        'encoded_format': header['encoded_format'],
        'encoded_format_name': 'RGB_444',
        'input_format': header.get('input_format'),
        'precision': header['precision'],
        'width': header['width'],
        'height': header['height'],
        'num_channels': header['num_channels'],
        'num_wavelets': header['num_wavelets'],
        'num_subbands': header['num_subbands'],
        'num_spatial': header.get('num_spatial'),
        'first_wavelet': header.get('first_wavelet'),
        'index_sizes': index_sizes,
        'band_count': len(chunks),
        'channels': channels,
    }


def probe_avi(path):
    """Probe every compressed video chunk in an AVI file."""
    data = Path(path).read_bytes()
    packets = avi_video_chunks(data)
    parsed = [parse_rgb444_sample(packet) for packet in packets]
    return {
        'container_sha256': hashlib.sha256(data).hexdigest(),
        'container_bytes': len(data),
        'packet_bytes': [len(packet) for packet in packets],
        'packets': parsed,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--avi', type=Path, help='RIFF/AVI input (read-only)')
    source.add_argument('--packet', type=Path, help='single CFHD sample input')
    parser.add_argument('--json', action='store_true', help='emit JSON summary')
    args = parser.parse_args(argv)
    if args.avi:
        result = probe_avi(args.avi)
    else:
        data = Path(args.packet).read_bytes()
        result = {
            'container_sha256': hashlib.sha256(data).hexdigest(),
            'container_bytes': len(data),
            'packet_bytes': [len(data)],
            'packets': [parse_rgb444_sample(data)],
        }
    if args.json:
        json.dump(result, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write('\n')
    else:
        for packet in result['packets']:
            print('%dx%d RGB444 p12 channels=%d bands=%d wavelets=%d subbands=%d' %
                  (packet['width'], packet['height'], packet['num_channels'],
                   packet['band_count'], packet['num_wavelets'],
                   packet['num_subbands']))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
