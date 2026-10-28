# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Acceptance checks stay active with Python -O; portable report provenance."""
import re
import array
import sys


def require(condition, message='validation failed'):
    if not condition:
        raise ValueError(str(message))


def integer(value, minimum=0, maximum=2**64-1):
    require(type(value) is int and minimum <= value <= maximum, 'invalid integer field')
    return value


def hex_digest(value, length=64):
    require(isinstance(value, str) and re.fullmatch('[0-9a-f]{%d}' % length, value),
            'invalid digest field')


def p10_samples(data, width, height):
    """Validate tightly packed planar YUV422 p10 LE before comparing pixels."""
    integer(width, 2)
    integer(height, 1)
    require(width % 2 == 0, 'p10 YUV422 width must be even')
    require(len(data) == width * height * 4, 'p10 storage length mismatch')
    pixels = array.array('H', data)
    if sys.byteorder != 'little':
        pixels.byteswap()
    require(all(value <= 1023 for value in pixels), 'p10 sample outside 0..1023')
    return pixels


def portable_command(script, arguments):
    """Retain only recognized switches and numeric frame indices, never paths.

    This is a reproduction template, not the literal local execution command.
    Unknown arguments fail closed rather than entering a public report.
    """
    paths = {'--probe': 'HYPERCINE_PROBE', '--encoder': 'HYPERCINE_ENCODER',
             '--cpu-encoder': 'HYPERCINE_CPU_ENCODER', '--corpus-root': 'HYPERCINE_GOLDEN_DIR',
             '--stage-dir': 'HYPERCINE_STAGE_DIR', '--output': 'HYPERCINE_REPORT',
             '--output-dir': 'HYPERCINE_OUTPUT_DIR', '--report': 'HYPERCINE_REPORT',
             '--ffmpeg': 'HYPERCINE_FFMPEG'}
    flags = {'--sequence', '--inject-metadata', '--inputs-only', '--sdk-replay'}
    result = ['python3', 'tests/' + script]
    args = iter(arguments)
    frames = False
    for arg in args:
        option, equal, value = arg.partition('=')
        if option in paths:
            if not equal:
                require(next(args, None) is not None, 'missing command path argument')
            result.extend([option, '${' + paths[option] + '}'])
            frames = False
        elif arg in flags:
            result.append(arg)
            frames = False
        elif arg == '--frames':
            result.append(arg)
            frames = True
        elif frames and re.fullmatch('[0-9]+', arg):
            result.append(arg)
        else:
            raise ValueError('unsupported command provenance argument')
    return result
