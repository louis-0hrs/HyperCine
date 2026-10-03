# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Generate isolated timing wrappers in the build tree. Never write upstream.

Only function definitions are renamed; all existing call sites resolve through
the wrapper. Exclusive timing subtracts nested hooks. Fail on unexpected source
signatures rather than silently losing a stage. This is profiling, not a codec.
"""
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
selected = sys.argv[2]
targets = {
    'decoder.c': {
        'ParseSampleHeader': 0, 'UpdateCodecState': 0,
        'DecodeSampleLowPassBand': 1, 'DecodeFastRunsFSM16s': 1,
        'DeQuantFSM': 2,
        'ReconstructWaveletBand': 3, 'ReconstructSampleFrameToBuffer': 3,
        'ConvertYUVStripPlanarToBuffer': 5,
        'UpdateWaveletBandValidFlags': 6, 'UpdateWaveletBandStartedFlags': 6,
    },
    'bayer.c': {'ApplyActiveMetaData': 4, 'ConvertLinesToOutput': 5},
    'lutpath.cpp': {'OverrideCFHDDATA': 0},
    'convert.c': {'ConvertPlanarYUVToRGB': 5, 'ConvertPlanarYUVToYU64': 5,
                  'ConvertYUVStripPlanarToPacked': 5},
    'quantize.c': {'DequantizeBandRow16s': 2},
    'frame.c': {'ConvertLowpass16sToRGBNoIPPFast': 5},
    'spatial.c': {'InvertHorizontalStripYUV16sToPackedRGB32': 7},
}
for filename, functions in targets.items():
    if filename != selected:
        continue
    source = (root / 'upstream' / 'Codec' / filename).read_text(encoding='utf-8-sig')
    for name, stage in functions.items():
        pattern = re.compile(r'^(?P<ret>(?:bool|void|CODEC_ERROR)\s*\*?\s*)'
                             + re.escape(name) + r'\((?P<args>[^;{}]*?)\)\s*(?://[^\n]*\n\s*)?\{', re.M)
        def replace(match):
            ret, args = match['ret'].strip(), match['args']
            clean = re.sub(r'//[^\n]*|/\*.*?\*/', '', args, flags=re.S)
            if clean.strip() == 'HorizontalFilterParams':
                header = (root / 'upstream' / 'Codec' / 'wavelet.h').read_text(encoding='utf-8-sig')
                macro = re.search(r'^#define HorizontalFilterParams ((?:[^\n]*\\\n)*[^\n]*)', header, re.M)
                if not macro:
                    raise RuntimeError('missing HorizontalFilterParams declaration')
                clean = re.sub(r'/\*.*?\*/', '', macro[1], flags=re.S).replace('\\\n', ' ')
            names = []
            for arg in clean.split(','):
                words = re.findall(r'\b[A-Za-z_]\w*\b', arg)
                if not words:
                    raise RuntimeError(f'unsupported argument in {name}')
                names.append(words[-1])
            original = name + '_phase0_original'
            call = original + '(' + ', '.join(names) + ')'
            if ret == 'void':
                body = call + ';\n    DecodeProfileLeave();'
            else:
                body = ret + ' result = ' + call + ';\n    DecodeProfileLeave();\n    return result;'
            return (f'{ret} {original}({args});\n'
                    f'{ret} {name}({args}) {{\n    DecodeProfileEnter({stage});\n    {body}\n}}\n'
                    f'{ret} {original}({args}) {{')
        source, count = pattern.subn(replace, source)
        if count != 1:
            raise RuntimeError(f'{filename}: expected one definition of {name}, got {count}')
    source = '#include "decode_profile_hooks.h"\n' + source
    sys.stdout.buffer.write(source.encode('utf-8'))
