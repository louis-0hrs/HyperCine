#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Second-decoder triangulation on the pre-registered #120 1080p packet.

The packet and source identities are fixed before execution. The frozen GoPro
SDK and a pinned FFmpeg decode the exact same bytes; deltas are observations,
not a correctness oracle and not a gate change.
"""
import argparse
import json
import platform
import subprocess
import sys
from pathlib import Path

from compare import audit_case, digest, run

PACKET_SHA256 = '2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52'
SOURCE_SHA256 = '45c166c3d6a3710d620ff5d738980224edf53cf9ce1bceb87267e474294afff1'
PACKET_BYTES = 900396
SOURCE_BYTES = 8294400
GEOMETRY = (1920, 1080)
PROTOCOL_ID = 'registered-1080-bgra-second-decoder-20261018'
PROTOCOL_VERSION = 1


def sha_file(path):
    return digest(Path(path).read_bytes())


def verify_inputs(packet, source):
    if len(packet) != PACKET_BYTES or digest(packet) != PACKET_SHA256:
        raise ValueError('pre-registered packet identity or length mismatch')
    if len(source) != SOURCE_BYTES or digest(source) != SOURCE_SHA256:
        raise ValueError('pre-registered source identity or length mismatch')


def tool_identity(path, arguments):
    version = run([path] + list(arguments))
    return dict(version=version.splitlines()[0] if version else 'UNKNOWN',
                version_detail=version.strip(),
                binary_sha256=sha_file(path))


def cpu_model():
    script = ('(Get-CimInstance Win32_Processor | Select-Object -First 1 '
              'Name,NumberOfCores,NumberOfLogicalProcessors | ConvertTo-Json -Compress)')
    try:
        raw = subprocess.run(['powershell', '-NoProfile', '-Command', script],
                             capture_output=True, timeout=30)
        value = json.loads(raw.stdout.decode()) if raw.returncode == 0 else {}
        if isinstance(value, dict):
            return dict(model=value.get('Name', 'UNKNOWN'),
                        cores=value.get('NumberOfCores', 'UNKNOWN'),
                        logical_processors=value.get('NumberOfLogicalProcessors', 'UNKNOWN'))
    except (OSError, ValueError, subprocess.SubprocessError):
        pass
    return dict(model='UNKNOWN', cores='UNKNOWN', logical_processors='UNKNOWN')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--packet', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--sdk-dump', required=True)
    parser.add_argument('--ffmpeg', required=True)
    parser.add_argument('--ffprobe', required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        root = Path(__file__).resolve().parents[2]
        args.sdk_dump = str(Path(args.sdk_dump).resolve())
        args.ffmpeg = str(Path(args.ffmpeg).resolve())
        args.ffprobe = str(Path(args.ffprobe).resolve())
        args.work = Path(args.work).resolve()
        args.output = Path(args.output).resolve()
        packet = args.packet.read_bytes()
        source = args.source.read_bytes()
        verify_inputs(packet, source)
        if args.work.exists():
            raise ValueError('work directory must be fresh')
        args.work.mkdir(parents=True)

        source_commit = subprocess.run(
            ['git', 'rev-parse', 'HEAD'], cwd=root, capture_output=True,
            timeout=30, check=True).stdout.decode().strip()
        record = audit_case(packet, GEOMETRY[0], GEOMETRY[1],
                            'registered-1080-filmscan3', args)
        record.update(quality='filmscan3', pattern='REGISTERED_PACKET',
                      packet_bytes=len(packet), source_bytes=len(source),
                      source_sha256=SOURCE_SHA256)

        document = dict(
            schema_version=1,
            copyright='Copyright (c) 2026 HyperCine Contributors.',
            license='MIT OR Apache-2.0',
            protocol=dict(
                id=PROTOCOL_ID, version=PROTOCOL_VERSION,
                input_preregistered=True,
                packet_sha256=PACKET_SHA256, packet_bytes=PACKET_BYTES,
                source_sha256=SOURCE_SHA256, source_bytes=SOURCE_BYTES,
                geometry=list(GEOMETRY), requested_format='BGRA',
                decoded_resolution='CFHD_DECODED_RESOLUTION_FULL',
                processing_flags=65535,
                repeats_per_decoder=3,
                delta='GoPro minus FFmpeg',
                common_domain='tight top-down yuv422p10le, LE 16-bit containers',
                contract='FFmpeg is an independent triangulation only, not an oracle; '
                         'encoder-packet equality and release gates are unchanged'),
            sources=dict(
                source_commit=source_commit,
                script_sha256=sha_file(Path(__file__)),
                sdk_dump_source_sha256=sha_file(Path(__file__).with_name('sdk_dump.cpp')),
                avi_wrapper_sha256=sha_file(root / 'tests/validate_standalone.py')),
            tools=dict(
                gopro_sdk=dict(baseline='11574d0295771edccadd17af14af74a539f924c7',
                               output='YU64 Y0/Cr/Y1/Cb >>6, CPU_MAX=1, '
                                      'process_path=0xffff, colorspace 2/1/0',
                               binary_sha256=sha_file(args.sdk_dump)),
                ffmpeg=tool_identity(args.ffmpeg, ['-version']),
                ffprobe=tool_identity(args.ffprobe, ['-version'])),
            host=dict(system=platform.system(), machine=platform.machine(),
                      python=platform.python_version(), **cpu_model()),
            result=record,
            classification=dict(
                reference_stability=('STABLE_3_REPEATS'
                                     if record['stable_gopro'] and record['stable_ffmpeg']
                                     else 'UNSTABLE'),
                cross_decoder_agreement='MEASURED',
                encoder_packet_comparison='UNAVAILABLE',
                release_gate='UNRESOLVED', release_blocked=True))
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(document, indent=2, allow_nan=False) + '\n',
                               encoding='utf-8')
        print(json.dumps(dict(
            packet_sha256=PACKET_SHA256,
            stable_gopro=record['stable_gopro'],
            stable_ffmpeg=record['stable_ffmpeg'],
            ffprobe=record['ffprobe'],
            planes={plane: {key: record['planes'][plane][key]
                            for key in ('max_abs', 'mean_abs', 'p95_abs', 'rmse',
                                        'psnr_db', 'bias', 'mismatch_fraction')}
                    for plane in ('Y', 'Cb', 'Cr')})))
        return 0 if record['stable_gopro'] and record['stable_ffmpeg'] else 1
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError):
        print('INVALID_REGISTERED_TRIANGULATION: release blocked', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
