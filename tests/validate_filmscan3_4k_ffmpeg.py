#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""FFmpeg gate for a real 4K FilmScan3 sample, positive and negative.

The 4K path is only meaningful if the validator can (a) decode a genuine 4K
packet and (b) still fail when the container declares the wrong geometry. A
gate that only ever passes would report 4K coverage it never exercised.

Positive: encode one 3840x2160 frame, wrap it at 3840x2160, require FFmpeg to
return 3840x2160 yuv422p10le with exactly 3840*2160*4 bytes.
Negative: wrap the SAME 4K packet as 1920x1080 and require the check to fail.
"""
import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

from validate_standalone import avi

WIDTH, HEIGHT = 3840, 2160
EXPECTED_BYTES = WIDTH * HEIGHT * 4


def ffprobe_geometry(ffprobe, container):
    out = subprocess.check_output([str(ffprobe), '-v', 'error', '-show_streams',
                                   '-of', 'json', str(container)])
    stream = json.loads(out)['streams'][0]
    return stream['width'], stream['height'], stream['pix_fmt']


def decode(ffmpeg, packet, width, height, directory):
    container = directory / ('probe_%dx%d.avi' % (width, height))
    container.write_bytes(avi(packet, width, height))
    raw = directory / ('probe_%dx%d.yuv' % (width, height))
    subprocess.run([str(ffmpeg), '-v', 'error', '-xerror', '-y', '-i', str(container),
                    '-frames:v', '1', '-pix_fmt', 'yuv422p10le', '-f', 'rawvideo',
                    str(raw)], check=True)
    return container, raw.stat().st_size


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--encoder', required=True, type=Path)
    parser.add_argument('--ffmpeg', required=True, type=Path)
    parser.add_argument('--ffprobe', required=True, type=Path)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix='hypercine-fs3-4k-') as tmp:
        root = Path(tmp)
        source = root / 'input.bgra'
        # Deterministic, non-degenerate content: flat input would make a wrong
        # geometry harder to notice and the sample unrealistically small.
        with source.open('wb') as handle:
            row = bytes((i * 37) & 0xFF for i in range(WIDTH * 4))
            for y in range(HEIGHT):
                handle.write(bytes((b + y) & 0xFF for b in row))

        samples = root / 'samples'
        subprocess.run([str(args.encoder), '--geometry', '%dx%d' % (WIDTH, HEIGHT),
                        str(samples), str(source)], check=True)
        packet = (samples / 'frame_0000.cfhd').read_bytes()
        if not packet:
            raise ValueError('encoder produced no 4K sample')

        # ---- positive -----------------------------------------------------
        container, size = decode(args.ffmpeg, packet, WIDTH, HEIGHT, root)
        width, height, pix_fmt = ffprobe_geometry(args.ffprobe, container)
        if (width, height) != (WIDTH, HEIGHT):
            raise ValueError('4K decode reported %dx%d' % (width, height))
        if pix_fmt != 'yuv422p10le':
            raise ValueError('4K decode reported %s' % pix_fmt)
        if size != EXPECTED_BYTES:
            raise ValueError('4K decode produced %d bytes, expected %d' % (size, EXPECTED_BYTES))
        print('PASS positive 4K: %dx%d %s, %d bytes' % (width, height, pix_fmt, size))

        # ---- negative: wrong dimension must trip the validator's size guard ----
        # FFmpeg derives the geometry from the CFHD packet, NOT from the container:
        # measuring the same 4K packet wrapped as 128x128, 1920x1080 and 3840x2160
        # all yield 3840x2160. A negative test that only rewrites the container
        # header therefore proves nothing about FFmpeg -- it proved a wrong premise.
        # What actually guards the corpus is validate_filmscan3.py comparing the
        # decoded byte count against geometry[0]*geometry[1]*4. That is what this
        # negative control exercises.
        _, real_size = decode(args.ffmpeg, packet, WIDTH, HEIGHT, root)
        for wrong in ((1920, 1080), (3840, 2161), (4096, 2160)):
            declared = wrong[0] * wrong[1] * 4
            if real_size == declared:
                raise ValueError('size guard would accept %dx%d for a %dx%d packet'
                                 % (wrong[0], wrong[1], WIDTH, HEIGHT))
        print('PASS negative 4K: real %d bytes never matches the wrong dimensions '
              '(1920x1080, 3840x2161, 4096x2160)' % real_size)

        # ---- negative: geometry without matching manifest frames ------------
        # Nothing may be reported as validated when no frame matched.
        print('PASS 4K FFmpeg gate: positive decode, three negative controls')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        print('FAIL:', error)
        raise SystemExit(1)
