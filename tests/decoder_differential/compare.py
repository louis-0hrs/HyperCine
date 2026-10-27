#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Bounded, standard-library-only decoder audit. Delta = GoPro minus FFmpeg."""
import argparse
from array import array
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from validate_standalone import avi


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def unpack(raw, width, height):
    if width <= 0 or width % 2 or height <= 0 or width > 7680 or height > 4320:
        raise ValueError('Invalid bounded YUV422 geometry')
    if len(raw) != width * height * 4:
        raise ValueError('Wrong p10 byte length (tight top-down planes required)')
    values = array('H')
    values.frombytes(raw)
    if sys.byteorder != 'little':
        values.byteswap()
    if max(values) > 1023:
        raise ValueError('Nonzero upper six bits in p10 container')
    return values


def summarize(hist):
    count = sum(hist.values())
    if not count:
        return dict(samples=0, mismatches=0, max_abs=0, mean_abs=None)
    absolute = Counter()
    for delta, n in hist.items():
        absolute[abs(delta)] += n
    cumulative = 0
    for error in sorted(absolute):
        cumulative += absolute[error]
        if cumulative >= math.ceil(count * .95):
            p95 = error
            break
    squares = sum(d*d*n for d, n in hist.items())
    return dict(samples=count, mismatches=count-hist.get(0, 0),
                mismatch_fraction=(count-hist.get(0, 0))/count,
                max_abs=max(absolute), mean_abs=sum(d*n for d, n in absolute.items())/count,
                p95_abs=p95, bias=sum(d*n for d, n in hist.items())/count,
                rmse=math.sqrt(squares/count),
                psnr_db=10*math.log10(1023**2*count/squares) if squares else None,
                signed_histogram={str(d): hist[d] for d in sorted(set(hist) | set(range(-3, 4)))})


def compare(gopro, ffmpeg, width, height):
    a, b = unpack(gopro, width, height), unpack(ffmpeg, width, height)
    offset, planes = 0, {}
    for name, pw in [('Y', width), ('Cb', width//2), ('Cr', width//2)]:
        hist, groups, first = Counter(), {}, []
        for i in range(pw*height):
            va, vb = a[offset+i], b[offset+i]
            delta = int(va)-int(vb)
            x, y = i % pw, i // pw
            hist[delta] += 1
            # Disjoint bins within each grouping; border is eight luma pixels.
            border = x < (8 if name == 'Y' else 4) or x >= pw-(8 if name == 'Y' else 4) or y < 8 or y >= height-8
            keys = [('border', 'border' if border else 'interior'),
                    ('parity', f'{x%2},{y%2}'),
                    ('reference_code_bin', str(vb//64)),
                    ('spatial_8x8', f'{x*8//pw},{y*8//height}')]
            for category, key in keys:
                groups.setdefault(category, {}).setdefault(key, Counter())[delta] += 1
            if delta and len(first) < 32:
                first.append(dict(x=x, y=y, gopro=va, ffmpeg=vb, delta=delta))
        planes[name] = dict(**summarize(hist), first_32_differences=first,
                            groups={category: {key: summarize(h) for key, h in sorted(bins.items())}
                                    for category, bins in sorted(groups.items())})
        offset += pw*height
    return planes


def run(command):
    result = subprocess.run([str(x) for x in command], capture_output=True, timeout=20)
    if result.returncode:
        # Verbose tool diagnostics may contain local paths; keep them out of reports.
        raise RuntimeError(f'Decoder command failed: exit {result.returncode}')
    return result.stdout.decode('utf-8', errors='replace')


def verified_packet(root, manifest, frame, encoder='sdk'):
    relative = f"frames/{manifest['profile']['quality']}-sequence-{frame['width']}x{frame['height']}/frame_{frame['frame_index']:04d}/{encoder}.cfhd"
    entry = next(a for a in manifest['artifacts'] if a['path'] == relative)
    packet = (root/relative).read_bytes()
    if len(packet) != entry['byte_length'] or digest(packet) != entry['sha256'] or digest(packet) != frame[f'{encoder}_sha256']:
        raise ValueError('Archived packet does not match manifest')
    return packet


def audit_case(packet, width, height, label, args):
    work = Path(args.work)/label
    work.mkdir(parents=True, exist_ok=True)
    sample = work/'input.cfhd'
    sample.write_bytes(packet)
    wrapped = work/'input.avi'
    wrapped.write_bytes(avi(packet, width, height))
    probe = json.loads(run([args.ffprobe, '-v', 'error', '-select_streams', 'v:0',
                           '-show_entries', 'stream=codec_name,pix_fmt,width,height,color_range,color_space,chroma_location',
                           '-of', 'json', wrapped]))['streams'][0]
    if (probe.get('codec_name'), probe.get('width'), probe.get('height'), probe.get('pix_fmt')) != ('cfhd', width, height, 'yuv422p10le'):
        raise ValueError('FFprobe source format/geometry differs from audit contract')
    hashes, first = [], None
    for repeat in range(3):
        # Every process must produce its own fresh file. Exit zero without a
        # new output cannot reuse the previous call's pixels and look stable.
        sdk, ff = work/f'gopro-{repeat+1}.p10', work/f'ffmpeg-{repeat+1}.p10'
        if sdk.exists() or ff.exists():
            raise ValueError('Decoder repeat output already exists')
        run([args.sdk_dump, sample, sdk, width, height])
        run([args.ffmpeg, '-v', 'error', '-y', '-threads', '1', '-i', wrapped,
             '-frames:v', '1', '-pix_fmt', 'yuv422p10le', '-f', 'rawvideo', ff])
        a, b = sdk.read_bytes(), ff.read_bytes()
        unpack(a, width, height); unpack(b, width, height)
        hashes.append(dict(gopro=digest(a), ffmpeg=digest(b)))
        if repeat == 0:
            first = compare(a, b, width, height)
    return dict(label=label, width=width, height=height, input_sha256=digest(packet),
                ffprobe=probe,
                repeat_hashes=hashes,
                stable_gopro=len({h['gopro'] for h in hashes}) == 1,
                stable_ffmpeg=len({h['ffmpeg'] for h in hashes}) == 1,
                planes=first, cause='CAUSE_UNKNOWN')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--corpus', type=Path, required=True)
    p.add_argument('--sdk-dump', required=True)
    p.add_argument('--ffmpeg', required=True)
    p.add_argument('--ffprobe', required=True)
    p.add_argument('--work', required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    records = []
    for quality in ('low', 'filmscan2', 'filmscan3'):
        root = args.corpus/quality
        manifest = json.loads((root/'manifest.json').read_text())
        for index in (0, 3, 6):
            frame = next(f for f in manifest['frames'] if f['frame_index'] == index and f['width'] == 1920 and f['height'] == 1080)
            packet = verified_packet(root, manifest, frame)
            record = audit_case(packet, 1920, 1080, f'{quality}-{index}', args)
            record.update(quality=quality, frame_index=index, pattern=frame['pattern'],
                          corpus_manifest_sha256=digest((root/'manifest.json').read_bytes()))
            records.append(record)
            print(record['label'], [record['planes'][plane]['max_abs'] for plane in ('Y', 'Cb', 'Cr')], flush=True)
    root = args.corpus/'low'/'3840x2160'
    manifest = json.loads((root/'manifest.json').read_text())
    frame = manifest['frames'][0]
    records.append(audit_case(verified_packet(root, manifest, frame), 3840, 2160, 'low-4k-flat', args))
    # Independent historical 128px SDK/FFmpeg oracle, without reusing SDK output.
    fixtures = Path(__file__).resolve().parents[1]/'fixtures'
    for pattern in ('flat', 'gradient', 'canonical'):
        records.append(audit_case((fixtures/'standalone'/f'{pattern}.cfhd').read_bytes(),
                                 128, 128, 'historical-'+pattern, args))
    records.append(audit_case((fixtures/'windows-oracle'/'control.cfhd').read_bytes(),
                             128, 128, 'historical-sdk-control', args))
    report = dict(copyright='Copyright (c) 2026 HyperCine Contributors.', license='MIT OR Apache-2.0',
                  schema_version=1, ffmpeg_version=run([args.ffmpeg, '-version']),
                  ffprobe_version=run([args.ffprobe, '-version']).splitlines()[0],
                  audit_source_sha256={name: digest((Path(__file__).parent/name).read_bytes())
                                       for name in ('compare.py', 'sdk_dump.cpp')},
                  sdk_baseline='11574d0295771edccadd17af14af74a539f924c7',
                  contract=dict(format='yuv422p10le', orientation='top-down', planes='Y,Cb,Cr',
                                storage='tight LE unsigned 16-bit, low 10 bits', delta='GoPro minus FFmpeg',
                                sdk='YU64 Y0/Cr/Y1/Cb >>6; PRCS=65535, CLSY=2, CLSR=1, CLSF=0, CPU_MAX=1',
                                ffmpeg='cfhd, -threads 1, no explicit range or matrix conversion',
                                psnr_exact='null means zero MSE',
                                spatial='8x8 normalized plane grid; not codec tile attribution'),
                  pending=['IDWT first differing stage', 'highpass coefficient attribution',
                           'FFmpeg 8 version control', 'CPU_MAX alternative', 'explicit range/chroma controls',
                           'BGRA common-conversion audit', '8K and sequence matrix'],
                  cases=records)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False)+'\n', encoding='utf-8')
    if not all(r['stable_gopro'] and r['stable_ffmpeg'] for r in records):
        raise RuntimeError('Repeated decoder output unstable; see report')


if __name__ == '__main__':
    main()
