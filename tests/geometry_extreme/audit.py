#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""C0 arithmetic model only: allocates no frames and invokes no codec/GPU."""
import json

U64_MAX = (1 << 64) - 1
CHUNK_MAX = ((1 << 24) - 1) * 4
GEOMETRIES = ((3840, 2160), (7680, 4320), (12288, 8040), (17520, 8040))


def size_integer(*values):
    if any(type(value) is not int for value in values):
        raise ValueError('integer size required')


def add(a, b, limit=U64_MAX):
    size_integer(a, b, limit)
    if min(a, b) < 0 or a > limit or b > limit - a:
        raise OverflowError('size addition')
    return a + b


def mul(a, b, limit=U64_MAX):
    size_integer(a, b, limit)
    if min(a, b) < 0 or a > limit or (a and b > limit // a):
        raise OverflowError('size multiplication')
    if b > limit:
        raise OverflowError('size operand')
    return a * b


def align(n, alignment):
    size_integer(n, alignment)
    if n < 0:
        raise ValueError('negative size')
    if alignment < 1 or alignment & (alignment - 1):
        raise ValueError('power-of-two alignment required')
    return add(n, alignment - 1) & -alignment


def chunk_header(key, payload_bytes):
    """Model Writer::End, without allocating a payload or emitting a sample."""
    size_integer(key, payload_bytes)
    if key not in (0x2000, 0x2100, 0x2200):
        raise ValueError('unsupported audit chunk')
    if payload_bytes < 0 or payload_bytes % 4 or payload_bytes > CHUNK_MAX:
        raise ValueError('unrepresentable chunk content')
    words = payload_bytes // 4
    return (((-(key | (words >> 16))) & 0xffff) << 16) | (words & 0xffff)


def surface(w, h):
    size_integer(w, h)
    if min(w, h) < 0:
        raise ValueError('negative surface dimension')
    return mul(mul(align(w, 8), h), 2)


def geometry(w, h):
    size_integer(w, h)
    if w < 128 or h < 24 or w % 16 or h % 8 or max(w, h) > 65535:
        raise ValueError('outside C0 geometry domain')
    pixels = mul(w, h)
    bgra = mul(pixels, 4)
    color = sum(surface(cw, h) for cw in (w, w // 2, w // 2))
    bands = readback = 0
    for cw in (w, w // 2, w // 2):
        ch = h
        for level in range(3):
            cw //= 2
            ch //= 2
            size = surface(cw, ch)
            bands = add(bands, mul(size, 4))
            readback = add(readback, mul(size, 4 if level == 2 else 3))
    scratch = mul(surface(w // 2, h), 2)
    workspace = add(add(bgra, color), add(bands, scratch))
    # Caller BGRA + GPU shared workspace + production host band copies.
    subtotal = add(add(bgra, workspace), readback)
    return dict(width=w, height=h, pixels=pixels, bgra8_bytes=bgra,
                yuv422p10le_bytes=bgra, rgb444p12le_bytes=mul(pixels, 6),
                rgba4444p12le_bytes=mul(pixels, 8),
                l3_luma_width=w // 8, l3_chroma_width=w // 16,
                l3_chroma_stride_samples=align(w // 16, 8),
                l3_height=h // 8, width_mod_128=w % 128,
                metal_workspace_bytes=workspace,
                metal_production_readback_bytes=readback,
                caller_workspace_readback_subtotal_bytes=subtotal,
                # Not an output size prediction; smallest enclosing sample
                # content max / pixel count, excluding external sample header.
                sample_content_limit_bytes=CHUNK_MAX,
                hypercine_main='UNSUPPORTED_GEOMETRY',
                sdk='SDK_ORACLE_PENDING', ffmpeg='REFERENCE_PENDING')


def report():
    return dict(license='MIT OR Apache-2.0',
                copyright='Copyright (c) 2026 HyperCine Contributors.',
                source_commit='e87ef0258ba37f5c78f1557c59d9c7331df5521c',
                scope='C0 arithmetic estimates; no encode/decode or allocation',
                geometries=[geometry(w, h) for w, h in GEOMETRIES])


if __name__ == '__main__':
    print(json.dumps(report(), indent=2, sort_keys=True))
