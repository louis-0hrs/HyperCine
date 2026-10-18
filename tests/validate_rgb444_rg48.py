#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Replay pinned F1 RG48 sources through the SDK-free source importer only."""
import argparse
import array
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import sys
import tempfile

PINS = Path(__file__).parent / 'reference_rgb444' / 'rg48_f1_pins.json'


def checked_artifact(root, record):
    relative = PurePosixPath(record['path'])
    if (relative.is_absolute() or '..' in relative.parts or '\\' in record['path']
            or ':' in record['path']):
        raise ValueError('invalid relative artifact path')
    path = root.joinpath(*relative.parts)
    if not path.resolve().is_relative_to(root.resolve()):
        raise ValueError('artifact escapes corpus root')
    if path.stat().st_size != record['byte_length']:
        raise ValueError('artifact byte length mismatch: ' + record['path'])
    data = path.read_bytes()
    if len(data) != record['byte_length'] or hashlib.sha256(data).hexdigest() != record['sha256']:
        raise ValueError('artifact SHA-256 mismatch: ' + record['path'])
    return path, data


def replay(root, probe, pins):
    # Original F1 sources were written as native Windows little-endian uint16.
    # The public importer accepts native endian; reject a mismatched replay host.
    if sys.byteorder != 'little':
        raise ValueError('F1 native-endian replay requires a little-endian host')
    for manifest in pins['manifests']:
        checked_artifact(root, manifest)
    results = []
    for case in pins['cases']:
        w, h, pitch = case['width'], case['height'], case['source_pitch']
        if (type(w) is not int or type(h) is not int or type(pitch) is not int
                or not 0 < w <= 4096 or not 0 < h <= 4096 or w*h > 2097152
                or pitch < w*6 or pitch % 16 or pitch*h > 16*1024*1024):
            raise ValueError('invalid replay geometry')
        source_path = None
        source = None
        for name in ('source.bin', 'sdk.cfhd', 'sdk.normalized.cfhd', 'decoded.bin'):
            path, data = checked_artifact(root, case['artifacts'][name])
            if name == 'source.bin':
                source_path, source = path, data
        if len(source) != pitch*h:
            raise ValueError('source extent disagrees with layout')
        # Fresh output for each invocation: a successful exit cannot reuse a
        # previous frame. The probe itself performs no SHA/pin validation.
        with tempfile.TemporaryDirectory(prefix='hypercine-rg48-') as directory:
            output = Path(directory) / 'canonical.bin'
            subprocess.run([str(probe), '--export', str(w), str(h), str(pitch),
                            str(source_path), str(output)], check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
            raw = output.read_bytes()
        if len(raw) != w*h*6:
            raise ValueError('imported plane extent differs')
        words = array.array('H')
        words.frombytes(source)
        actual = array.array('H')
        actual.frombytes(raw)
        plane_results = []
        for c, channel in enumerate(('red', 'green', 'blue')):
            expected = case['canonical_p12_le_planes'][c]
            if expected['channel'] != channel or expected['byte_length'] != w*h*2:
                raise ValueError('invalid pinned plane domain')
            plane = raw[c*w*h*2:(c+1)*w*h*2]
            digest = hashlib.sha256(plane).hexdigest()
            if digest != expected['sha256']:
                raise ValueError('pinned canonical plane differs: ' + case['case_id'] + '/' + channel)
            # Independent integer division reference from every actual source
            # component; never use a decoded lossy SDK image as source truth.
            for y in range(h):
                row = y*pitch//2
                for x in range(w):
                    if actual[c*w*h+y*w+x] != words[row+x*3+c] // 16:
                        raise ValueError('source mismatch: %s %s x=%d y=%d' %
                                         (case['case_id'], channel, x, y))
            plane_results.append({'channel': channel, 'sha256': digest, 'byte_length': len(plane)})
        results.append({'case_id': case['case_id'], 'status': 'SOURCE_IMPORT_PASS',
                        'samples_compared': w*h*3, 'planes': plane_results})
        print('SOURCE_IMPORT_PASS ' + case['case_id'], flush=True)
    if not results:
        raise ValueError('empty source replay')
    return {'status': 'SOURCE_IMPORT_PASS', 'cases': results,
            'artifacts_verified': len(results)*4 + len(pins['manifests']),
            'samples_compared': sum(c['samples_compared'] for c in results),
            'sdk_decode_execution': 'NOT_RUN; retained original output verified only',
            'internal_stage_oracle': 'PENDING_SDK_INTERNAL_STAGE',
            'rgb444_bitstream_equality': 'NOT_RUN; no RGB encoder packet emitted'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--f1-root', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    result = replay(args.f1_root, args.probe.resolve(), json.loads(PINS.read_text()))
    if args.report:
        args.report.write_text(json.dumps(result, indent=2) + '\n')
    print('PASS %d cases; %d source samples; %d verified artifacts' %
          (len(result['cases']), result['samples_compared'], result['artifacts_verified']))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, KeyError, subprocess.SubprocessError):
        # Avoid echoing private paths embedded in OS/subprocess exceptions.
        print('FAIL RG48 replay: artifact, source, output or probe validation failed', file=sys.stderr)
        raise SystemExit(1)
