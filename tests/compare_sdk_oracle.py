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
from pathlib import Path
import struct
import subprocess
from validate_standalone import avi, reference


def metric(a, b, peak=1023):
    assert len(a) == len(b) and len(a) > 0
    errors = [x-y for x, y in zip(a, b)]
    total = sum(e*e for e in errors)
    first = next((i for i, e in enumerate(errors) if e), None)
    return dict(psnr_db=10*math.log10(peak*peak*len(a)/total) if total else None,
                max_error=max(map(abs, errors)), mismatches=sum(e != 0 for e in errors),
                first_index=first, first_actual=a[first] if first is not None else None,
                first_reference=b[first] if first is not None else None)


def p10_metrics(actual, expected):
    a, b = struct.unpack('<32768H', actual), struct.unpack('<32768H', expected)
    bounds = (0, 16384, 24576, 32768)
    return dict(overall=metric(a, b), planes={name: metric(a[bounds[i]:bounds[i+1]], b[bounds[i]:bounds[i+1]])
                for i, name in enumerate(('Y', 'Cb', 'Cr'))})


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
    root = Path(args.output); root.mkdir(parents=True, exist_ok=True)
    fixture = Path(__file__).parent/'fixtures'/'standalone'
    data, active, encoded, length = {}, None, [], 0
    observations = []
    if args.windows_fixtures:
        manifest = json.loads((Path(args.windows_fixtures)/'manifest.json').read_text())
        for file in manifest['files']:
            raw = (Path(args.windows_fixtures)/file['file']).read_bytes()
            assert len(raw) == file['bytes'] and hashlib.sha256(raw).hexdigest() == file['sha256']
            data[file['file']] = raw
        run_url, log_sha = manifest['run_url'], manifest['log_sha256']
    else:
        assert args.run_url, '--run-url required with --log'
        run_url = args.run_url
        log_sha = hashlib.sha256(Path(args.log).read_bytes()).hexdigest()
    for line in Path(args.log).read_text().splitlines() if args.log else []:
        if 'distinctYU64=' in line or 'status=' in line or 'SDK decode PASS:' in line or 'SDK decode FAIL:' in line:
            observations.append(dict(step=line.split('\t')[1], result=line.split('Z ', 1)[-1].strip()))
        if 'ORACLE_DATA_BEGIN ' in line:
            assert active is None
            active, size = line.split('ORACLE_DATA_BEGIN ', 1)[1].split()
            length = int(size); encoded = []
        elif 'ORACLE_DATA_END ' in line:
            assert line.split('ORACLE_DATA_END ', 1)[1].strip() == active
            raw = base64.b64decode(''.join(encoded), validate=True)
            assert len(raw) == length and active not in data
            data[active] = raw; (root/active).write_bytes(raw); active = None
        elif 'ORACLE_DATA ' in line:
            assert active is not None
            encoded.append(line.split('ORACLE_DATA ', 1)[1].strip())
    assert active is None
    assert set(data) == {n+ext for n in ('flat', 'gradient', 'canonical') for ext in ('.sdk-p10', '.sdk-bgra')} | {'control.cfhd', 'control.sdk-p10'}, set(data)
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
    packet=data['control.cfhd']; container=root/'control.avi';container.write_bytes(avi(packet))
    stream=json.loads(subprocess.check_output([args.ffprobe,'-v','error','-show_streams','-of','json',str(container)]))['streams'][0]
    assert (stream['codec_name'],stream['width'],stream['height'],stream['pix_fmt']) == ('cfhd',128,128,'yuv422p10le'), stream
    decoded=root/'control.ffmpeg-p10'
    subprocess.run([args.ffmpeg,'-v','error','-xerror','-y','-i',str(container),'-frames:v','1',
        '-c:v','rawvideo','-pix_fmt','yuv422p10le','-f','rawvideo',str(decoded)],check=True)
    raw=decoded.read_bytes();source=(fixture/'canonical.input-p10').read_bytes()
    results['control']=dict(packet_bytes=len(packet),packet_sha256=hashlib.sha256(packet).hexdigest(),
        sdk_p10_sha256=hashlib.sha256(data['control.sdk-p10']).hexdigest(),
        ffmpeg_p10_sha256=hashlib.sha256(raw).hexdigest(),
        sdk_vs_ffmpeg_p10=p10_metrics(data['control.sdk-p10'],raw),
        sdk_vs_source_p10=p10_metrics(data['control.sdk-p10'],source),
        ffmpeg_vs_source_p10=p10_metrics(raw,source))
    # Write genuine evidence before reporting any quality failure.
    (root/'comparison.json').write_text(json.dumps(results,indent=2)+'\n')
    for sample in results['samples']:
        overall=sample['sdk_vs_ffmpeg_p10']['overall']
        assert overall['psnr_db'] is None or overall['psnr_db']>35, sample
        if sample['pattern']=='flat': assert overall['max_error']==0, sample
    for name in ('sdk_vs_ffmpeg_p10','sdk_vs_source_p10','ffmpeg_vs_source_p10'):
        overall=results['control'][name]['overall']
        assert overall['psnr_db'] is None or overall['psnr_db']>35, (name,overall)
    print(json.dumps(results,indent=2))


if __name__ == '__main__':
    main()
