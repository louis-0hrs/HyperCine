# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Bounded parser for the progressive GOP1 corpus framing, not a decoder.

Recognizes sample/level/subband size chunks and SDK metadata records. Entropy
bytes are opaque. Unexpected geometry/framing is rejected, never guessed from
byte patterns. No SDK headers or runtime are used.
"""
import hashlib
import struct

VARIABLE = {b'GUID': 'TAG_CLIP_GUID', b'DATE': 'TAG_ENCODE_DATE',
            b'TIME': 'TAG_ENCODE_TIME', b'TIMC': 'TAG_TIMECODE'}


def fnv64(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return '%016x' % value


def parse_sample(data):
    data = bytes(data)
    if len(data) % 4 or len(data) < 24 or data[:4] != b'\x00\x01\x00\x09':
        raise ValueError('unsupported/truncated sample')
    bands, masks = [], []
    state = {'channel': 0}

    def pair(offset):
        return struct.unpack_from('>hH', data, offset)

    def metadata(begin, end):
        at = begin
        while at < end:
            if end-at < 8:
                raise ValueError('truncated metadata record')
            name = data[at:at+4]
            size_type = struct.unpack_from('<I', data, at+4)[0]
            size = size_type & 0xffffff
            padded = (size+3) & ~3
            if at+8+padded > end:
                raise ValueError('metadata value exceeds chunk')
            if name in VARIABLE:
                if size_type >> 24 != (ord('G') if name == b'GUID' else ord('c')):
                    raise ValueError('unexpected variable metadata type')
                if (name == b'GUID' and size != 16) or (name != b'GUID' and not 1 <= size <= 32):
                    raise ValueError('unexpected variable metadata size')
                masks.append({'tag': VARIABLE[name], 'offset': at+8, 'length': padded})
            at += 8+padded

    def walk(begin, end, depth=0):
        if depth > 5:
            raise ValueError('excessive chunk nesting')
        at = begin
        while at < end:
            if end-at < 4:
                raise ValueError('truncated tag')
            tag, value = pair(at)
            code = -tag if tag < 0 else tag
            group = code & 0xff00
            if tag < 0 and group in (0x2000, 0x2100, 0x2200):
                limit = at+4+(((code & 255) << 16) | value)*4
                if limit > end:
                    raise ValueError('size chunk exceeds parent')
                if group == 0x2000:
                    if limit-at < 12:
                        raise ValueError('short subband chunk')
                    low = state.get('lowpass', False)
                    header, trailer = ((4, 0x0f0f), (4, 0x1b4b)) if low else ((55, 0), (56, 0))
                    if pair(at+4) != header or pair(limit-4) != trailer:
                        raise ValueError('unexpected band header/trailer')
                    width = state.get(27 if low else 49)
                    height = state.get(28 if low else 50)
                    if not width or not height:
                        raise ValueError('missing band geometry')
                    payload = data[at+8:limit-4]
                    if low and len(payload) != width*height*2:
                        raise ValueError('lowpass storage length mismatch')
                    bands.append({'channel': state['channel'], 'subband': 0 if low else state.get(51),
                                  'width': width, 'height': height, 'offset': at+8,
                                  'payload': payload, 'lowpass': low,
                                  'frame_start': state.get('band_start'),
                                  'chunk_offset': at, 'chunk_end': limit,
                                  'framed': data[state['band_start']:limit]})
                else:
                    walk(at+4, limit, depth+1)
                at = limit
                continue
            if tag == -0x4002:
                limit = at+4+value*4
                if limit > end:
                    raise ValueError('metadata chunk exceeds parent')
                metadata(at+4, limit)
                at = limit
                continue
            if tag == 2:
                # Index entries are raw 32-bit channel sizes, not tag tuples.
                if value != 3 or at+4+4*value > end:
                    raise ValueError('invalid three-channel index')
                at += 4+4*value
                continue
            if tag == 4 and value in (0x1a4a, 0x0e0e):
                state['band_start'] = at
            if tag == 62:
                if value not in (1, 2):
                    raise ValueError('unsupported channel')
                state['channel'] = value
            if tag == 25:
                state['lowpass'] = True
            if tag == 48:
                state['lowpass'] = False
            if tag in (27, 28, 49, 50, 51):
                state[tag] = value
            at += 4

    walk(0, len(data))
    if [(b['channel'], b['subband']) for b in bands] != [(c, s) for c in range(3) for s in range(10)]:
        raise ValueError('expected exactly 30 ordered subbands')
    return {'bands': bands, 'metadata_mask_ranges': masks}


def normalized_sample(data, parsed, expected_ranges=None):
    ranges = parsed['metadata_mask_ranges']
    if expected_ranges is not None and ranges != expected_ranges:
        raise ValueError('parsed metadata ranges differ from reference manifest')
    result = bytearray(data)
    for item in ranges:
        result[item['offset']:item['offset']+item['length']] = bytes(item['length'])
    return bytes(result)


def verify_sample(data, frame, kind):
    parsed = parse_sample(data)
    normalized = normalized_sample(data, parsed, frame['metadata_mask_ranges'])
    if hashlib.sha256(normalized).hexdigest() != frame[kind+'_normalized_sha256']:
        raise ValueError('normalized sample SHA mismatch')
    for actual, expected in zip(parsed['bands'], frame['bands']):
        if (actual['width'], actual['height']) != (expected['width'], expected['height']):
            raise ValueError('sample/manifest band geometry mismatch')
        if hashlib.sha256(actual['framed']).hexdigest() != expected[kind+'_payload_sha256']:
            raise ValueError('parsed payload SHA mismatch')
        if actual['lowpass']:
            p = actual['payload']
            little = b''.join(p[i:i+2][::-1] for i in range(0, len(p), 2))
            if fnv64(little) != expected[kind+'_coeff_fnv64']:
                raise ValueError('parsed lowpass coefficient FNV mismatch')
    return parsed, normalized


def reframe_band(reference, entropy):
    """Diagnostic wrapper only: reuse parsed headers, update subband word count."""
    if len(entropy) % 4:
        raise ValueError('entropy payload must be word aligned')
    framed = bytearray(reference['framed'])
    begin = reference['offset']-reference['frame_start']
    end = begin+len(reference['payload'])
    framed[begin:end] = entropy
    chunk = reference['chunk_offset']-reference['frame_start']
    words = (len(framed)-chunk-4)//4
    if words > 0xffffff:
        raise ValueError('subband exceeds size field')
    code = 0x2000 | (words >> 16)
    struct.pack_into('>hH', framed, chunk, -code, words & 65535)
    return bytes(framed)
