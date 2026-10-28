#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Real 4K FilmScan3 decode gate, optionally requiring audited CPU/Metal parity.

--require-metal requires the existing native stage-auditing Metal tool, a
separate CPU encoder, all full packet/trace comparisons and independent FFmpeg
decode of BOTH outputs. Portable unit tests are not GPU qualification.
"""
import argparse
import json
import re
import subprocess
import tempfile
from pathlib import Path

from validate_standalone import avi
from validation_support import require, integer, hex_digest

WIDTH, HEIGHT = 3840, 2160
EXPECTED_BYTES = WIDTH * HEIGHT * 4


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'duplicate trace key: ' + key)
        result[key] = value
    return result


def read_trace(directory, count):
    document = json.loads((directory / 'trace.json').read_text(), object_pairs_hook=unique_object)
    require(isinstance(document, dict) and document.get('profile') == 'FilmScan3', 'invalid trace profile')
    frames = document.get('frames')
    require(isinstance(frames, list) and len(frames) == count, 'missing encoder trace frames')
    for index, frame in enumerate(frames):
        require(isinstance(frame, dict), 'invalid trace frame')
        for key in ('frame_index', 'fs_rate_limiter', 'previous_sample_bytes', 'sample_bytes'):
            require(key in frame, 'missing trace field: ' + key)
            integer(frame[key])
        require(frame['frame_index'] == index, 'unordered/duplicate trace frame index')
        require(frame.get('coefficient_hash') == 'full', 'parity requires full coefficient hashing')
        for key, length in (('quant', 36), ('scale', 36), ('coefficient_fnv64', 30)):
            values = frame.get(key)
            require(isinstance(values, list) and len(values) == length, 'invalid trace table: ' + key)
            for value in values:
                if key == 'coefficient_fnv64':
                    hex_digest(value, 16)
                else:
                    integer(value, 0, 65535)
    return frames


def compare_outputs(cpu, metal, count):
    cpu_frames, metal_frames = read_trace(cpu, count), read_trace(metal, count)
    for index, (c, m) in enumerate(zip(cpu_frames, metal_frames)):
        name = 'frame_%04d.cfhd' % index
        a, b = (cpu / name).read_bytes(), (metal / name).read_bytes()
        if a != b:
            first = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
            raise ValueError('frame %d CPU/Metal packet differs at byte %d' % (index, first))
        require(bool(a), 'empty encoded packet')
        require(c['sample_bytes'] == len(a) and m['sample_bytes'] == len(b), 'trace sample length mismatch')
        for key in c.keys() | m.keys():
            require(key in c and key in m and c[key] == m[key],
                    'frame %d CPU/Metal trace mismatch: %s' % (index, key))


def require_metal_audit(stdout, count):
    # A device-name string alone is NOT execution evidence. The mandatory tool
    # constructs MetalFrontend (throws on unavailable GPU), compares actual CPU
    # and GPU color/wavelet buffers incl. padding, then reports these counters.
    require('Required Metal device: ' in stdout, 'mandatory Metal device missing')
    audits = re.findall(r'; audited frames=(\d+) levels=(\d+)\b', stdout)
    require(audits == [(str(count), str(count * 9))], 'missing/incomplete Metal stage audit')


def ffprobe_geometry(ffprobe, container):
    out = subprocess.check_output([str(ffprobe), '-v', 'error', '-show_streams',
                                   '-of', 'json', str(container)], timeout=120)
    streams = json.loads(out)['streams']
    require(len(streams) == 1, 'expected one decoded video stream')
    stream = streams[0]
    return stream['width'], stream['height'], stream['pix_fmt']


def check_decode(width, height, pix_fmt, size):
    require((width, height) == (WIDTH, HEIGHT), '4K decode geometry mismatch')
    require(pix_fmt == 'yuv422p10le', '4K decode pixel format mismatch')
    require(size == EXPECTED_BYTES, '4K decode storage mismatch')


def decode(ffmpeg, packet, width, height, directory):
    container = directory / 'probe.avi'
    container.write_bytes(avi(packet, width, height))
    raw = directory / 'probe.yuv'
    subprocess.run([str(ffmpeg), '-v', 'error', '-xerror', '-y', '-i', str(container),
                    '-frames:v', '1', '-pix_fmt', 'yuv422p10le', '-f', 'rawvideo',
                    str(raw)], check=True, timeout=120)
    return container, raw.stat().st_size


def make_inputs(root, count):
    # Four bounded, changing-complexity fixtures; at most 4*32 MiB input storage.
    # The 16-frame sequence repeats the ordered flat/ramp/edge/detail cycle,
    # WITHOUT resetting either encoder between frames. No golden data supplied.
    sources = []
    for pattern in ([1] if count == 1 else range(4)):
        source = root / ('input_%d.bgra' % pattern)
        with source.open('wb') as handle:
            for y in range(HEIGHT):
                if pattern == 0:
                    row = bytes((64, 96, 128, 255)) * WIDTH
                elif pattern == 1:
                    row = bytes((i * 37 + y) & 255 for i in range(WIDTH * 4))
                elif pattern == 2:
                    row = b''.join(bytes((v, v, v, 255)) for v in
                                   (255 if (x // 8 + y // 8) % 2 else 0 for x in range(WIDTH)))
                else:
                    row = bytes(((i * 73) ^ (y * 151) ^ ((i * y) >> 3)) & 255
                                for i in range(WIDTH * 4))
                handle.write(row)
        sources.append(source)
    return [sources[i % len(sources)] for i in range(count)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--encoder', required=True, type=Path)
    parser.add_argument('--cpu-encoder', type=Path)
    parser.add_argument('--require-metal', action='store_true')
    parser.add_argument('--frames', type=int, choices=(1, 16), default=1)
    parser.add_argument('--ffmpeg', required=True, type=Path)
    parser.add_argument('--ffprobe', required=True, type=Path)
    args = parser.parse_args()
    if args.require_metal:
        require(args.cpu_encoder is not None, 'mandatory Metal gate needs an independent CPU encoder')
        require(args.cpu_encoder.resolve() != args.encoder.resolve(), 'CPU and Metal encoder paths must differ')

    with tempfile.TemporaryDirectory(prefix='hypercine-fs3-4k-') as tmp:
        root = Path(tmp)
        inputs = make_inputs(root, args.frames)
        outputs = [('encoder', args.encoder, root / 'samples')]
        if args.cpu_encoder:
            outputs.append(('cpu', args.cpu_encoder, root / 'cpu-samples'))
        for label, encoder, directory in outputs:
            result = subprocess.run([str(encoder), '--geometry', '%dx%d' % (WIDTH, HEIGHT),
                                     '--coefficient-hash', 'full', str(directory)] + list(map(str, inputs)),
                                    check=True, capture_output=True, text=True, timeout=900)
            if label == 'encoder' and args.require_metal:
                require_metal_audit(result.stdout, args.frames)
        if args.cpu_encoder:
            compare_outputs(root / 'cpu-samples', root / 'samples', args.frames)
            pair = 'CPU/Metal' if args.require_metal else 'encoder/CPU (no GPU qualification)'
            print('PASS 4K %s comparison: %d complete packets, state, 36 quant/scale and 30 bands' % (pair, args.frames))
        for label, _, directory in outputs:
            trace = read_trace(directory, args.frames)
            for index, frame in enumerate(trace):
                packet = (directory / ('frame_%04d.cfhd' % index)).read_bytes()
                require(bool(packet) and len(packet) == frame['sample_bytes'], 'invalid encoded packet length')
                container, size = decode(args.ffmpeg, packet, WIDTH, HEIGHT, directory)
                check_decode(*ffprobe_geometry(args.ffprobe, container), size)
            print('PASS positive 4K: %s, %d decoded frames, %dx%d yuv422p10le, %d bytes/frame'
                  % (label, args.frames, WIDTH, HEIGHT, EXPECTED_BYTES))
        print('PASS 4K FFmpeg gate' + (' (required native Metal stage audit)' if args.require_metal else ' (no GPU qualification)'))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError, TypeError, OSError, subprocess.SubprocessError) as error:
        print('FAIL:', error)
        raise SystemExit(1)
