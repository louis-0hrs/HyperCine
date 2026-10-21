#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Reduce a ThreadSanitizer log for the RCA-195 flag-pair question.

The Milestone-1 report derived, from source, that `band_valid_flags` and
`band_started_flags` are written under different lock discipline and read
without a lock. This tool checks a TSan log against that claim and prints only
sanitized facts: no absolute paths, no addresses, no thread ids.

It also verifies that the log was produced from the tree it is being compared
against. A sanitizer report is only evidence about *this* source if the frames
it names exist at those lines in this tree, so every cited `Codec/*.c:line` is
resolved and its source line is emitted.

Usage:
  python tools/analyze_rca195_tsan_log.py --log <path> [--json <path>]
  python tools/analyze_rca195_tsan_log.py --self-test
"""
import argparse
import collections
import hashlib
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

# `#0 Symbol /abs/path/upstream/Codec/file.c:123 (binary+0x..)`
FRAME = re.compile(r'#(\d+) (\S+) /\S*/upstream/Codec/([A-Za-z0-9_]+\.c):(\d+)')
ACCESS = re.compile(
    r'^  (?:Previous )?([Rr]ead|[Ww]rite) of size (\d+) at (0x[0-9a-f]+)'
    r' by thread (T\d+)( \(mutexes: [^)]*\))?:', re.M)

FLAG_WRITER = 'UpdateWaveletBandValidFlags'
FLAG_READER_SITE = ('decoder.c', 13039)
SHARED_INVERSE_SITE = ('decoder.c', 13107)


def sanitize(log_text):
    """Parse the log into race blocks. Returns sanitized structures only."""
    blocks = []
    for chunk in log_text.split('=================='):
        if 'WARNING: ThreadSanitizer' not in chunk:
            continue
        heads = list(ACCESS.finditer(chunk))
        if len(heads) < 2:
            continue

        accesses = []
        for index, head in enumerate(heads[:2]):
            end = heads[index + 1].start() if index + 1 < len(heads) else len(chunk)
            segment = chunk[head.end():end]
            frames = FRAME.findall(segment)
            accesses.append({
                'kind': head.group(1).capitalize(),
                'size': int(head.group(2)),
                'under_mutex': bool(head.group(5)),
                'top_frame': ('%s:%s' % (frames[0][2], frames[0][3])) if frames else None,
                'top_symbol': frames[0][1] if frames else None,
                'frames': ['%s:%s' % (f[2], f[3]) for f in frames],
                'has_flag_writer': any(f[1] == FLAG_WRITER for f in frames),
                'has_inverse_entry': any(
                    (f[2], int(f[3])) == SHARED_INVERSE_SITE for f in frames),
            })

        first, second = accesses
        blocks.append({
            'same_address': None,  # filled below; addresses never leave this scope
            'first': first,
            'second': second,
            '_same': heads[0].group(3) == heads[1].group(3),
        })

    for block in blocks:
        block['same_address'] = block.pop('_same')
    return blocks


def summarize(blocks):
    total = len(blocks)
    both_inverse = sum(1 for b in blocks
                       if b['first']['has_inverse_entry'] and b['second']['has_inverse_entry'])
    same_address = sum(1 for b in blocks if b['same_address'])

    flag_pairs = []
    for block in blocks:
        if not (block['first']['has_flag_writer'] or block['second']['has_flag_writer']):
            continue
        writer = block['first'] if block['first']['has_flag_writer'] else block['second']
        reader = block['second'] if block['first']['has_flag_writer'] else block['first']
        flag_pairs.append({
            'writer_symbol': writer['top_symbol'],
            'writer_under_mutex': writer['under_mutex'],
            'reader_site': reader['top_frame'],
            'reader_under_mutex': reader['under_mutex'],
            'same_address': block['same_address'],
        })

    by_pair = collections.Counter(
        (b['first']['top_frame'], b['second']['top_frame']) for b in blocks)

    return {
        'race_blocks': total,
        'same_address_blocks': same_address,
        'both_sides_in_inverse_reconstruct': both_inverse,
        'flag_writer_blocks': len(flag_pairs),
        'flag_writer_under_mutex': all(p['writer_under_mutex'] for p in flag_pairs) if flag_pairs else None,
        'flag_reader_under_mutex': any(p['reader_under_mutex'] for p in flag_pairs) if flag_pairs else None,
        'flag_reader_sites': sorted({p['reader_site'] for p in flag_pairs}),
        'distinct_site_pairs': len(by_pair),
        'site_pairs': [
            {'a': a, 'b': b, 'count': n} for (a, b), n in by_pair.most_common(12)
        ],
    }


def check_tree(repo, blocks):
    """Resolve every cited line in this tree; a mismatch invalidates the log."""
    cited = set()
    for block in blocks:
        for side in ('first', 'second'):
            for frame in block[side]['frames']:
                name, _, line = frame.partition(':')
                cited.add((name, int(line)))

    resolved = {}
    missing = []
    for name, line in sorted(cited):
        path = repo / 'upstream' / 'Codec' / name
        if not path.is_file():
            missing.append('%s (file absent)' % name)
            continue
        text = path.read_text(encoding='utf-8', errors='replace').replace('\r\n', '\n')
        lines = text.split('\n')
        if line < 1 or line > len(lines):
            missing.append('%s:%d (out of range)' % (name, line))
            continue
        resolved['%s:%d' % (name, line)] = lines[line - 1].strip()[:90]

    return {'missing': missing, 'resolved': resolved}


def file_identities(repo):
    out = {}
    for name in ('decoder.c', 'entropy_threading.c', 'spatial.c', 'wavelet.c'):
        path = repo / 'upstream' / 'Codec' / name
        if path.is_file():
            out[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    return out


def build_report(repo, log_path, options=None):
    options = options or {}
    log_path = Path(log_path)
    if not log_path.is_file():
        raise SystemExit('log not found: %s' % log_path.name)
    raw = log_path.read_text(encoding='utf-8', errors='replace')
    blocks = sanitize(raw)
    if not blocks and not options.get('allow_clean'):
        raise SystemExit('no ThreadSanitizer race blocks found in %s' % log_path.name)

    if not blocks:
        # A clean sanitiser run is a result, not a failure: it is the control
        # arm against which the racy arms are compared.
        return {
            'log_basename': log_path.name,
            'log_sha256': hashlib.sha256(raw.encode('utf-8', 'replace')).hexdigest(),
            'log_bytes': len(raw.encode('utf-8', 'replace')),
            'clean': True,
            'reduction': {
                'race_blocks': 0,
                'same_address_blocks': 0,
                'both_sides_in_inverse_reconstruct': 0,
                'flag_writer_blocks': 0,
                'flag_writer_under_mutex': None,
                'flag_reader_under_mutex': None,
                'flag_reader_sites': [],
                'distinct_site_pairs': 0,
                'site_pairs': [],
            },
            'cited_lines_resolved_against_this_tree': {},
            'cited_lines_missing': [],
            'tree_source_sha256': file_identities(repo),
        }

    tree = check_tree(repo, blocks)
    return {
        'log_basename': log_path.name,
        'log_sha256': hashlib.sha256(raw.encode('utf-8', 'replace')).hexdigest(),
        'log_bytes': len(raw.encode('utf-8', 'replace')),
        'clean': False,
        'reduction': summarize(blocks),
        'cited_lines_resolved_against_this_tree': tree['resolved'],
        'cited_lines_missing': tree['missing'],
        'tree_source_sha256': file_identities(repo),
    }


# ------------------------------------------------------------------ self-test

SYNTHETIC = """\
==================
WARNING: ThreadSanitizer: data race (pid=1)
  Write of size 4 at 0xdeadb0000001 by thread T1 (mutexes: write M0):
    #0 UpdateWaveletBandValidFlags /abs/upstream/Codec/decoder.c:24098 (bin+0x1)
    #1 DecodeEntropy /abs/upstream/Codec/entropy_threading.c:224 (bin+0x2)

  Previous read of size 4 at 0xdeadb0000001 by thread T2:
    #0 ReconstructWaveletBand /abs/upstream/Codec/decoder.c:13039 (bin+0x3)
    #1 DecodeEntropy /abs/upstream/Codec/entropy_threading.c:302 (bin+0x4)

SUMMARY: ThreadSanitizer: data race /abs/upstream/Codec/decoder.c:24098
==================
==================
WARNING: ThreadSanitizer: data race (pid=1)
  Read of size 2 at 0xdeadb0000002 by thread T1:
    #0 InvertSpatialQuantDescale16s /abs/upstream/Codec/spatial.c:22529 (bin+0x5)
    #1 TransformInverseSpatialQuantLowpass /abs/upstream/Codec/wavelet.c:5756 (bin+0x6)
    #2 ReconstructWaveletBand /abs/upstream/Codec/decoder.c:13107 (bin+0x7)

  Previous write of size 8 at 0xdeadb0000002 by thread T2:
    #0 _mm_store_si128 /usr/include/emmintrin.h:736 (bin+0x8)
    #1 InvertSpatialQuant16s /abs/upstream/Codec/spatial.c:22062 (bin+0x9)
    #2 TransformInverseSpatialQuantLowpass /abs/upstream/Codec/wavelet.c:5778 (bin+0xa)
    #3 ReconstructWaveletBand /abs/upstream/Codec/decoder.c:13107 (bin+0xb)

SUMMARY: ThreadSanitizer: data race /abs/upstream/Codec/spatial.c:22529
==================
"""


def self_test(repo):
    failures = []
    blocks = sanitize(SYNTHETIC)
    if len(blocks) != 2:
        failures.append('expected 2 blocks, got %d' % len(blocks))
    else:
        summary = summarize(blocks)
        checks = (
            ('same_address_blocks', summary['same_address_blocks'], 2),
            ('flag_writer_blocks', summary['flag_writer_blocks'], 1),
            ('both_sides_in_inverse_reconstruct',
             summary['both_sides_in_inverse_reconstruct'], 1),
            ('flag_reader_under_mutex', summary['flag_reader_under_mutex'], False),
        )
        for label, actual, expected in checks:
            if actual != expected:
                failures.append('%s: expected %r, got %r' % (label, expected, actual))
        if summary['flag_reader_sites'] != ['decoder.c:13039']:
            failures.append('reader site not identified: %r' % (summary['flag_reader_sites'],))

    # No absolute path or address may survive reduction.
    blob = json.dumps(summarize(blocks))
    for leak in ('/abs/', '0xdeadb', 'T1', 'T2'):
        if leak in blob:
            failures.append('reduction leaked %r' % leak)

    # A log whose cited lines do not exist in this tree must be detectable.
    tree = check_tree(repo, sanitize(SYNTHETIC.replace('decoder.c:24098',
                                                      'decoder.c:999999')))
    if not any('out of range' in item for item in tree['missing']):
        failures.append('out-of-range line was not detected')

    if failures:
        for failure in failures:
            print('SELF-TEST FAIL %s' % failure, file=sys.stderr)
        return 1
    print('SELF-TEST PASS: synthetic log reduced, 4 assertions and 2 leak checks')
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', help='ThreadSanitizer log to reduce')
    parser.add_argument('--json', help='write the sanitized reduction as JSON')
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--allow-clean', action='store_true',
                        help='accept a log with no races (control arm)')
    parser.add_argument('--repo', default=str(REPO), help=argparse.SUPPRESS)
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test(Path(args.repo).resolve())
    if not args.log:
        parser.error('--log is required unless --self-test is given')

    report = build_report(Path(args.repo).resolve(), args.log,
                          {'allow_clean': args.allow_clean})
    if report['cited_lines_missing']:
        print('VERIFY FAIL: log frames do not resolve against this tree: %r'
              % report['cited_lines_missing'], file=sys.stderr)
        return 2

    if args.json:
        path = Path(args.json)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(report, indent=1) + '\n', encoding='utf-8')
        print('wrote %s' % path)

    reduction = report['reduction']
    if report.get('clean'):
        print('race blocks: 0 (clean run)')
        print('VERIFY PASS')
        return 0
    print('race blocks: %d (%d same-address)'
          % (reduction['race_blocks'], reduction['same_address_blocks']))
    print('both sides inside the inverse reconstruct: %d'
          % reduction['both_sides_in_inverse_reconstruct'])
    print('flag-writer blocks: %d, writer under mutex: %s, reader under mutex: %s'
          % (reduction['flag_writer_blocks'],
             reduction['flag_writer_under_mutex'],
             reduction['flag_reader_under_mutex']))
    print('reader sites: %s' % ', '.join(reduction['flag_reader_sites']))
    print('VERIFY PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
