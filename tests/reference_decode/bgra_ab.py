#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Same immutable canonical BGRA packet, active processing metadata OFF versus ON."""
import argparse
import json
from pathlib import Path
import platform
import subprocess
import sys

import contract
from measure import metrics, run, sha


def rgb(raw):
    if not raw or len(raw) % 4:
        raise ValueError('invalid BGRA buffer')
    # Preserve channel order; the comparison excludes alpha like Phase2C.
    return raw[0::4]+raw[1::4]+raw[2::4]


def quality(source, pixels, width, height):
    if len(source) != width*height*4 or len(pixels) != len(source):
        raise ValueError('invalid BGRA geometry')
    top = metrics(rgb(source), rgb(pixels))
    pitch = width*4
    flipped = b''.join(pixels[y*pitch:(y+1)*pitch] for y in range(height-1, -1, -1))
    bottom = metrics(rgb(source), rgb(flipped))
    selected = max((top, bottom), key=lambda m: float('inf') if m['source_pixels_exact'] else m['psnr_db'])
    return dict(psnr_db=selected['psnr_db'], source_pixels_exact=selected['source_pixels_exact'],
                source_psnr_top_down=top['psnr_db'], source_psnr_bottom_up=bottom['psnr_db'],
                best_orientation='top_down' if selected is top else 'bottom_up')


def validate(row, active, width, height):
    """Reject metadata failures and configuration drift even with decoder status 0."""
    if row['active_metadata'] is not active or row['requested_format'] != 'BGRA':
        raise ValueError('worker configuration drift')
    expected = 0 if active else -1
    metadata_ok = all(type(row[name]) is int and row[name] == expected for name in
        ('metadata_open_status', 'metadata_init_status', 'metadata_set_status'))
    if type(row['processing_flags']) is not int or row['processing_flags'] != (65535 if active else 0):
        raise ValueError('processing flags drift')
    row['output_valid'] = row['output_valid'] is True and metadata_ok and (
        row['width'] == width and row['height'] == height and row['pitch'] == width*4 and
        row['buffer_size'] == width*height*4)
    return row


def collect(worker, packet, source, case, active, processes, root):
    width, height = (128, 128) if case == '128' else (1920, 1080)
    immutable = packet.read_bytes()
    mode = 'decode-bgra-on' if active else 'decode-bgra-off'
    calls = []
    first = None
    for process in range(1, processes+1):
        prefix = root/(mode+'-'+str(process))
        rows = [contract.strict_document(line.decode()) for line in
                run([worker, mode, case, packet, prefix]).splitlines()]
        if len(rows) != 5 or [r['ordinal'] for r in rows] != list(range(1, 6)):
            raise ValueError('incomplete worker')
        for row in rows:
            row = validate(row, active, width, height)
            row.update(process=process, sample_sha256=sha(packet.read_bytes()))
            if row['output_valid']:
                pixels = Path(str(prefix)+'-'+str(row['ordinal'])+'.bgra').read_bytes()
                if len(pixels) != width*height*4:
                    raise ValueError('invalid output buffer')
                row.update(pixel_sha256=sha(pixels), rgb_sha256=sha(rgb(pixels)),
                           **quality(source, pixels, width, height))
                if first is None:
                    first = pixels
                row['versus_first_decode'] = metrics(first, pixels)
                difference = next((i for i, (a,b) in enumerate(zip(first,pixels)) if a != b), None)
                row['first_difference'] = None if difference is None else dict(
                    byte_offset=difference, x=difference//4 % width, y=difference//(width*4),
                    channel='BGRA'[difference%4], reference=first[difference], actual=pixels[difference])
            calls.append(row)
    document = dict(schema_version=1, sample_sha256=sha(immutable), encoder_packets_equal=None,
        calls=calls, source_sha256=sha(source), source_format='BGRA8 canonical 0x1337BEEF',
        requested_format='BGRA', geometry=[width,height], pitch=width*4, active_metadata=active,
        processing_flags=65535 if active else None, range='UNSPECIFIED', matrix='UNSPECIFIED',
        worker_threading='original SDK default _THREADED_DECODER=1; no CPU_MAX override',
        runtime_worker_count=None, quality_metric='Phase2C best of both row orientations, BGR only, peak 255',
        alternate_decoder={'status':'UNAVAILABLE','reason':'not_measured_in_BGRA_AB'},
        first_observable_divergence='SDK decoded BGRA output' if len({c.get('pixel_sha256') for c in calls})>1 else 'NOT_OBSERVED',
        first_internal_inverse_stage='UNOBSERVED: public API exposes final output only')
    document['result'] = contract.report(document)
    document['summary'] = dict(calls=len(calls), processes=processes,
        distinct_pixel_hashes=len({c.get('pixel_sha256') for c in calls}),
        distinct_rgb_hashes=len({c.get('rgb_sha256') for c in calls}),
        internally_divergent_processes=sum(len({c.get('pixel_sha256') for c in calls if c['process']==p})>1
            for p in range(1,processes+1)),
        decoder_statuses=sorted({c['decoder_status'] for c in calls}))
    return document


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--worker', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--processes', type=int, default=4, choices=range(2, 7))
    args = parser.parse_args()
    try:
        root = Path(__file__).resolve().parents[2]
        sources = [Path(__file__), Path(__file__).with_name('worker.cpp'), Path(contract.__file__),
            Path(__file__).with_name('measure.py'), root/'include/hypercine/canonical.h',
            root/'upstream/Codec/config.h', root/'upstream/DecoderSDK/SampleDecoder.cpp',
            root/'upstream/Codec/decoder.c', root/'upstream/Codec/InvertHorizontalStrip16s.c']
        report = dict(copyright='Copyright (c) 2026 HyperCine Contributors.', license='MIT OR Apache-2.0',
            schema_version=1, platform=dict(system=platform.system(), machine=platform.machine(),
            python=platform.python_version()), source_commit=run(['git','rev-parse','HEAD']).decode().strip(),
            source_sha256={p.relative_to(root).as_posix():sha(p.read_bytes()) for p in sources},
            worker_sha256=sha(args.worker.read_bytes()), release_blocked=True, cases=[])
        for case in ('128','1080'):
            work = args.work/case
            work.mkdir(parents=True, exist_ok=False)
            packet, source = work/'sample.cfhd', work/'source.bgra'
            run([args.worker.resolve(),'generate-bgra',case,packet,source])
            original = source.read_bytes()
            for active in (False,True):
                report['cases'].append(collect(args.worker.resolve(),packet,original,case,active,args.processes,work))
        args.output.write_text(json.dumps(report,indent=2,allow_nan=False)+'\n',encoding='utf-8')
        print(json.dumps([dict(geometry=c['geometry'],active_metadata=c['active_metadata'],
            **c['summary'],classification=c['result']['classification']) for c in report['cases']]))
        return 1 # No encoder-pair evidence or release acceptance is collected.
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError):
        print('INVALID_BGRA_MEASUREMENT: release blocked',file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
