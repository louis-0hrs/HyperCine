#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Recover binary evidence from Windows build log; independently decode GoPro control.

Windows never needs FFmpeg. Its real SDK control packet and unmasked pixel planes
are carried in log records and decoded/compared on Mac. No SDK is emulated.
"""
import argparse
import base64
import hashlib
import json
import math
import os
from pathlib import Path
from validation_support import require, p10_samples
import subprocess
from validate_standalone import avi, reference


# Log lines that carry a Windows-run observation. The summary line keeps
# quality_measured= and distinctOutputsMax= after the PASS marker was retired,
# so replay must select it explicitly instead of silently recording less.
OBSERVATION_MARKERS = ('distinctYU64=', 'quality_measured=', 'status=',
                       'STRICT_FAILURE', 'Oracle harness error')
ORACLE_NAMES = {name + ext for name in ('flat', 'gradient', 'canonical')
                for ext in ('.sdk-p10', '.sdk-bgra')} | {'control.cfhd', 'control.sdk-p10'}


def is_observation_line(line):
    return any(marker in line for marker in OBSERVATION_MARKERS)


def metric(a, b, peak=1023):
    require(len(a) == len(b) and len(a) > 0, 'pixel plane lengths differ or are empty')
    errors = [x-y for x, y in zip(a, b)]
    total = sum(e*e for e in errors)
    first = next((i for i, e in enumerate(errors) if e), None)
    return dict(psnr_db=10*math.log10(peak*peak*len(a)/total) if total else None,
                max_error=max(map(abs, errors)), mismatches=sum(e != 0 for e in errors),
                first_index=first, first_actual=a[first] if first is not None else None,
                first_reference=b[first] if first is not None else None)


def p10_metrics(actual, expected):
    a, b = p10_samples(actual, 128, 128), p10_samples(expected, 128, 128)
    bounds = (0, 16384, 24576, 32768)
    return dict(overall=metric(a, b), planes={name: metric(a[bounds[i]:bounds[i+1]], b[bounds[i]:bounds[i+1]])
                for i, name in enumerate(('Y', 'Cb', 'Cr'))})


def extract_log(lines):
    """Accept exactly the bounded public oracle inventory before any extracted file is written."""
    data, active, encoded, declared, encoded_size = {}, None, [], 0, 0
    for line in lines:
        if 'ORACLE_DATA_BEGIN ' in line:
            require(active is None, 'nested oracle record')
            fields = line.split('ORACLE_DATA_BEGIN ', 1)[1].split()
            require(len(fields) == 2, 'invalid oracle header')
            name, size = fields
            require(name in ORACLE_NAMES and name not in data, 'invalid or duplicate oracle name')
            require(size.isascii() and size.isdecimal(), 'invalid oracle length')
            declared = int(size)
            require(0 < declared <= 16 * 1024 * 1024, 'oracle record too large')
            require(name == 'control.cfhd' or declared == 65536, 'invalid pixel storage')
            active, encoded, encoded_size = name, [], 0
        elif 'ORACLE_DATA_END ' in line:
            require(active is not None and
                    line.split('ORACLE_DATA_END ', 1)[1].strip() == active,
                    'unmatched oracle end')
            raw = base64.b64decode(''.join(encoded), validate=True)
            require(len(raw) == declared, 'oracle record length mismatch')
            data[active] = raw
            active = None
        elif 'ORACLE_DATA ' in line:
            require(active is not None, 'oracle payload outside record')
            part = line.split('ORACLE_DATA ', 1)[1].strip()
            encoded_size += len(part)
            require(encoded_size <= 4 * ((declared + 2) // 3),
                    'oracle payload exceeds declared length')
            encoded.append(part)
    require(active is None, 'unterminated oracle record')
    require(set(data) == ORACLE_NAMES, 'incomplete oracle inventory')
    return data



def write_exclusive(path, data):
    """Create an evidence file once: never follow a final symlink or truncate a prior receipt."""
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, 'O_NOFOLLOW'):
        flags |= os.O_NOFOLLOW
    with os.fdopen(os.open(path, flags, 0o600), 'wb') as target:
        target.write(data)


def main():
    parser = argparse.ArgumentParser()
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--log')
    source.add_argument('--windows-fixtures')
    parser.add_argument('--ffmpeg', required=True)
    parser.add_argument('--ffprobe', required=True)
    parser.add_argument('--run-url')
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    root = Path(args.output)
    require(not root.is_symlink(), 'oracle output directory is a symlink')
    root.mkdir(parents=True, exist_ok=True)
    require(root.is_dir() and not root.is_symlink(), 'invalid oracle output directory')
    fixture = Path(__file__).parent/'fixtures'/'standalone'
    data = {}
    observations = []
    if args.windows_fixtures:
        manifest = json.loads((Path(args.windows_fixtures)/'manifest.json').read_text())
        require(type(manifest) is dict and type(manifest.get('files')) is list,
                'invalid fixture manifest')
        fixture_root = Path(args.windows_fixtures).resolve()
        for file in manifest['files']:
            require(type(file) is dict and file.get('file') in ORACLE_NAMES and
                    file['file'] not in data, 'invalid or duplicate fixture name')
            path = fixture_root / file['file']
            require(path.resolve().parent == fixture_root, 'fixture escapes directory')
            raw = path.read_bytes()
            require(type(file.get('bytes')) is int and len(raw) == file['bytes'] and
                    hashlib.sha256(raw).hexdigest() == file.get('sha256'),
                    'fixture length or digest mismatch')
            data[file['file']] = raw
        run_url, log_sha = manifest['run_url'], manifest['log_sha256']
    else:
        require(args.run_url, '--run-url required with --log' )
        run_url = args.run_url
        log_sha = hashlib.sha256(Path(args.log).read_bytes()).hexdigest()
    if args.log:
        lines = Path(args.log).read_text().splitlines()
        for line in lines:
            if is_observation_line(line):
                observations.append(dict(
                    step=line.split('\t')[1] if '\t' in line else 'unlabelled',
                    result=line.split('Z ', 1)[-1].strip()))
        data = extract_log(lines)
    require(set(data) == ORACLE_NAMES, 'incomplete oracle inventory')
    # Check ALL outputs (not just extracted records) before writing even the first byte.
    destinations = {'control.avi', 'control.ffmpeg-p10', 'comparison.json'}
    if args.log:
        destinations.update(ORACLE_NAMES)
    for name in sorted(destinations):
        target = root / name
        require(target.resolve().parent == root.resolve() and
                not target.exists() and not target.is_symlink(),
                'unsafe or occupied oracle destination')
    if args.log:
        for name in sorted(ORACLE_NAMES):
            write_exclusive(root / name, data[name])
    results = dict(copyright='Copyright (c) 2026 HyperCine Contributors.', license='MIT OR Apache-2.0',
        windows_run=run_url, log_sha256=log_sha, windows_observations=observations,
        ffmpeg_version=subprocess.check_output([args.ffmpeg, '-version'], text=True).splitlines()[0],
        input_contract='128x128 top-down BT.709 studio YUV422 p10; YU64 LE Y0 Cr Y1 Cb >>6; BGRA SDK bottom-up',
        alpha_excluded=True, samples=[], control={})
    for pattern in ('flat', 'gradient', 'canonical'):
        raw = data[pattern+'.sdk-p10']
        rgb = data[pattern+'.sdk-bgra']
        # Declared inversion only, never pick the better orientation or mask pixels.
        topdown = b''.join(rgb[y*512:(y+1)*512] for y in reversed(range(128)))
        reference_rgb = (fixture/(pattern+'.bgra')).read_bytes()
        rows, _ = reference(pattern)
        source_rgb = [v for row in rows for pixel in row for v in pixel]
        record = dict(pattern=pattern, sdk_p10_sha256=hashlib.sha256(raw).hexdigest(),
            sdk_bgra_sha256=hashlib.sha256(rgb).hexdigest(),
            sdk_vs_ffmpeg_p10=p10_metrics(raw, (fixture/(pattern+'.p10')).read_bytes()),
            sdk_vs_uncompressed_p10=p10_metrics(raw, (fixture/(pattern+'.input-p10')).read_bytes()),
            sdk_vs_ffmpeg_rgb=metric([x for i, x in enumerate(topdown) if i%4 != 3],
                                    [x for i, x in enumerate(reference_rgb) if i%4 != 3], 255),
            sdk_vs_source_rgb=metric([x for i, x in enumerate(topdown) if i%4 != 3], source_rgb, 255))
        results['samples'].append(record)
    packet=data['control.cfhd']; container=root/'control.avi';write_exclusive(container, avi(packet))
    stream=json.loads(subprocess.check_output([args.ffprobe,'-v','error','-show_streams','-of','json',str(container)]))['streams'][0]
    require((stream['codec_name'],stream['width'],stream['height'],stream['pix_fmt']) ==
            ('cfhd',128,128,'yuv422p10le'), 'unexpected oracle stream')
    decoded=root/'control.ffmpeg-p10'
    subprocess.run([args.ffmpeg,'-v','error','-xerror','-n','-i',str(container),'-frames:v','1',
        '-c:v','rawvideo','-pix_fmt','yuv422p10le','-f','rawvideo',str(decoded)],check=True)
    raw=decoded.read_bytes();source=(fixture/'canonical.input-p10').read_bytes()
    results['control']=dict(packet_bytes=len(packet),packet_sha256=hashlib.sha256(packet).hexdigest(),
        sdk_p10_sha256=hashlib.sha256(data['control.sdk-p10']).hexdigest(),
        ffmpeg_p10_sha256=hashlib.sha256(raw).hexdigest(),
        sdk_vs_ffmpeg_p10=p10_metrics(data['control.sdk-p10'],raw),
        sdk_vs_source_p10=p10_metrics(data['control.sdk-p10'],source),
        ffmpeg_vs_source_p10=p10_metrics(raw,source))
    # Write genuine evidence before reporting any quality failure.
    write_exclusive(root/'comparison.json', (json.dumps(results,indent=2)+'\n').encode('utf-8'))
    for sample in results['samples']:
        overall=sample['sdk_vs_ffmpeg_p10']['overall']
        require(overall['psnr_db'] is None or overall['psnr_db'] > 35,
                'SDK sample PSNR gate failed')
        if sample['pattern'] == 'flat':
            require(overall['max_error'] == 0, 'flat sample mismatch')
    for name in ('sdk_vs_ffmpeg_p10','sdk_vs_source_p10','ffmpeg_vs_source_p10'):
        overall=results['control'][name]['overall']
        require(overall['psnr_db'] is None or overall['psnr_db'] > 35,
                'control PSNR gate failed')
    print(json.dumps(results,indent=2))


if __name__ == '__main__':
    main()
