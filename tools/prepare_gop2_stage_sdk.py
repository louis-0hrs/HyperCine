#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Add observation calls ONLY to new private copies of two frozen SDK units.

No decoder algorithm, flag, validity gate, arithmetic or upstream file is changed.
The resulting private objects overlay the two corresponding objects of the
already pinned pristine SDK archive. Every byte change is indexed by SHA-256.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

TREE = '837528ef9e9a55c416e9592ecb54e1c8f1ff9cf8'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def instrument_decoder(data):
    anchor = b'\t// Was the subband successfully decoded?'
    if data.count(anchor) != 1:
        raise ValueError('decoder anchor count mismatch')
    hook = (b'\t/* HyperCine #189: copy finished coefficients before inverse submission. */\n'
            b'\tif (result && transform->type == TRANSFORM_TYPE_FIELDPLUS && subband >= 0 && subband < 17)\n'
            b'\t\tHyperCineGop2ObserveBand(decoder, wavelet, channel, subband, index, band);\n\n')
    prefix = (b'extern void HyperCineGop2ObserveBand(void*, void*, int, int, int, int);\n'
              b'extern void HyperCineGop2ObserveSignedSpatial(void*, int, void*, int);\n')
    data = data.replace(anchor, hook + anchor)
    anchor = (b'\t\t// Save the codec error code in the decoder state and return\n'
              b'\t\tdecoder->error = error;\n\t\treturn;')
    if data.count(anchor) != 1:
        raise ValueError('native frame return anchor count mismatch')
    hook = (b'\t\t// Save the codec error code in the decoder state and return\n'
            b'\t\tdecoder->error = error;\n'
            b'\t\tif (error == CODEC_ERROR_OKAY && info->format == DECODED_FORMAT_YU64 && codec->progressive &&\n'
            b'\t\t\tcodec->precision == 10 && transform_array[0]->type == TRANSFORM_TYPE_FIELDPLUS)\n'
            b'\t\t\tHyperCineGop2ObserveSignedSpatial(decoder, frame, output, pitch);\n\t\treturn;')
    return prefix + data.replace(anchor, hook)


def instrument_wavelet(data):
    begin = data.index(b'// Invert a spatial wavelet transform to packed 10-bit pixels.')
    end = data.index(b'//#endif', begin)
    part = data[begin:end]
    # Each original row strip is copied before the original packer receives it.
    anchor = b'ConvertYUVStripPlanarToV210(plane_array, plane_pitch, strip, output_row_ptr,'
    if part.count(anchor) != 3:
        raise ValueError('wavelet strip anchor count mismatch')
    hook = (b'HyperCineGop2ObserveRows(plane_array, plane_pitch, frame_index, 2 * row, info->width, strip.height);\n\t'
            + anchor)
    part = part.replace(anchor, hook)
    prefix = b'extern void HyperCineGop2ObserveRows(void*, int*, int, int, int, int);\n'
    return prefix + data[:begin] + part + data[end:]


def prepare(root, destination):
    root = root.resolve(strict=True)
    destination = destination.resolve()
    destination.relative_to((root / 'build').resolve())
    if destination.exists():
        raise ValueError('private SDK destination exists')
    tree = subprocess.check_output(['git', 'rev-parse', 'HEAD:upstream'], cwd=root, text=True).strip()
    if tree != TREE:
        raise ValueError('unrecognized frozen SDK tree')
    subprocess.run(['git', 'diff', '--quiet', 'HEAD', '--', 'upstream'], cwd=root, check=True)
    records = []
    prepared = []
    for name, patcher in (('decoder.c', instrument_decoder), ('wavelet.c', instrument_wavelet)):
        original = (root / 'upstream/Codec' / name).read_bytes().replace(b'\r\n', b'\n')
        blob = subprocess.check_output(['git', 'show', 'HEAD:upstream/Codec/' + name], cwd=root)
        if original != blob.replace(b'\r\n', b'\n'):
            raise ValueError('frozen source content differs')
        patched = patcher(original)
        prepared.append((name, patched))
        records.append({'id': 'Codec/' + name, 'original_sha256': sha(original),
                        'instrumented_sha256': sha(patched), 'original_byte_length': len(original),
                        'instrumented_byte_length': len(patched)})
    (destination / 'Codec').mkdir(parents=True)
    for name, data in prepared:
        (destination / 'Codec' / name).write_bytes(data)
    index = {'schema': 'hypercine.gop2.sdk_observer/1', 'version': 2,
             'license': 'MIT OR Apache-2.0', 'sdk_revision': '11574d0295771edccadd17af14af74a539f924c7',
             'upstream_tree': TREE, 'observer_only': True, 'tracked_upstream_modified': False,
             'sources': records, 'pre_inverse_capture': 'DecodeSampleSubband after entropy, before inverse submission',
             'signed_capture': 'Alternate original TransformInverseSpatialToV210 before ConvertYUVStripPlanarToV210, all three row-strip sites; require packed output equals original default path',
             'line_endings': 'LF-normalized source copies'}
    (destination / 'observer.json').write_text(json.dumps(index, indent=2) + '\n', encoding='utf-8')
    return index


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    try:
        result = prepare(args.root, args.out)
        print(json.dumps({'status': 'PRIVATE_OBSERVER_PREPARED', 'sources': result['sources']}))
        return 0
    except (OSError, ValueError, subprocess.SubprocessError):
        print('Private observer preparation failed; upstream untouched.', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
