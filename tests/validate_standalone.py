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
import time


def chunk(tag, data):
    return tag + struct.pack('<I', len(data)) + data + bytes(len(data) & 1)


def avi(packet, width=128, height=128):
    avih = struct.pack('<14I', 40000, 0, 0, 0, 1, 0, 1, len(packet), width, height, 0, 0, 0, 0)
    strh = struct.pack('<4s4sIHH8I4h', b'vids', b'CFHD', 0, 0, 0, 0, 1, 25, 0, 1,
                       len(packet), 0xffffffff, 0, 0, 0, width, height)
    strf = struct.pack('<IiiHH4sIiiII', 40, width, height, 1, 24, b'CFHD', len(packet), 0, 0, 0, 0)
    hdrl = chunk(b'LIST', b'hdrl' + chunk(b'avih', avih) +
                 chunk(b'LIST', b'strl' + chunk(b'strh', strh) + chunk(b'strf', strf)))
    return chunk(b'RIFF', b'AVI ' + hdrl + chunk(b'LIST', b'movi' + chunk(b'00dc', packet)))


def reference(pattern, width=128, height=128):
    rows = []
    seed = 0x1337beef
    for y in range(height):
        row = []
        for x in range(width):
            if pattern == 'flat':
                b, g, r = 128, 128, 128
            elif pattern == 'gradient':
                gx, gy = x*255//width, y*255//height
                check = 200 if ((x//32) ^ (y//32)) & 1 else 50
                b, g, r = (gx+check)//2, (gy+check)//2, gx ^ gy
            elif pattern == 'noise':
                seed = (seed*1664525+1013904223) & 0xffffffff
                b, g, r = seed & 255, (seed >> 8) & 255, (seed >> 16) & 255
            elif pattern == 'checker':
                b = g = r = 255 if (x ^ y) & 1 else 0
            elif pattern == 'edges':
                b, g, r = ((255,0,0) if x < width//2 else (0,0,255))
                if x in (0,width-1) or y in (0,height-1): b,g,r = 0,255,0
            elif pattern == 'impulse':
                b = g = r = 255 if (x,y) in ((0,0),(1,1),(2,2),(width-1,height-1),(width-2,height-2)) else 0
            elif pattern == 'canonical':
                seed = (seed*1664525+1013904223) & 0xffffffff
                gx, gy, noise = x*255//width, y*255//height, (seed >> 24) & 15
                b, g, r = (gx*13+noise)//14, (gy*9+(noise >> 1))//10, (gx//2+gy//2+noise)//2
            else:
                raise ValueError("unknown pattern: " + pattern)
            row.append((b, g, r))
        rows.append(row)
    planes = [[], [], []]  # Y, Cb, Cr: FFmpeg rawvideo order
    for row in rows:
        for b, g, r in row:
            planes[0].append(64+(11966*r+40254*g+4064*b+8192)//16384)
        for x in range(0, width, 2):
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
    parser.add_argument('--width', type=int, default=128)
    parser.add_argument('--height', type=int, default=128)
    parser.add_argument('--patterns', nargs='+', default=['flat','gradient','canonical'])
    args = parser.parse_args()
    width, height = args.width, args.height
    if not (128 <= width <= 4096 and width%128==0 and 24 <= height <= 4096 and height%8==0 and width*height<=2097152):
        parser.error("unsupported encoder geometry")
    count = width*height*2
    with tempfile.TemporaryDirectory(prefix='hypercine-decode-') as temporary:
        root = Path(args.output_dir or temporary)
        root.mkdir(parents=True, exist_ok=True)
        results = []
        for pattern in args.patterns:
            packet_path = root / (pattern+'.cfhd')
            rows, planes = reference(pattern, width, height)
            input_name = pattern
            if pattern not in ('flat','gradient','canonical'):
                input_path = root / (pattern+'-input.bgra')
                input_path.write_bytes(bytes(v for row in rows for pixel in row for v in (*pixel,255)))
                input_name = 'bgra:' + str(input_path)
            started = time.monotonic()
            subprocess.run([args.encoder, input_name, str(packet_path), str(width), str(height)], check=True)
            encode_seconds = time.monotonic()-started
            packet = packet_path.read_bytes()
            subprocess.run([args.encoder, input_name, str(packet_path), str(width), str(height)], check=True)
            assert packet_path.read_bytes() == packet, 'non-deterministic repeated CLI encode'
            container = root / (pattern+'.avi')
            container.write_bytes(avi(packet, width, height))
            probe = subprocess.check_output([args.ffprobe, '-v', 'error', '-show_streams', '-of', 'json', str(container)])
            stream = json.loads(probe)['streams'][0]
            assert (stream['codec_name'], stream['width'], stream['height'], stream['pix_fmt']) == ('cfhd', width, height, 'yuv422p10le'), stream
            output = root / (pattern+'.yuv')
            subprocess.run([args.ffmpeg, '-v', 'error', '-xerror', '-y', '-i', str(container), '-frames:v', '1',
                            '-c:v', 'rawvideo', '-pix_fmt', 'yuv422p10le', '-f', 'rawvideo', str(output)], check=True)
            raw = output.read_bytes()
            assert len(raw) == width*height*4, len(raw)
            decoded = struct.unpack('<%dH' % count, raw)
            assert all(v <= 1023 for v in decoded), 'p10 range'
            expected = sum(planes, [])
            errors = [a-b for a, b in zip(decoded, expected)]
            plane_metrics = []
            offset = 0
            for name, values in zip(('Y','Cb','Cr'), planes):
                delta = errors[offset:offset+len(values)]
                squares = sum(e*e for e in delta)/len(delta)
                first = next((i for i,e in enumerate(delta) if e), None)
                plane_metrics.append(dict(channel=name, samples=len(values), exact_samples=sum(e==0 for e in delta),
                                          max_error=max(map(abs,delta)),
                                          psnr_db=round(10*math.log10(1023**2/squares),6) if squares else None,
                                          first_difference=dict(x=first%(width if name=='Y' else width//2),
                                                                y=first//(width if name=='Y' else width//2),
                                                                expected=values[first], decoded=decoded[offset+first]) if first is not None else None))
                offset += len(values)
            mse = sum(e*e for e in errors)/len(errors)
            psnr = 10*math.log10(1023**2/mse) if mse else 999.0
            rgb_path = root / (pattern+'.bgra')
            subprocess.run([args.ffmpeg, '-v', 'error', '-xerror', '-y', '-i', str(container),
                            '-vf', 'scale=in_color_matrix=bt709:out_color_matrix=bt709:in_range=limited:out_range=full:flags=bicubic',
                            '-frames:v', '1', '-c:v', 'rawvideo', '-pix_fmt', 'bgra', '-f', 'rawvideo', str(rgb_path)], check=True)
            rgb = rgb_path.read_bytes()
            assert len(rgb) == width*height*4
            rgb_errors = [rgb[(y*width+x)*4+c]-rows[y][x][c]
                          for y in range(height) for x in range(width) for c in range(3)]
            rgb_mse = sum(e*e for e in rgb_errors)/len(rgb_errors)
            rgb_psnr = 10*math.log10(255**2/rgb_mse) if rgb_mse else None
            # Isolate the format's color/subsampling loss without any wavelet,
            # quantization or entropy. FFmpeg only reconstructs reference YUV.
            reference_path = root / (pattern+'-reference.yuv')
            reference_path.write_bytes(struct.pack('<%dH' % count, *expected))
            baseline_path = root / (pattern+'-reference.bgra')
            subprocess.run([args.ffmpeg, '-v', 'error', '-xerror', '-y', '-f', 'rawvideo',
                            '-pixel_format', 'yuv422p10le', '-video_size', '%dx%d' % (width, height), '-i', str(reference_path),
                            '-vf', 'scale=in_color_matrix=bt709:out_color_matrix=bt709:in_range=limited:out_range=full:flags=bicubic',
                            '-frames:v', '1', '-c:v', 'rawvideo', '-pix_fmt', 'bgra', '-f', 'rawvideo', str(baseline_path)], check=True)
            baseline = baseline_path.read_bytes()
            assert len(baseline) == len(rgb)
            baseline_errors = [baseline[(y*width+x)*4+c]-rows[y][x][c]
                               for y in range(height) for x in range(width) for c in range(3)]
            baseline_mse = sum(e*e for e in baseline_errors)/len(baseline_errors)
            baseline_psnr = 10*math.log10(255**2/baseline_mse) if baseline_mse else None
            result = dict(pattern=pattern, sample_bytes=len(packet), sample_sha256=hashlib.sha256(packet).hexdigest(),
                          decoded_sha256=hashlib.sha256(raw).hexdigest(), psnr_yuv_p10_db=round(psnr, 6) if mse else None,
                          yuv_exact=(mse == 0), planes=plane_metrics,
                          psnr_rgb_subsampling_baseline_db=round(baseline_psnr, 6) if baseline_psnr else None,
                          psnr_source_rgb_db=round(rgb_psnr, 6) if rgb_psnr else None,
                          encode_process_seconds=round(encode_seconds,6),
                          max_error=max(map(abs, errors)), format=stream['pix_fmt'], width=width, height=height)
            results.append(result)
            print(json.dumps(result), flush=True)
            if width == height == 128 and pattern in ('flat','gradient','canonical'):
                evidence = json.loads((Path(__file__).resolve().parents[1]/'docs/validation/macos_standalone_2026-10-14.json').read_text())
                golden = next(item for item in evidence['samples'] if item['pattern']==pattern)
                assert result['sample_sha256'] == golden['sample_sha256'], 'original 128 packet changed'
                assert result['decoded_sha256'] == golden['decoded_sha256'], 'original decoded 128 golden changed'
            assert psnr > 35, result
            if pattern == 'flat':
                assert mse == 0, result
            # GradientChecker has discontinuous chroma above 4:2:2's horizontal
            # bandwidth. Record its RGB loss separately; do not relabel it as
            # quantization loss or assert universal RGB quality for this format.
            if pattern in ('flat','canonical'):
                assert rgb_psnr is None or rgb_psnr > 35, result
        if args.output_dir:
            (root/'validation.json').write_text(json.dumps(results, indent=2)+'\n')


if __name__ == '__main__':
    main()
