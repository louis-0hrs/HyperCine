#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Independent FFmpeg oracle, not a HyperCine decoder or SDK parity test."""
import argparse
import hashlib
import json
import pathlib
import struct
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
FIXTURE = 'upstream/Example/videos/CFHDsample422.AVI'
WIDTH, HEIGHT, COUNT = 1920, 1080, 24
FRAME_BYTES = WIDTH * HEIGHT * 4


def sha(data):
    return hashlib.sha256(data).hexdigest()


def load_object(text):
    def unique_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('duplicate JSON key')
            result[key] = value
        return result

    def reject_constant(value):
        raise ValueError('nonstandard JSON constant')

    result = json.loads(text, object_pairs_hook=unique_pairs,
                        parse_constant=reject_constant)
    if not isinstance(result, dict):
        raise ValueError('expected JSON object')
    return result


def same_value(actual, expected):
    # Python equality aliases bool/int and int/float; evidence types are binding.
    if type(actual) is not type(expected):
        return False
    if isinstance(actual, dict):
        return actual.keys() == expected.keys() and all(
            same_value(actual[key], expected[key]) for key in actual)
    if isinstance(actual, list):
        return len(actual) == len(expected) and all(
            same_value(a, e) for a, e in zip(actual, expected))
    return actual == expected


def summarize_frame(data, width=WIDTH, height=HEIGHT):
    if (type(width) is not int or type(height) is not int or
            width <= 0 or width % 2 or height <= 0 or len(data) != width * height * 4):
        raise ValueError('invalid planar YUV422p10 frame size')
    planes = []
    at = 0
    for name, samples_per_row in [('Y', width), ('U', width // 2), ('V', width // 2)]:
        size = samples_per_row * height * 2
        plane = data[at:at + size]
        values = struct.unpack('<' + 'H' * (size // 2), plane)
        if max(values) > 1023:
            raise ValueError('sample outside unsigned 10-bit range')
        planes.append(dict(name=name, pitch_bytes=samples_per_row * 2,
                           first=values[0], last=values[-1], sha256=sha(plane)))
        at += size
    return dict(sha256=sha(data), planes=planes)


def run_json(command):
    result = subprocess.run(command, check=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=120)
    return load_object(result.stdout)


def capture(ffmpeg, ffprobe):
    fixture = ROOT / FIXTURE
    common = [ffprobe, '-v', 'error', '-select_streams', 'v:0']
    stream = run_json(common + ['-show_entries',
        'stream=codec_name,codec_tag_string,width,height,pix_fmt,r_frame_rate,avg_frame_rate,nb_frames,field_order,bits_per_raw_sample:stream_tags=timecode',
        '-of', 'json', str(fixture)])['streams'][0]
    if (stream['codec_name'], stream['width'], stream['height'], stream['pix_fmt'],
            stream['nb_frames']) != ('cfhd', WIDTH, HEIGHT, 'yuv422p10le', str(COUNT)):
        raise ValueError('unexpected fixture stream')
    packets = run_json(common + ['-show_packets', '-show_data_hash', 'sha256',
        '-show_entries', 'packet=pts,dts,duration,size,flags,data_hash',
        '-of', 'json', str(fixture)])['packets']
    if len(packets) != COUNT or any('data_hash' not in p for p in packets):
        raise ValueError('unexpected packet inventory')
    # Disk-backed bounded output avoids retaining the sequence in RAM.
    with tempfile.TemporaryDirectory(prefix='hypercine-p7-') as temporary:
        raw_path = pathlib.Path(temporary) / 'decoded.yuv'
        subprocess.run([ffmpeg, '-v', 'error', '-xerror', '-threads', '1',
            '-c:v', 'cfhd', '-i', str(fixture), '-map', '0:v:0', '-an', '-sn', '-dn',
            '-frames:v', str(COUNT + 1), '-fps_mode', 'passthrough',
            '-pix_fmt', '+yuv422p10le', '-f', 'rawvideo', str(raw_path)],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, check=True, timeout=120)
        if raw_path.stat().st_size != COUNT * FRAME_BYTES:
            raise ValueError('missing, truncated or extra decoded frames')
        with raw_path.open('rb') as raw:
            frames = [dict(index=i, **summarize_frame(raw.read(FRAME_BYTES))) for i in range(COUNT)]
    version = subprocess.run([ffmpeg, '-version'], check=True, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, timeout=10).stdout.decode().splitlines()[0]
    return dict(license='MIT OR Apache-2.0; Copyright (c) 2026 HyperCine Contributors',
                schema=1, oracle='independent FFmpeg cfhd; no SDK pixel comparison',
                fixture=FIXTURE, fixture_sha256=sha(fixture.read_bytes()),
                ffmpeg_version=version, stream=stream, packets=packets,
                output=dict(format='yuv422p10le', orientation='top-down',
                            byte_order='little-endian', frame_bytes=FRAME_BYTES,
                            frame_count=COUNT, frames=frames),
                metadata_limits=dict(timecode='stream tag only if present; packet tags not parsed',
                                     fps='container stream rate; not inferred from CFHD packet',
                                     profile='GOP/quality/field semantics not established by this probe'),
                sdk_pixel_parity='PENDING_MATCHED_ORIGINAL_SDK_OUTPUT')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ffmpeg', required=True)
    parser.add_argument('--ffprobe', required=True)
    parser.add_argument('--output', type=pathlib.Path)
    parser.add_argument('--verify', type=pathlib.Path)
    args = parser.parse_args()
    try:
        report = capture(args.ffmpeg, args.ffprobe)
        if args.verify:
            expected = load_object(args.verify.read_text(encoding='utf-8'))
            # Version provenance may differ; compressed bytes and decoded pixels may not.
            for key in ['schema', 'fixture_sha256', 'stream', 'packets', 'output']:
                if not same_value(report[key], expected[key]):
                    raise ValueError('baseline mismatch: ' + key)
        if args.output:
            args.output.write_text(json.dumps(report, indent=2) + '\n')
        print('PASS: 24 CFHD packets and 24 complete YUV422p10 frames')
    except (ValueError, TypeError, UnicodeError, KeyError, IndexError, OSError, RecursionError,
            subprocess.SubprocessError):
        # Raw tool errors can contain local paths or arbitrary input metadata.
        parser.exit(1, 'FAIL: oracle unavailable, invalid fixture, decode error or baseline mismatch\n')


if __name__ == '__main__':
    main()
