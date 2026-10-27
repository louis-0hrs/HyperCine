#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Apply the test-only RCA-160 band-0 gate to a private SDK copy.

The frozen repository tree is never modified. Pass the root of an out-of-repo
copy (the directory that contains upstream/). The script is deterministic,
uses function/branch anchors instead of line numbers, and fails closed if any
anchor is missing or ambiguous.
"""
import argparse
import hashlib
import shutil
import sys
from pathlib import Path

PROBE_HEADER = Path(__file__).with_name('rca160_probe.h')

GATE_BLOCK = '''\t\t\t// Check that all of the wavelet bands have been decoded
\t\t\t//assert(BANDS_ALL_VALID(wavelet));
\t\t\tif (!BANDS_ALL_VALID(wavelet))
\t\t\t{
\t\t\t\tdecoder->error = CODEC_ERROR_BAD_FRAME;
\t\t\t\treturn;
\t\t\t}'''

GATE_REPLACEMENT = '''\t\t\t// RCA-160 test-only admission gate: the original predicate is kept
\t\t\t// when RCA160_STRICT_GATE is off; when on it additionally requires
\t\t\t// band 0 of the input wavelet to be valid.
\t\t\tif (!RCA160Band0Gate(decoder, "spatial", channel, index, wavelet))
\t\t\t{
\t\t\t\tdecoder->error = CODEC_ERROR_BAD_FRAME;
\t\t\t\treturn;
\t\t\t}'''

PUBLISH_BLOCK = '''\t\t// Update the wavelet band flags
\t\twavelet->band_valid_flags |= BAND_VALID_MASK(band);
\t\twavelet->band_started_flags |= BAND_VALID_MASK(band);'''

PUBLISH_REPLACEMENT = '''\t\t// Update the wavelet band flags
\t\twavelet->band_valid_flags |= BAND_VALID_MASK(band);
\t\twavelet->band_started_flags |= BAND_VALID_MASK(band);
\t\trca160_new_valid = wavelet->band_valid_flags;
\t\trca160_new_started = wavelet->band_started_flags;
\t\trca160_event_ns = RCA160NowNs();
\t\trca160_event_tid = RCA160ThreadId();'''

PUBLISH_LOCALS = '''\t\tuint32_t rca160_old_valid = wavelet->band_valid_flags;
\t\tuint32_t rca160_old_started = wavelet->band_started_flags;
\t\tuint32_t rca160_new_valid;
\t\tuint32_t rca160_new_started;
\t\tunsigned long long rca160_event_ns;
\t\tunsigned long rca160_event_tid;'''

PUBLISH_CALL = '''\t\tRCA160ProbePublish(wavelet, band, rca160_old_valid, rca160_old_started,
\t\t\t\t\t\t   rca160_new_valid, rca160_new_started,
\t\t\t\t\t\t   rca160_event_ns, rca160_event_tid);
'''

STARTED_BLOCK = '''\t\twavelet->band_started_flags |= BAND_VALID_MASK(band);'''

STARTED_REPLACEMENT = '''\t\twavelet->band_started_flags |= BAND_VALID_MASK(band);
\t\trca160_started_new = wavelet->band_started_flags;
\t\trca160_started_ns = RCA160NowNs();
\t\trca160_started_tid = RCA160ThreadId();'''

STARTED_LOCALS = '''\t\tuint32_t rca160_started_old = wavelet->band_started_flags;
\t\tuint32_t rca160_started_new;
\t\tunsigned long long rca160_started_ns;
\t\tunsigned long rca160_started_tid;'''

STARTED_CALL = '''\t\tRCA160ProbeStarted(wavelet, band, rca160_started_old,
\t\t\t\t\t\t  rca160_started_new, rca160_started_ns,
\t\t\t\t\t\t  rca160_started_tid);
'''

ROUTE_ANCHOR = '''\t//IMAGE *image = frame->channel[0];
\tbool inverted = false;
\tint output_width = info->width;
\tint output_height = info->height;
\tint descale = precision - 8;'''

ROUTE_REPLACEMENT = '''\t//IMAGE *image = frame->channel[0];
\tbool inverted = false;
\tint output_width = info->width;
\tint output_height = info->height;
\tint descale = precision - 8;
\tRCA160ProbeRoute("CopyLowpass16sToBuffer", output_width, output_height,
\t\t\t\t\t output_pitch, info->format, encode_format, num_channels,
\t\t\t\t\t chroma_offset, precision);'''


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'anchor {label!r} occurred {count} times, expected 1')
    return text.replace(old, new, 1)


def replace_first(text, old, new, label, total_expected, replacements):
    count = text.count(old)
    if count != total_expected:
        raise SystemExit(f'anchor {label!r} occurred {count} times, expected {total_expected}')
    return text.replace(old, new, replacements)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', required=True,
                        help='out-of-repo copy root containing upstream/')
    args = parser.parse_args()

    root = Path(args.root).resolve()
    decoder = root / 'upstream' / 'Codec' / 'decoder.c'
    header_target = root / 'upstream' / 'Codec' / 'rca160_probe.h'
    if not decoder.is_file():
        raise SystemExit(f'missing {decoder}')
    if not PROBE_HEADER.is_file():
        raise SystemExit(f'missing probe header {PROBE_HEADER}')

    source = decoder.read_text(encoding='utf-8', errors='strict')
    if 'RCA160Band0Gate' in source:
        raise SystemExit('target already contains RCA-160 instrumentation')

    source = replace_once(source, '#include "lutpath.h"',
                          '#include "lutpath.h"\n#include "rca160_probe.h"',
                          'include')
    # The two spatial lowpass sites are the registered intervention. The third
    # textual match is the index==3 highpass site and is intentionally left
    # untouched.
    source = replace_first(source, GATE_BLOCK, GATE_REPLACEMENT, 'gate', 3, 2)

    publish_anchor = '''\tif (decoder != NULL && wavelet != NULL)
\t{

#if (1 && DEBUG)'''
    source = replace_once(source, publish_anchor,
                          publish_anchor.replace(
                              '\tif (decoder != NULL && wavelet != NULL)\n\t{\n',
                              '\tif (decoder != NULL && wavelet != NULL)\n\t{\n' + PUBLISH_LOCALS + '\n\n'),
                          'publish locals')
    source = replace_once(source, PUBLISH_BLOCK, PUBLISH_REPLACEMENT, 'publish')
    source = replace_once(source, '#endif\n\n\n\t}\n}\n\n\nvoid UpdateWaveletBandStartedFlags',
                          '#endif\n\n' + PUBLISH_CALL + '\n\t}\n}\n\n\nvoid UpdateWaveletBandStartedFlags',
                          'publish call')

    started_anchor = '''\tif (decoder != NULL && wavelet != NULL)
\t{
\t\t// Update the wavelet band flags
#if _DELAYED_THREAD_START==0'''
    source = replace_once(source, started_anchor,
                          started_anchor.replace(
                              '\tif (decoder != NULL && wavelet != NULL)\n\t{\n',
                              '\tif (decoder != NULL && wavelet != NULL)\n\t{\n' + STARTED_LOCALS + '\n\n'),
                          'started locals')
    started_statement = ('\t\twavelet->band_started_flags |= BAND_VALID_MASK(band);\n'
                         '\n#if _DELAYED_THREAD_START==0')
    source = replace_once(source, started_statement,
                          '\t\twavelet->band_started_flags |= BAND_VALID_MASK(band);\n'
                          '\t\trca160_started_new = wavelet->band_started_flags;\n'
                          '\t\trca160_started_ns = RCA160NowNs();\n'
                          '\t\trca160_started_tid = RCA160ThreadId();\n'
                          '\n#if _DELAYED_THREAD_START==0',
                          'started update')
    started_end = '''#endif
\t}
}

bool DecodedBandsValid'''
    source = replace_once(source, started_end,
                          '#endif\n\n' + STARTED_CALL + '\t}\n}\n\nbool DecodedBandsValid',
                          'started call')

    source = replace_once(source, ROUTE_ANCHOR, ROUTE_REPLACEMENT, 'route')

    shutil.copyfile(PROBE_HEADER, header_target)
    decoder.write_text(source, encoding='utf-8')
    print(f'decoder.c {sha256(decoder)}')
    print(f'rca160_probe.h {sha256(header_target)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
