#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Optional external decoder gate. No FFmpeg code is linked into HyperCine.

Wrap one encoder-produced CFHD packet in a minimal unindexed AVI, decode using
FFmpeg, check geometry/format/quality against independently calculated planes.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile


def chunk(tag, data):
    return tag + struct.pack('<I', len(data)) + data + bytes(len(data) & 1)


def avi(packet):
    avih = struct.pack('<14I', 40000, 0, 0, 0, 1, 0, 1, len(packet), 128, 128, 0, 0, 0, 0)
    strh = struct.pack('<4s4sIHH8I4h', b'vids', b'CFHD', 0, 0, 0, 0, 1, 25, 0, 1,
                       len(packet), 0xffffffff, 0, 0, 0, 128, 128)
    strf = struct.pack('<IiiHH4sIiiII', 40, 128, 128, 1, 24, b'CFHD', len(packet), 0, 0, 0, 0)
    hdrl = chunk(b'LIST', b'hdrl' + chunk(b'avih', avih) +
                 chunk(b'LIST', b'strl' + chunk(b'strh', strh) + chunk(b'strf', strf)))
    return chunk(b'RIFF', b'AVI ' + hdrl + chunk(b'LIST', b'movi' + chunk(b'00dc', packet)))


def reference(pattern):
    rows = []
    seed = 0x1337beef
    for y in range(128):
        row = []
        for x in range(128):
            if pattern == 'flat':
                b, g, r = 128, 128, 128
            elif pattern == 'gradient':
                gx, gy = x*255//128, y*255//128
                check = 200 if ((x//32) ^ (y//32)) & 1 else 50
                b, g, r = (gx+check)//2, (gy+check)//2, gx ^ gy
            else:
                seed = (seed*1664525+1013904223) & 0xffffffff
                gx, gy, noise = x*255//128, y*255//128, (seed >> 24) & 15
                b, g, r = (gx*13+noise)//14, (gy*9+(noise >> 1))//10, (gx//2+gy//2+noise)//2
            row.append((b, g, r))
        rows.append(row)
    planes = [[], [], []]  # Y, Cb, Cr: FFmpeg rawvideo order
    for row in rows:
        for b, g, r in row:
            planes[0].append(64+(11966*r+40254*g+4064*b+8192)//16384)
        for x in range(0, 128, 2):
            cb, cr = 0, 0
            for b, g, r in row[x:x+2]:
                cb += -6596*r-22189*g+28784*b
                cr += 28784*r-26145*g-2639*b
            planes[1].append(512+(cb+16384)//32768)
            planes[2].append(512+(cr+16384)//32768)
    return rows, planes


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--encoder', required=True)
    parser.add_argument('--ffmpeg', required=True)
    parser.add_argument('--ffprobe', required=True)
    parser.add_argument('--output-dir')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='hypercine-decode-') as temporary:
        root = Path(args.output_dir or temporary)
        root.mkdir(parents=True, exist_ok=True)
        results = []
        for pattern in ('flat', 'gradient', 'canonical'):
            packet_path = root / (pattern+'.cfhd')
            subprocess.run([args.encoder, pattern, str(packet_path)], check=True)
            packet = packet_path.read_bytes()
            container = root / (pattern+'.avi')
            container.write_bytes(avi(packet))
            probe = subprocess.check_output([args.ffprobe, '-v', 'error', '-show_streams', '-of', 'json', str(container)])
            stream = json.loads(probe)['streams'][0]
            assert (stream['codec_name'], stream['width'], stream['height'], stream['pix_fmt']) == ('cfhd', 128, 128, 'yuv422p10le'), stream
            output = root / (pattern+'.yuv')
            subprocess.run([args.ffmpeg, '-v', 'error', '-xerror', '-y', '-i', str(container), '-frames:v', '1',
                            '-c:v', 'rawvideo', '-pix_fmt', 'yuv422p10le', '-f', 'rawvideo', str(output)], check=True)
            raw = output.read_bytes()
            assert len(raw) == 65536, len(raw)
            decoded = struct.unpack('<32768H', raw)
            rows, planes = reference(pattern)
            expected = sum(planes, [])
            errors = [a-b for a, b in zip(decoded, expected)]
            mse = sum(e*e for e in errors)/len(errors)
            psnr = 10*math.log10(1023**2/mse) if mse else 999.0
            rgb_path = root / (pattern+'.bgra')
            subprocess.run([args.ffmpeg, '-v', 'error', '-xerror', '-y', '-i', str(container),
                            '-vf', 'scale=in_color_matrix=bt709:out_color_matrix=bt709:in_range=limited:out_range=full:flags=bicubic',
                            '-frames:v', '1', '-c:v', 'rawvideo', '-pix_fmt', 'bgra', '-f', 'rawvideo', str(rgb_path)], check=True)
            rgb = rgb_path.read_bytes()
            assert len(rgb) == 65536
            rgb_errors = [rgb[(y*128+x)*4+c]-rows[y][x][c]
                          for y in range(128) for x in range(128) for c in range(3)]
            rgb_mse = sum(e*e for e in rgb_errors)/len(rgb_errors)
            rgb_psnr = 10*math.log10(255**2/rgb_mse) if rgb_mse else None
            # Isolate the format's color/subsampling loss without any wavelet,
            # quantization or entropy. FFmpeg only reconstructs reference YUV.
            reference_path = root / (pattern+'-reference.yuv')
            reference_path.write_bytes(struct.pack('<32768H', *expected))
            baseline_path = root / (pattern+'-reference.bgra')
            subprocess.run([args.ffmpeg, '-v', 'error', '-xerror', '-y', '-f', 'rawvideo',
                            '-pixel_format', 'yuv422p10le', '-video_size', '128x128', '-i', str(reference_path),
                            '-vf', 'scale=in_color_matrix=bt709:out_color_matrix=bt709:in_range=limited:out_range=full:flags=bicubic',
                            '-frames:v', '1', '-c:v', 'rawvideo', '-pix_fmt', 'bgra', '-f', 'rawvideo', str(baseline_path)], check=True)
            baseline = baseline_path.read_bytes()
            assert len(baseline) == len(rgb)
            baseline_errors = [baseline[(y*128+x)*4+c]-rows[y][x][c]
                               for y in range(128) for x in range(128) for c in range(3)]
            baseline_mse = sum(e*e for e in baseline_errors)/len(baseline_errors)
            baseline_psnr = 10*math.log10(255**2/baseline_mse) if baseline_mse else None
            result = dict(pattern=pattern, sample_bytes=len(packet), sample_sha256=hashlib.sha256(packet).hexdigest(),
                          decoded_sha256=hashlib.sha256(raw).hexdigest(), psnr_yuv_p10_db=round(psnr, 6) if mse else None,
                          yuv_exact=(mse == 0),
                          psnr_rgb_subsampling_baseline_db=round(baseline_psnr, 6) if baseline_psnr else None,
                          psnr_source_rgb_db=round(rgb_psnr, 6) if rgb_psnr else None,
                          max_error=max(map(abs, errors)), format=stream['pix_fmt'], width=128, height=128)
            results.append(result)
            print(json.dumps(result), flush=True)
            assert psnr > 35, result
            if pattern == 'flat':
                assert mse == 0, result
            # GradientChecker has discontinuous chroma above 4:2:2's horizontal
            # bandwidth. Record its RGB loss separately; do not relabel it as
            # quantization loss or assert universal RGB quality for this format.
            if pattern != 'gradient':
                assert rgb_psnr is None or rgb_psnr > 35, result
        if args.output_dir:
            (root/'validation.json').write_text(json.dumps(results, indent=2)+'\n')


if __name__ == '__main__':
    main()
