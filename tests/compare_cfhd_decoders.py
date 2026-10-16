#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Issue #11: replay archived SDK planes against fresh independent CFHD decodes.

This diagnostic measures differences; it never hides them behind a PSNR gate.
The SDK is an archived Windows oracle, not executed or emulated on this host.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import shutil
import subprocess
import tempfile

from reference_sample import parse_sample
from validate_standalone import avi


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def unpack_plane(raw, width, height, stride=None, endian='little', bottom_up=False):
    """Read unsigned p10 in p16 containers, excluding declared row padding."""
    stride = width * 2 if stride is None else stride
    if (width < 1 or height < 1 or stride < width * 2 or stride % 2
            or endian not in ('little', 'big') or len(raw) != stride * height):
        raise ValueError('invalid plane geometry, endian or storage')
    rows = range(height-1, -1, -1) if bottom_up else range(height)
    values = [v for y in rows for v in struct.unpack_from(
        ('<' if endian == 'little' else '>') + str(width) + 'H', raw, y * stride)]
    if any(v > 1023 for v in values):
        raise ValueError('sample exceeds unsigned p10 range')
    return values


def planes_p10(raw, width, height):
    if width < 2 or width % 2 or height < 1 or len(raw) != width * height * 4:
        raise ValueError('expected exactly one complete planar YUV422 p10 frame')
    result, at = {}, 0
    for name, w in [('Y', width), ('Cb', width//2), ('Cr', width//2)]:
        size = w * height * 2
        result[name] = (unpack_plane(raw[at:at+size], w, height), w)
        at += size
    return result


def metrics(actual, reference, width):
    if (width < 1 or not actual or len(actual) != len(reference)
            or len(actual) % width or any(not 0 <= v <= 1023 for v in actual)
            or any(not 0 <= v <= 1023 for v in reference)):
        raise ValueError('invalid comparison domain')
    count, height = len(actual), len(actual)//width
    total = squared = absolute = mismatches = maximum = border = 0
    first, rows, columns, histogram = [], {}, {}, {}
    for i, (a, r) in enumerate(zip(actual, reference)):
        d = a-r  # Signed convention is always FFmpeg minus SDK.
        total += d; squared += d*d; absolute += abs(d); maximum = max(maximum, abs(d))
        if d:
            mismatches += 1
            y, x = divmod(i, width)
            border += int(x in (0, width-1) or y in (0, height-1))
            rows[y] = rows.get(y, 0)+1; columns[x] = columns.get(x, 0)+1
            histogram[d] = histogram.get(d, 0)+1
            if len(first) < 32:
                first.append(dict(x=x, y=y, actual=a, reference=r, difference=d))
    def busiest(counts):
        return [dict(coordinate=k, mismatches=v) for k, v in
                sorted(counts.items(), key=lambda item: (-item[1], item[0]))[:8]]
    return dict(samples=count, exact_samples=count-mismatches,
                exact_fraction=(count-mismatches)/count, mismatches=mismatches,
                max_abs_error=maximum, mean_abs_error=absolute/count,
                rmse=math.sqrt(squared/count), bias=total/count,
                psnr_db=10*math.log10(1023**2*count/squared) if squared else None,
                psnr_infinite=squared == 0, first_32_differences=first,
                error_histogram={str(k): v for k, v in sorted(histogram.items())},
                border_mismatches=border, interior_mismatches=mismatches-border,
                affected_rows=len(rows), affected_columns=len(columns),
                busiest_rows=busiest(rows), busiest_columns=busiest(columns))


def compare(actual, reference, width, height):
    a, r = planes_p10(actual, width, height), planes_p10(reference, width, height)
    return {name: metrics(values, r[name][0], w) for name, (values, w) in a.items()}


def checked_files(root):
    manifest = json.loads((root/'manifest.json').read_text())
    data = {}
    for entry in manifest['files']:
        name = entry['file']
        if name in data or Path(name).name != name or name in ('.', '..'):
            raise ValueError('invalid or duplicate fixture name')
        raw = (root/name).read_bytes()
        if len(raw) != entry['bytes'] or sha256(raw) != entry['sha256']:
            raise ValueError('fixture integrity mismatch: ' + name)
        data[name] = raw
    return manifest, data


def command(argv):
    # Keep raw diagnostics local: paths/build flags can contain private data.
    proc = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode:
        raise ValueError('external decoder/probe failed (diagnostics withheld)')
    return proc.stdout


def audit(ffmpeg, ffprobe, repeats, fixtures):
    if not 2 <= repeats <= 20:
        raise ValueError('repeats must be in [2,20]')
    sdk_manifest, sdk = checked_files(fixtures/'windows-oracle')
    source_manifest, source = checked_files(fixtures/'standalone')
    report = dict(schema_version=1, license='MIT OR Apache-2.0',
                  generator_sha256=sha256(Path(__file__).read_bytes()),
                  ffmpeg_binary_sha256=sha256(Path(shutil.which(ffmpeg) or ffmpeg).read_bytes()),
                  ffprobe_binary_sha256=sha256(Path(shutil.which(ffprobe) or ffprobe).read_bytes()),
                  sdk_manifest_sha256=sha256((fixtures/'windows-oracle'/'manifest.json').read_bytes()),
                  standalone_manifest_sha256=sha256((fixtures/'standalone'/'manifest.json').read_bytes()),
                  scope='128x128 progressive GOP1; archived SDK replay',
                  sdk_run=sdk_manifest['run_url'], sdk_commit=sdk_manifest['commit'],
                  sdk_contract=sdk_manifest['oracle'],
                  sdk_new_execution=False,
                  standalone_encoder_commit=source_manifest['encoder_commit'],
                  ffmpeg_version=command([ffmpeg, '-version']).decode().splitlines()[0],
                  ffprobe_version=command([ffprobe, '-version']).decode().splitlines()[0],
                  pixel_contract='top-down Y,Cb,Cr; unsigned LE p10 in p16; tight planes; BT.709 studio',
                  sdk_conversion='archived YU64 LE Y0,Cr,Y1,Cb >>6; no row inversion',
                  sign='FFmpeg minus SDK', rgb_status='PENDING_MATCHED_CHROMA_UPSAMPLING',
                  pending=['1080p/4K/8K identical-packet SDK planes', '120-frame sequence',
                           'new SDK thread/dither experiments', 'inverse-wavelet cause localization'],
                  samples=[])
    with tempfile.TemporaryDirectory(prefix='hypercine-differential-') as temporary:
        root = Path(temporary)
        for name in ('flat', 'gradient', 'canonical', 'control'):
            packet = sdk['control.cfhd'] if name == 'control' else source[name+'.cfhd']
            expected = sdk[name+'.sdk-p10']
            parsed = parse_sample(packet)
            container = root/(name+'.avi'); container.write_bytes(avi(packet))
            # Prove the test muxer hands FFmpeg the identical compressed bytes.
            extracted_path = root/'extracted.cfhd'
            command([ffmpeg, '-v', 'error', '-y', '-i', str(container),
                     '-map', '0:v:0', '-c:v', 'copy', '-f', 'rawvideo', str(extracted_path)])
            extracted = extracted_path.read_bytes()
            if extracted != packet:
                raise ValueError('container packet identity mismatch')
            stream = json.loads(command([ffprobe, '-v', 'error', '-show_streams',
                                         '-of', 'json', str(container)]))['streams']
            if len(stream) != 1 or any(stream[0].get(k) != v for k, v in
                    dict(codec_name='cfhd', width=128, height=128, pix_fmt='yuv422p10le').items()):
                raise ValueError('decoded geometry/format mismatch')
            band_records = []
            for b in parsed['bands']:
                tags = dict(struct.iter_unpack('>hH', packet[b['frame_start']:b['chunk_offset']]))
                band_records.append(dict(channel=b['channel'], subband=b['subband'],
                                         width=b['width'], height=b['height'],
                                         quant=tags.get(34 if b['lowpass'] else 53),
                                         scale=tags.get(54), payload_sha256=sha256(b['payload'])))
            record = dict(pattern=name, producer='GoPro SDK' if name == 'control' else 'Mac standalone Q2',
                          packet_sha256=sha256(packet), packet_bytes=len(packet),
                          extracted_packet_sha256=sha256(extracted), sdk_p10_sha256=sha256(expected),
                          bands=band_records,
                          modes=[])
            baseline = None
            for mode, flags in [('bitexact_single', ['-threads', '1', '-flags', '+bitexact']),
                                ('default_auto', ['-threads', '0']),
                                ('bitexact_two', ['-threads', '2', '-flags', '+bitexact'])]:
                hashes = []
                decoded = None
                for _ in range(repeats):
                    decoded_path = root/'decoded.p10'
                    command([ffmpeg, '-v', 'error', '-xerror', '-y', *flags, '-i', str(container),
                             '-map', '0:v:0', '-an', '-c:v', 'rawvideo', '-pix_fmt',
                             'yuv422p10le', '-f', 'rawvideo', str(decoded_path)])
                    decoded = decoded_path.read_bytes()
                    planes_p10(decoded, 128, 128)  # also rejects extra/missing frames
                    hashes.append(sha256(decoded))
                if baseline is None:
                    baseline = hashes[0]
                record['modes'].append(dict(mode=mode, input_options=flags, requested_runs=repeats,
                                           hashes=hashes, distinct_outputs=len(set(hashes)),
                                           matches_single=all(h == baseline for h in hashes),
                                           comparison='same as bitexact_single' if mode != 'bitexact_single' and all(
                                               h == baseline for h in hashes) else compare(decoded, expected, 128, 128)))
            report['samples'].append(record)
    report['stable'] = all(m['distinct_outputs'] == 1 and m['matches_single']
                           for s in report['samples'] for m in s['modes'])
    report['status'] = 'MEASURED_DIFFERENCES' if report['stable'] else 'UNSTABLE_OUTPUT'
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ffmpeg', required=True)
    parser.add_argument('--ffprobe', required=True)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--fixtures', type=Path, default=Path(__file__).parent/'fixtures')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = audit(args.ffmpeg, args.ffprobe, args.repeats, args.fixtures)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, allow_nan=False)+'\n')
    print('Decoder differential:', result['status'], '; 4 packets, 3 modes,', args.repeats, 'runs per mode')
    return 0 if result['stable'] else 1


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, OSError, KeyError, TypeError, struct.error):
        raise SystemExit('Decoder differential failed; verify fixtures, tools and arguments')
