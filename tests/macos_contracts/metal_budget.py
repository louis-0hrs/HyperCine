#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Read-only model of existing resident Metal buffers; no GPU allocation."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def budget(width, height, padding=0):
    if (type(width) is not int or width <= 0 or width % 2 or
            type(height) is not int or height <= 0 or
            type(padding) is not int or padding < 0):
        raise ValueError('invalid Metal memory-model geometry or padding')
    row = width * 4
    stride = row + padding
    buffers = [stride * (height - 1) + row]
    for channel in range(3):
        w, h = (width // 2 if channel else width), height
        buffers.append(((w + 7) // 8 * 8) * h * 2)
        for _ in range(3):
            w, h = w // 2, h // 2
            buffers.extend([((w + 7) // 8 * 8) * h * 2] * 4)
    scratch = ((width // 2 + 7) // 8 * 8) * height * 2
    buffers.extend([scratch, scratch])
    if len(buffers) != 42:
        raise ValueError('resident Metal buffer inventory mismatch')
    return dict(width=width, height=height, stride_bytes=stride,
                buffer_count=len(buffers), resident_bytes=sum(buffers),
                largest_buffer_bytes=max(buffers),
                input_index_product=stride * height,
                input_index_fits_uint32=stride * height <= 0xffffffff,
                status='ESTIMATE_ONLY')


def main():
    rows = [budget(w, h) for w, h in [(1920, 1080), (3840, 2160),
            (7680, 4320), (12288, 8040), (17520, 8040)]]
    # Check padding accounting independently: one input copy, no trailing pad.
    for row in rows:
        padded = budget(row['width'], row['height'], 16)
        if padded['resident_bytes'] - row['resident_bytes'] != 16 * (row['height'] - 1):
            raise ValueError('resident Metal input padding accounting mismatch')
    # Atomic replacement keeps both workspaces alive until assignment completes.
    # This transient estimate excludes allocator/driver overhead and host copies.
    transitions = [dict(old=old['width'], new=new['width'],
                        minimum_overlap_bytes=old['resident_bytes'] + new['resident_bytes'])
                   for old, new in zip(rows, rows[1:])]
    print(json.dumps(dict(schema=1, license='MIT OR Apache-2.0; Copyright (c) 2026 HyperCine Contributors', source_sha256=hashlib.sha256(
        (ROOT / 'src/metal/filmscan3_metal.mm').read_bytes()).hexdigest(),
        rows=rows, transitions=transitions,
        caveats=['No Metal execution or measured device limits',
                 'CPU planes, downloads, samples and driver allocations excluded',
                 'maxBufferLength and available shared RAM must be queried at runtime',
                 'Current main rejects 4K+ before workspace preparation']), indent=2))


if __name__ == '__main__':
    main()
