# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Measure the encoded band contract of original RGB444 p12 packets.

This is a measurement tool for issue #37, not a decoder and not an encoder.
It answers one question from real bytes only: for every channel, wavelet level
and spatial orientation, which band quantization/scale values and which band
geometry did the original SDK actually write?

It deliberately does not model the transform. Reading `B_QUANT` tells you what
the encoder chose; it does not tell you how the coefficients were produced.
That distinction is what keeps this from being mistaken for encoder math.

Source anchors are the frozen upstream tree (read-only):
  * upstream/Codec/codec.h            codec tag numbering
  * upstream/Codec/codec.h:465-489   internal ENCODED_FORMAT values
  * upstream/Codec/frame.c:6334-6337  planes 0/1/2 store G/R/B
  * upstream/Codec/wavelet.h:312-315  LH=1, HL=2, HH=3
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

SAMPLE_TYPE_INTRA = 9
ENCODED_FORMAT_RGB_444 = 3
ENCODED_FORMAT_NAMES = {1: 'YUV_422', 2: 'BAYER', 3: 'RGB_444', 4: 'RGBA_4444'}
PRECISION_P12 = 12

CHANNEL_ORDER = ('G', 'R', 'B')
BAND_LH, BAND_HL, BAND_HH = 1, 2, 3
ORIENTATION_NAMES = {BAND_LH: 'LH', BAND_HL: 'HL', BAND_HH: 'HH'}

# Scalar tags that carry the declarations this measurement relies on. Negative
# (chunk) tags are handled separately because they carry a nested extent.
TAGS = {
    1: 'sample_type', 2: 'index', 10: 'transform_type', 11: 'num_frames',
    12: 'num_channels', 13: 'num_wavelets', 14: 'num_subbands',
    15: 'num_spatial', 16: 'first_wavelet', 20: 'width', 21: 'height',
    24: 'group_trailer', 25: 'lowpass_subband', 26: 'num_levels',
    27: 'lowpass_width', 28: 'lowpass_height', 33: 'pixel_offset',
    34: 'lowpass_quantization', 35: 'pixel_depth', 37: 'wavelet_type',
    38: 'wavelet_number', 39: 'wavelet_level', 40: 'num_bands',
    41: 'highpass_width', 42: 'highpass_height', 45: 'lowpass_scale',
    48: 'band_number', 49: 'band_width', 50: 'band_height',
    51: 'band_subband', 52: 'band_encoding', 53: 'band_quantization',
    54: 'band_scale', 55: 'band_header', 62: 'channel', 69: 'frame_number',
    70: 'precision', 71: 'input_format', 72: 'band_coding_flags',
    79: 'version', 80: 'quality_l', 81: 'quality_h', 83: 'prescale_table',
    84: 'encoded_format', 85: 'display_height',
}

# Chunk group ids: 0x2000 band payload, 0x2100 level container, 0x2200 sample.
GROUP_BAND, GROUP_LEVEL, GROUP_SAMPLE = 0x2000, 0x2100, 0x2200


class ContractError(ValueError):
    pass


def _pair(data, offset):
    return struct.unpack_from('>hH', data, offset)


def measure_packet(data):
    """Walk one RGB444 p12 sample and return its measured band contract."""
    data = bytes(data)
    if len(data) % 4 or len(data) < 24:
        raise ContractError('packet is not word aligned or too short')
    if data[:4] != b'\x00\x01\x00\x09':
        raise ContractError('not an intra CFHD sample')

    header = {}
    index_sizes = []
    bands = []
    state = {'channel': 0, 'lowpass': False}

    def walk(begin, end, depth=0):
        if depth > 5:
            raise ContractError('excessive chunk nesting')
        at = begin
        while at < end:
            if end - at < 4:
                raise ContractError('truncated tag')
            tag, value = _pair(data, at)
            code = -tag if tag < 0 else tag
            group = code & 0xff00
            if tag < 0 and group in (GROUP_BAND, GROUP_LEVEL, GROUP_SAMPLE):
                limit = at + 4 + (((code & 0xff) << 16) | value) * 4
                if limit > end or limit < at + 4:
                    raise ContractError('size chunk exceeds parent')
                if group == GROUP_BAND:
                    lowpass = state.get('lowpass', False)
                    payload = data[at + 8:limit - 4]
                    if lowpass:
                        width, height = state.get('lowpass_width'), state.get('lowpass_height')
                        if not width or not height:
                            raise ContractError('missing lowpass geometry')
                        if len(payload) != width * height * 2:
                            raise ContractError('lowpass storage length mismatch')
                    else:
                        width, height = state.get('band_width'), state.get('band_height')
                        if not width or not height:
                            raise ContractError('missing band geometry')
                        if len(payload) % 4:
                            raise ContractError('highpass payload is not word aligned')
                    channel_index = state.get('channel', 0)
                    if not 0 <= channel_index < 3:
                        # RGBA_4444 and Bayer declare four or more channels. This
                        # tool measures the three-plane RGB_444 family, so the
                        # sample is refused by name instead of indexing past the
                        # three-plane domain.
                        raise ContractError(
                            'sample declares channel %d; RGB_444 has three planes'
                            % channel_index)
                    bands.append({
                        'channel_index': channel_index,
                        'channel': CHANNEL_ORDER[channel_index],
                        'lowpass': lowpass,
                        'width': width, 'height': height,
                        # A lowpass band declares its own quantization/pixel depth;
                        # it never carries the highpass band tags. Reading those
                        # from carried-over state would attribute one channel's
                        # highpass values to the next channel's lowpass.
                        'band_number': None if lowpass else state.get('band_number'),
                        'orientation': (None if lowpass else
                                        ORIENTATION_NAMES.get(state.get('band_number'))),
                        'subband': state.get('lowpass_subband') if lowpass
                                   else state.get('band_subband'),
                        'level': None if lowpass else state.get('wavelet_level'),
                        'wavelet_number': None if lowpass else state.get('wavelet_number'),
                        'wavelet_type': None if lowpass else state.get('wavelet_type'),
                        'num_levels': state.get('num_levels'),
                        'num_bands': None if lowpass else state.get('num_bands'),
                        'band_encoding': None if lowpass else state.get('band_encoding'),
                        'band_quantization': None if lowpass else state.get('band_quantization'),
                        'band_scale': None if lowpass else state.get('band_scale'),
                        'coding_flags': None if lowpass else state.get('band_coding_flags'),
                        'lowpass_quantization': state.get('lowpass_quantization') if lowpass else None,
                        'pixel_depth': state.get('pixel_depth') if lowpass else None,
                        'payload_bytes': len(payload),
                        'payload_sha256': hashlib.sha256(payload).hexdigest(),
                    })
                else:
                    walk(at + 4, limit, depth + 1)
                at = limit
                continue
            if tag == -0x4002:
                limit = at + 4 + value * 4
                if limit > end:
                    raise ContractError('metadata chunk exceeds parent')
                at = limit
                continue
            if depth == 0 and tag == 2:
                # Accept any bounded entry count here and validate it after the
                # header is known. Raising on the count alone would reject a
                # four-channel RGBA_4444 or a Bayer packet as "invalid three
                # channel index", hiding the real reason: wrong family.
                if not 0 < value <= 8 or at + 4 + 4 * value > end:
                    raise ContractError('invalid sample index table')
                index_sizes.extend(struct.unpack_from('>%dI' % value, data, at + 4))
                at += 4 + 4 * value
                continue
            name = TAGS.get(code)
            if name is None:
                at += 4
                continue
            if depth == 0:
                header[name] = value
            elif name in ('num_levels', 'lowpass_width', 'lowpass_height',
                          'wavelet_type', 'wavelet_number', 'wavelet_level',
                          'num_bands', 'highpass_width', 'highpass_height',
                          'lowpass_scale', 'band_number', 'band_width',
                          'band_height', 'band_subband', 'band_encoding',
                          'band_quantization', 'band_scale', 'band_header',
                          'band_coding_flags', 'lowpass_subband', 'channel',
                          'pixel_depth', 'lowpass_quantization'):
                state[name] = value
                if name == 'lowpass_subband':
                    # Highpass band tags never apply to the following lowpass.
                    state['lowpass'] = True
                    for stale in ('band_number', 'band_subband', 'wavelet_level',
                                  'wavelet_number', 'wavelet_type', 'num_bands',
                                  'band_encoding', 'band_quantization', 'band_scale',
                                  'band_coding_flags', 'band_width', 'band_height',
                                  'highpass_width', 'highpass_height'):
                        state.pop(stale, None)
                elif name == 'band_number':
                    state['lowpass'] = False
            if tag == 62:
                state['channel'] = value
            at += 4

    walk(0, len(data))

    required = ('sample_type', 'transform_type', 'num_frames', 'num_channels',
                'num_wavelets', 'num_subbands', 'width', 'height', 'precision',
                'encoded_format')
    missing = [name for name in required if name not in header]
    if missing:
        raise ContractError('missing header tags: ' + ', '.join(sorted(missing)))
    if header['sample_type'] != SAMPLE_TYPE_INTRA:
        raise ContractError('not an intra sample')
    if header['encoded_format'] != ENCODED_FORMAT_RGB_444:
        raise ContractError('not internal RGB_444 (encoded_format=%d, %s)'
                            % (header['encoded_format'],
                               ENCODED_FORMAT_NAMES.get(header['encoded_format'],
                                                        'unknown family')))
    if header['num_frames'] != 1:
        raise ContractError('not a GOP1 sample')
    if header['num_channels'] != 3:
        raise ContractError('RGB_444 requires exactly three channels, sample declares %d'
                            % header['num_channels'])
    if header['precision'] != PRECISION_P12:
        raise ContractError('not a p12 sample')
    if len(index_sizes) != 3 or min(index_sizes) <= 0:
        raise ContractError('sample index does not carry three positive channel sizes')

    # Group the bands per channel and check the declaration against the bytes.
    channels = {}
    for band in bands:
        channels.setdefault(band['channel_index'], []).append(band)
    summary = []
    for index in range(3):
        items = channels.get(index, [])
        low = [b for b in items if b['lowpass']]
        high = [b for b in items if not b['lowpass']]
        if len(low) != 1:
            raise ContractError('channel %d must have exactly one lowpass band' % index)
        if len(high) != header['num_subbands'] - 1:
            raise ContractError('channel %d declares %d highpass bands but carries %d'
                                % (index, header['num_subbands'] - 1, len(high)))
        subbands = sorted(b['subband'] for b in high)
        if subbands != list(range(1, header['num_subbands'])):
            raise ContractError('channel %d highpass subbands are not 1..%d'
                                % (index, header['num_subbands'] - 1))
        measured_levels = {}
        for band in high:
            if band['orientation'] is None:
                raise ContractError('highpass band without a spatial orientation')
            key = (band['level'], band['orientation'])
            measured_levels[key] = band
        for level in range(1, header['num_wavelets'] + 1):
            present = {o for (declared, o) in measured_levels if declared == level}
            if present != {'LH', 'HL', 'HH'}:
                raise ContractError('channel %d level %d lacks LH/HL/HH' % (index, level))
        summary.append({
            'index': index,
            'plane': CHANNEL_ORDER[index],
            'declared_index_bytes': index_sizes[index],
            'lowpass': {k: low[0][k] for k in
                        ('width', 'height', 'payload_bytes', 'payload_sha256',
                         'lowpass_quantization', 'pixel_depth')},
            'bands': [{k: band[k] for k in
                       ('level', 'orientation', 'subband', 'width', 'height',
                        'band_encoding', 'band_quantization', 'band_scale',
                        'coding_flags', 'payload_bytes', 'payload_sha256')}
                      for band in sorted(high, key=lambda b: b['subband'])],
        })

    return {
        'sample_type': header['sample_type'],
        'transform_type': header['transform_type'],
        'encoded_format': header['encoded_format'],
        'encoded_format_name': ENCODED_FORMAT_NAMES.get(header['encoded_format']),
        'input_format': header.get('input_format'),
        'precision': header['precision'],
        'width': header['width'],
        'height': header['height'],
        'num_channels': header['num_channels'],
        'num_wavelets': header['num_wavelets'],
        'num_subbands': header['num_subbands'],
        'num_spatial': header.get('num_spatial'),
        'first_wavelet': header.get('first_wavelet'),
        'quality_l': header.get('quality_l'),
        'quality_h': header.get('quality_h'),
        'prescale_table': header.get('prescale_table'),
        'declared_band_total': header['num_channels'] * header['num_subbands'],
        'measured_band_total': len(bands),
        'channels': summary,
    }


def quant_table_from_contract(packet):
    """Return the 36 declared band quant entries in CFHD channel-major order.

    CFHD writes three channels of three levels of four entries (LL + LH + HL +
    HH). The lowpass carries no `band_quantization`, so slot 0 stays None; the
    comparable part is the nine highpass slots per channel, placed at the same
    channel-major indices the shared calculator uses.
    """
    orientation_slot = {'LH': 1, 'HL': 2, 'HH': 3}
    table = []
    for channel in packet['channels']:
        per_channel = [None] * 12
        for band in channel['bands']:
            slot = (band['level'] - 1) * 4 + orientation_slot[band['orientation']]
            per_channel[slot] = band['band_quantization']
        table.extend(per_channel)
    return table


def measure_case(packet_path):
    data = Path(packet_path).read_bytes()
    packet = measure_packet(data)
    return {
        'packet_bytes': len(data),
        'packet_sha256': hashlib.sha256(data).hexdigest(),
        'packet': packet,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--packet', type=Path, help='single CFHD sample (read-only)')
    source.add_argument('--f1-root', type=Path,
                        help='F1 corpus root, measured read-only')
    parser.add_argument('--json', type=Path, help='write the measured contract here')
    args = parser.parse_args(argv)

    if args.packet:
        result = measure_case(args.packet)
        if args.json:
            args.json.write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
        packet = result['packet']
        print('%dx%d RGB444 p12 channels=%d subbands=%d bands=%d quality_l=%s' % (
            packet['width'], packet['height'], packet['num_channels'],
            packet['num_subbands'], packet['measured_band_total'], packet['quality_l']))
        return 0

    root = args.f1_root
    measurements = []
    for path in sorted(root.rglob('sdk.cfhd')):
        try:
            measured = measure_case(path)
        except ContractError as error:
            measurements.append({'path': str(path.relative_to(root)),
                                 'status': 'REJECTED', 'reason': str(error)})
            continue
        measurements.append({
            'path': str(path.relative_to(root)),
            'status': 'MEASURED',
            'packet_bytes': measured['packet_bytes'],
            'packet_sha256': measured['packet_sha256'],
            'quality_l': measured['packet']['quality_l'],
            'num_subbands': measured['packet']['num_subbands'],
            'measured_band_total': measured['packet']['measured_band_total'],
            'bands': [[{k: band[k] for k in ('level', 'orientation', 'subband',
                                             'width', 'height',
                                             'band_quantization', 'band_scale')}
                       for band in channel['bands']]
                      for channel in measured['packet']['channels']],
        })
    if not measurements:
        raise ContractError('no sdk.cfhd under the given root')
    accepted = [m for m in measurements if m['status'] == 'MEASURED']
    if not accepted:
        raise ContractError('no packet satisfied the RGB444 p12 contract')
    result = {
        'status': 'MEASURED',
        'scope': 'declared band geometry and quant/scale values read from original '
                 'packets; no transform, entropy or pixel claim',
        'root_kind': 'F1 corpus (private, resolved through HYPERCINE_GOLDEN_DIR)',
        'packets': len(measurements),
        'measured': len(accepted),
        'rejected': len(measurements) - len(accepted),
        'cases': measurements,
    }
    if args.json:
        args.json.write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    print('MEASURED %d/%d packets' % (len(accepted), len(measurements)))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ContractError, OSError):
        print('FAIL band contract measurement', file=sys.stderr)
        raise SystemExit(1)
