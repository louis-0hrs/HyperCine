#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Bounded CPU-only immutable packet measurement; raw buffers stay local."""
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys

import contract
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from validate_standalone import avi


def sha(data):
    return hashlib.sha256(data).hexdigest()


def metrics(a, b):
    if not a or len(a) != len(b):
        raise ValueError('invalid comparison buffers')
    hist = Counter(x-y for x, y in zip(a, b))
    squared = sum(d*d*n for d, n in hist.items())
    return dict(changed_bytes=len(a)-hist.get(0, 0), max_abs_delta=max(map(abs, hist)),
                psnr_db=10*math.log10(255**2*len(a)/squared) if squared else None,
                source_pixels_exact=squared == 0)


def run(command):
    result = subprocess.run(list(map(str, command)), capture_output=True, timeout=90)
    if result.returncode:
        raise ValueError('diagnostic subprocess failed')
    return result.stdout


def control(worker, packet, work):
    """Existing 128px flat fixture: hash stability only, no invented source PSNR."""
    immutable = packet.read_bytes()
    calls = []
    root = work/'flat-control'
    root.mkdir(parents=True, exist_ok=False)
    for process in (1, 2):
        prefix = root/str(process)
        rows = [json.loads(line) for line in run([worker, 'decode', '128', packet, prefix]).splitlines()]
        if len(rows) != 5 or [r['ordinal'] for r in rows] != list(range(1, 6)):
            raise ValueError('incomplete control')
        for row in rows:
            row.update(process=process, sample_sha256=sha(packet.read_bytes()))
            if row['output_valid']:
                pixels = Path(str(prefix)+'-'+str(row['ordinal'])+'.yuy2').read_bytes()
                if len(pixels) != 128*128*2:
                    raise ValueError('invalid control buffer')
                row['pixel_sha256'] = sha(pixels)
            calls.append(row)
    return dict(fixture='tests/fixtures/standalone/flat.cfhd', sample_sha256=sha(immutable),
        calls=calls, distinct_pixel_hashes=len({c.get('pixel_sha256') for c in calls}),
        quality_gate='UNMEASURED', release_blocked=True)


def measure(worker, case, processes, work, ffmpeg=None, ffprobe=None):
    width, height = (128, 128) if case == '128' else (1920, 1080)
    root = work / case
    root.mkdir(parents=True, exist_ok=False)
    packet, source = root/'sample.cfhd', root/'source.yuy2'
    run([worker, 'generate', case, packet, source])
    immutable = packet.read_bytes()
    original = source.read_bytes()
    sample_sha = sha(immutable)
    calls = []
    first = None
    for process in range(1, processes+1):
        prefix = root / ('process-'+str(process))
        rows = [json.loads(line) for line in run([worker, 'decode', case, packet, prefix]).splitlines()]
        if len(rows) != 5 or [r['ordinal'] for r in rows] != list(range(1, 6)):
            raise ValueError('incomplete worker observations')
        for row in rows:
            row.update(process=process, sample_sha256=sha(packet.read_bytes()))
            if row['output_valid']:
                pixels = Path(str(prefix)+'-'+str(row['ordinal'])+'.yuy2').read_bytes()
                if len(pixels) != width*height*2 or row['buffer_size'] != len(pixels) or row['pitch'] != width*2:
                    raise ValueError('invalid worker geometry')
                row.update(pixel_sha256=sha(pixels), **metrics(original, pixels))
                if first is None:
                    first = pixels
                row['versus_first_decode'] = metrics(first, pixels)
            calls.append(row)
    document = dict(schema_version=1, sample_sha256=sample_sha,
                    # No CPU/GPU/reference packet pair was measured by this worker.
                    encoder_packets_equal=None, calls=calls, source_sha256=sha(original),
                    geometry=[width, height], seed='0x1337BEEF', quality='FilmScan3',
                    format='YUY2', bit_depth=8, pitch=width*2,
                    range='UNSPECIFIED', matrix='UNSPECIFIED',
                    alternate_decoder=dict(status='UNAVAILABLE', reason='not_configured'))
    if ffmpeg and ffprobe:
        # Reuse #11's single-packet AVI wrapper; no second differential audit.
        wrapped = root/'sample.avi'
        wrapped.write_bytes(avi(immutable, width, height))
        version_lines = run([ffmpeg, '-version']).decode().splitlines()
        version = version_lines[0]
        probe = json.loads(run([ffprobe, '-v', 'error', '-show_entries',
            'stream=codec_name,width,height,pix_fmt,bits_per_raw_sample,color_range,color_space',
            '-of', 'json', wrapped]))['streams'][0]
        if probe.get('codec_name') != 'cfhd' or probe.get('width') != width or probe.get('height') != height:
            raise ValueError('alternate decoder geometry')
        outputs = []
        for repeat in range(1, 4):
            output = root/('ffmpeg-'+str(repeat)+'.yuy2')
            run([ffmpeg, '-v', 'error', '-threads', '1', '-i', wrapped, '-frames:v', '1',
                 '-flags', '+bitexact', '-pix_fmt', 'yuyv422', '-f', 'rawvideo', output])
            pixels = output.read_bytes()
            outputs.append(dict(process=repeat, pixel_sha256=sha(pixels),
                versus_source=metrics(original, pixels),
                versus_gopro_first=metrics(first, pixels) if first is not None else None))
        document['alternate_decoder'] = dict(status='MEASURED', version=version,
            executable_sha256=sha(ffmpeg.read_bytes()), probe=probe, format='yuyv422',
            library_versions=[line.strip() for line in version_lines if line.startswith('lib')],
            ffprobe_version=run([ffprobe, '-version']).decode().splitlines()[0],
            ffprobe_sha256=sha(ffprobe.read_bytes()),
            bit_depth=8, pitch=width*2, range=probe.get('color_range', 'UNSPECIFIED'),
            matrix=probe.get('color_space', 'UNSPECIFIED'),
            conversion='FFmpeg native p10 to packed p8; no forced range/matrix; not a byte oracle',
            calls=outputs, acceptance='UNREVIEWED')
    document['result'] = contract.report(document)
    document['repeat_summary'] = dict(
        processes=processes, calls=len(calls),
        within_process_divergent=sum(len({c.get('pixel_sha256') for c in calls if c['process']==p})>1
                                     for p in range(1, processes+1)),
        distinct_pixel_hashes=len({c.get('pixel_sha256') for c in calls}),
        first_call_distinct_hashes=len({c.get('pixel_sha256') for c in calls if c['ordinal']==1}))
    return document


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--worker', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--processes', type=int, default=6, choices=range(2, 31))
    parser.add_argument('--ffmpeg', type=Path)
    parser.add_argument('--ffprobe', type=Path)
    args = parser.parse_args()
    try:
        if bool(args.ffmpeg) != bool(args.ffprobe):
            raise ValueError('alternate decoder configuration incomplete')
        report = dict(schema_version=1, platform='Windows CPU-only',
            source_sha256={p.name: sha(p.read_bytes()) for p in
                (Path(__file__), Path(__file__).with_name('worker.cpp'), Path(contract.__file__),
                 Path(__file__).parents[1]/'validate_standalone.py')},
            worker_sha256=sha(args.worker.read_bytes()),
            source_commit=run(['git', 'rev-parse', 'HEAD']).decode().strip(),
            release_blocked=True, cases=[])
        report['flat_control'] = control(args.worker.resolve(),
            Path(__file__).resolve().parents[1]/'fixtures/standalone/flat.cfhd', args.work)
        for case in ('128', '1080'):
            report['cases'].append(measure(args.worker.resolve(), case, args.processes,
                                           args.work, args.ffmpeg, args.ffprobe))
        args.output.write_text(json.dumps(report, indent=2, allow_nan=False)+'\n', encoding='utf-8')
        print(json.dumps([c['repeat_summary'] | c['result'] for c in report['cases']]))
        # Evidence collection success does not satisfy the unresolved release gate.
        return 0 if all(c['result']['classification']=='PASS' for c in report['cases']) else 1
    except (OSError, ValueError, KeyError, subprocess.SubprocessError):
        print('INVALID_MEASUREMENT: release blocked', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
