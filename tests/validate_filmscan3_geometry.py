#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Independent FFmpeg geometry, frame count and p10 quality gate for P4."""
import argparse
import array
import sys
import hashlib
import json
import math
import re
from pathlib import Path
import platform
import struct
import subprocess
import tempfile
from reference_sample import parse_sample
from validate_standalone import chunk


def avi(packets, width=3840, height=2160):
    count = len(packets)
    size = max(map(len, packets))
    avih = struct.pack('<14I', 40000, 0, 0, 0, count, 0, 1, size, width, height, 0, 0, 0, 0)
    strh = struct.pack('<4s4sIHH8I4h', b'vids', b'CFHD', 0, 0, 0, 0, 1, 25, 0, count,
                       size, 0xffffffff, 0, 0, 0, width, height)
    strf = struct.pack('<IiiHH4sIiiII', 40, width, height, 1, 24, b'CFHD', size, 0, 0, 0, 0)
    hdrl = chunk(b'LIST', b'hdrl' + chunk(b'avih', avih) +
                 chunk(b'LIST', b'strl' + chunk(b'strh', strh) + chunk(b'strf', strf)))
    return chunk(b'RIFF', b'AVI ' + hdrl + chunk(b'LIST', b'movi' + b''.join(chunk(b'00dc', p) for p in packets)))


def sha(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024*1024), b''):
            value.update(block)
    return value.hexdigest()


def verify_sequence(path, expected_hashes, frame_bytes):
    """Compare decoded frames in order, using at most 1 MiB of read storage."""
    if type(frame_bytes) is not int or frame_bytes <= 0:
        raise ValueError('invalid sequence frame extent')
    if (type(expected_hashes) is not list or not expected_hashes or
            any(type(value) is not str or re.fullmatch('[0-9a-f]{64}', value) is None
                for value in expected_hashes)):
        raise ValueError('empty or invalid sequence hash inventory')
    hashes = []
    with path.open('rb') as stream:
        for index, expected in enumerate(expected_hashes):
            value = hashlib.sha256()
            remaining = frame_bytes
            while remaining:
                block = stream.read(min(remaining, 1024 * 1024))
                if not block:
                    raise ValueError('truncated sequence frame ' + str(index))
                value.update(block)
                remaining -= len(block)
            actual = value.hexdigest()
            if actual != expected:
                raise ValueError('sequence frame ' + str(index) + ' differs from independent decode')
            hashes.append(actual)
        if stream.read(1):
            raise ValueError('trailing decoded sequence data')
    return hashes


def probe(executable, path, frames):
    info = json.loads(subprocess.check_output([executable, '-v', 'error', '-count_frames',
        '-show_entries', 'stream=width,height,pix_fmt,nb_read_frames', '-of', 'json', str(path)]))
    streams = info['streams']
    if len(streams) != 1 or streams[0] != dict(width=3840, height=2160, pix_fmt='yuv422p10le', nb_read_frames=str(frames)):
        raise ValueError('wrong native CFHD format/geometry/framecount: ' + str(info))
    return streams[0]


def decoded_psnr(actual_bytes, reference_bytes):
    if not actual_bytes or len(actual_bytes) % 2 or len(reference_bytes) % 2:
        raise ValueError('empty or odd p10 storage')
    actual = array.array('H', actual_bytes)
    expected = array.array('H', reference_bytes)
    if sys.byteorder != 'little':
        actual.byteswap();expected.byteswap()
    if len(actual) != len(expected) or any(value > 1023 for value in actual) or any(value > 1023 for value in expected):
        raise ValueError('invalid decoded p10 values/length')
    square = sum((int(a)-int(b))**2 for a, b in zip(actual, expected))
    psnr = 10*math.log10(1023.0**2*len(actual)/square) if square else math.inf
    if psnr <= 35:
        raise ValueError('4K p10 PSNR <=35 dB: ' + str(psnr))
    return psnr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--encoder', required=True)
    parser.add_argument('--ffmpeg', required=True)
    parser.add_argument('--ffprobe', required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    report = dict(status='PASS', geometry=[3840, 2160], source_sdk='REFERENCE_BLOCKED',
                  eight_k='PENDING_8K', system=platform.platform(), seed='0x1337BEEF', frames=[])
    report['ffmpeg_version'] = subprocess.check_output([args.ffmpeg, '-version'], text=True).splitlines()[0]
    with tempfile.TemporaryDirectory(prefix='fs3-geometry-') as temporary:
        work = Path(temporary)
        subprocess.run([args.encoder, '--export', str(work)], check=True)
        try:
            import resource
            rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
            report['encoder_peak_rss_bytes'] = rss if platform.system() == 'Darwin' else rss*1024
            report['peak_rss_method'] = 'maximum child RSS after exporter and ffmpeg -version; exporter dominates'
        except ImportError:
            report['encoder_peak_rss_bytes'] = None
            report['peak_rss_method'] = 'unavailable on this host; use native allocator/process telemetry'
        sequence = []
        for index in range(19):
            stem = work / str(index)
            raw = stem.with_suffix('.cfhd').read_bytes()
            parsed = parse_sample(raw)
            row = json.loads(stem.with_suffix('.json').read_text())
            row.update(artifact_index=index, pattern=('Flat', 'GradientChecker', 'Noise')[index] if index < 3 else ('Noise', 'HorizontalGradient', 'Checkerboard')[(index-3) % 3],
                       input_sha256=sha(stem.with_suffix('.bgra')), sample_sha256=hashlib.sha256(raw).hexdigest(), sample_bytes=len(raw),
                       payload_sha256=[hashlib.sha256(b['payload']).hexdigest() for b in parsed['bands']],
                       framed_sha256=[hashlib.sha256(b['framed']).hexdigest() for b in parsed['bands']])
            if len(row['quant']) != 36 or len(row['scale']) != 36 or len(row['coefficient_fnv64']) != 30:
                raise ValueError('missing coefficient/table evidence')
            ufrm = raw.index(b'UFRM')
            if raw[ufrm+4:ufrm+8] != b'\x04\x00\x00L' or struct.unpack_from('<I', raw, ufrm+8)[0] != row['frame_index']:
                raise ValueError('UFRM metadata/frame state mismatch')
            container = stem.with_suffix('.avi');container.write_bytes(avi([raw]))
            row['probe'] = probe(args.ffprobe, container, 1)
            decoded = stem.with_suffix('.decoded.yuv')
            subprocess.run([args.ffmpeg, '-v', 'error', '-xerror', '-y', '-i', str(container),
                '-frames:v', '1', '-pix_fmt', 'yuv422p10le', '-f', 'rawvideo', str(decoded)], check=True)
            if decoded.stat().st_size != 3840*2160*4:
                raise ValueError('decoded p10 extent')
            psnr = decoded_psnr(decoded.read_bytes(), stem.with_suffix('.reference.yuv').read_bytes())
            row['psnr_db'] = psnr if math.isfinite(psnr) else None
            row['lossless'] = math.isinf(psnr)
            row['decoded_sha256'] = sha(decoded)
            if index >= 3:
                sequence.append(raw)
            report['frames'].append(row)
        multi = work/'sequence.avi';multi.write_bytes(avi(sequence))
        report['sequence_probe'] = probe(args.ffprobe, multi, 16)
        sequence_yuv = work/'sequence.yuv'
        subprocess.run([args.ffmpeg, '-v', 'error', '-xerror', '-y', '-i', str(multi), '-pix_fmt', 'yuv422p10le', '-f', 'rawvideo', str(sequence_yuv)], check=True)
        if sequence_yuv.stat().st_size != 16*3840*2160*4:
            raise ValueError('sequence decoded p10 extent/frame count')
        report['sequence_decoded_sha256'] = verify_sequence(
            sequence_yuv, [row['decoded_sha256'] for row in report['frames'][3:]], 3840*2160*4)
        report['sequence_pixel_identity'] = 'PASS'
        if len({r['sample_sha256'] for r in report['frames'][3:]}) != 16:
            raise ValueError('sequence packets do not change')
    (args.output_dir/'geometry.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: 19 independent 4K FFmpeg p10 decodes, 16-frame sequence pixel identity, PSNR >35 dB')


if __name__ == '__main__':
    main()
