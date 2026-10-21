#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Apply the RCA-195 test-only probe to a private out-of-repo SDK copy.

The frozen repository tree is never modified. Pass the root of a copy (the
directory containing `Codec/`). The script is deterministic and fails closed if
an anchor is missing or ambiguous.

Anchoring is *line based* on purpose. The frozen sources use CRLF endings and
carry "blank" lines that consist of a single tab, so multi-line string anchors
are brittle in exactly the way that matters here; matching stripped lines and
then locating a unique statement is stable against both.

What it installs:

  1. a *skip* hook in the worker drain. For a target `(channel, index)` the entry
     is consumed exactly as the real code consumes it -- `done = 1`,
     `num_entries--` -- but the reconstruction is never executed. This fabricates
     the "claimed but not executed" input the review asks about. It is failure
     injection, not a candidate fix.
  2. a queue-state record immediately before `WaitForTransformThread` discards
     the queue, so a drop becomes observable instead of inferred.

Both hooks are inert unless RCA195_PROBE_LOG / RCA195_SKIP are set.
"""
import argparse
import hashlib
import shutil
import sys
import tempfile
from pathlib import Path

PROBE_HEADER = Path(__file__).with_name('rca195_probe.h')

CLAIM_BLOCK = '''							/* RCA-195 test-only probe: record the claim so a claim that is
							   never followed by an execution is measurable. */
							RCA195LogClaim(channel, index);
'''

SKIP_BLOCK = '''\t\t\t\t\t\t/* RCA-195 test-only probe: the entry was already consumed above
\t\t\t\t\t\t   (done = 1, num_entries--). For the requested target we drop it
\t\t\t\t\t\t   here without executing the reconstruction: the "claimed but not
\t\t\t\t\t\t   executed" case under review. Failure injection, not a fix. */
\t\t\t\t\t\t/* Guarded on the admitted entry: channel/index are loop leftovers when
\t\t\t\t\t\t   no entry was found this pass, so an unguarded check fires again at
\t\t\t\t\t\t   loop exit (observed as 18 SKIP lines for 9 claims). */
\t\t\t\t\t\tif (wavelet != NULL && RCA195ShouldSkip(channel, index))
\t\t\t\t\t\t{
\t\t\t\t\t\t\tRCA195Log("SKIP ch=%d idx=%d entry_consumed=1 executed=0",
\t\t\t\t\t\t\t\t\t  channel, index);
\t\t\t\t\t\t\twavelet = NULL;
\t\t\t\t\t\t}
'''

EXEC_BLOCK = '''							/* RCA-195 test-only probe: the reconstruction is about to run. */
							RCA195LogExecute(channel, index);
'''

RESET_BLOCK = '''\t\t/* RCA-195 test-only probe: record what is about to be discarded.
\t\t   `admitted` is next_entry (one increment per successful claim in the drain
\t\t   loop); `unadmitted` is num_entries. Both are read here, not assumed: an
\t\t   earlier revision passed literal 0, 0 and published two constants as
\t\t   measurements. */
\t\tRCA195LogQueueReset(decoder->transform_queue.num_entries,
\t\t\t\t\t\t\tdecoder->transform_queue.free_entry,
\t\t\t\t\t\t\tdecoder->transform_queue.next_entry,
\t\t\t\t\t\t\tdecoder->transform_queue.next_entry,
\t\t\t\t\t\t\tdecoder->transform_queue.num_entries);
'''

INCLUDE_LINE = '#include "rca195_probe.h"'
OWNER_LINE = '#define RCA195_PROBE_OWNER 1'
INCLUDE_ANCHOR = '#include "timing.h"'


def read_lines(path):
    return path.read_text(encoding='utf-8', errors='replace').replace('\r\n', '\n').split('\n')


def write_lines(path, lines):
    path.write_text('\n'.join(lines), encoding='utf-8', newline='\n')


def find_unique_statement(lines, statement, start=0, end=None):
    """Index of the only stripped line equal to `statement` in [start, end)."""
    end = len(lines) if end is None else end
    hits = [i for i in range(start, end) if lines[i].strip() == statement]
    if len(hits) != 1:
        raise SystemExit('anchor %r is not unique (found %d)' % (statement, len(hits)))
    return hits[0]


def insert_include(lines, path_label):
    hits = [i for i, line in enumerate(lines) if line.strip() == INCLUDE_ANCHOR]
    if len(hits) != 1:
        raise SystemExit('%s: include anchor is not unique (found %d)'
                         % (path_label, len(hits)))
    if INCLUDE_LINE in [line.strip() for line in lines]:
        raise SystemExit('%s: probe include already present' % path_label)
    lines.insert(hits[0] + 1, INCLUDE_LINE)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def apply(root):
    root = Path(root)
    codec = root / 'Codec'
    threading_path = codec / 'entropy_threading.c'
    decoder_path = codec / 'decoder.c'
    for path in (threading_path, decoder_path):
        if not path.is_file():
            raise SystemExit('missing %s' % path)

    shutil.copyfile(PROBE_HEADER, codec / 'rca195_probe.h')

    # --- drain hook ------------------------------------------------------
    lines = read_lines(threading_path)
    claim = find_unique_statement(lines, 'data->queue[curr_entry].done = 1;')
    lines[claim + 1:claim + 1] = CLAIM_BLOCK.rstrip('\n').split('\n')

    gate = find_unique_statement(lines, 'if(wavelet && BANDS_ALL_VALID(wavelet))')
    lines[gate:gate] = SKIP_BLOCK.rstrip('\n').split('\n')

    execute = find_unique_statement(
        lines, 'ReconstructWaveletBand(decoder, transform, channel, wavelet, index, precision,')
    lines[execute:execute] = EXEC_BLOCK.rstrip('\n').split('\n')
    insert_include(lines, 'entropy_threading.c')
    write_lines(threading_path, lines)

    # --- queue-reset record ----------------------------------------------
    lines = read_lines(decoder_path)
    wait = find_unique_statement(
        lines, 'ThreadPoolWaitAllDone(&decoder->entropy_worker_new.pool);')
    reset = find_unique_statement(lines, 'decoder->transform_queue.started = 0;',
                                  start=wait)
    lines[reset:reset] = RESET_BLOCK.rstrip('\n').split('\n')
    insert_include(lines, 'decoder.c')
    hit = [i for i, line in enumerate(lines) if line.strip() == INCLUDE_LINE][0]
    lines.insert(hit, OWNER_LINE)
    write_lines(decoder_path, lines)

    return {
        'entropy_threading.c': sha256(threading_path),
        'decoder.c': sha256(decoder_path),
        'rca195_probe.h': sha256(codec / 'rca195_probe.h'),
    }


def self_test(repo):
    """Apply the probe to a scratch copy and check the anchors really land.

    The point is not that the patch applies -- it is that a *missing* anchor is
    rejected instead of silently producing a build without instrumentation.
    """
    repo = Path(repo).resolve()
    failures = []
    source = repo / 'upstream' / 'Codec'

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / 'copy'
        shutil.copytree(source, root / 'Codec')
        digests = apply(root)
        if len(digests) != 3:
            failures.append('expected three instrumented files, got %d' % len(digests))

        threading = (root / 'Codec' / 'entropy_threading.c').read_text(encoding='utf-8')
        decoder = (root / 'Codec' / 'decoder.c').read_text(encoding='utf-8')
        for needle, where in (('RCA195LogClaim(', 'claim hook'),
                              ('RCA195ShouldSkip(', 'skip hook'),
                              ('RCA195LogExecute(', 'execute hook')):
            if needle not in threading:
                failures.append('%s missing from entropy_threading.c' % where)
        for needle, where in (('RCA195LogQueueReset(', 'queue-reset record'),
                              ('RCA195_PROBE_OWNER', 'owning translation unit')):
            if needle not in decoder:
                failures.append('%s missing from decoder.c' % where)

        # The queue record must read the live counters. A literal passes the
        # presence check above while publishing a constant as a measurement.
        if 'RCA195LogQueueReset(num_entries' in decoder:
            failures.append('queue-reset record still passes a literal entry count')
        if 'decoder->transform_queue.next_entry,\n\t\t\t\t\t\t\tdecoder->transform_queue.next_entry,' not in decoder:
            failures.append('queue-reset record does not derive `admitted` from '
                            'next_entry')

        # The skip hook must sit *before* the gate it is meant to defeat, and the
        # claim must precede both: claim -> (unlock) -> skip -> gate -> execute.
        claim = threading.find('RCA195LogClaim(')
        skip = threading.find('RCA195ShouldSkip(')
        gate = threading.find('if(wavelet && BANDS_ALL_VALID(wavelet))')
        execute = threading.find('RCA195LogExecute(')
        if not (0 < claim < skip < gate < execute):
            failures.append('hook order is wrong (claim/skip/gate/execute at %d/%d/%d/%d)'
                            % (claim, skip, gate, execute))
        if 'wavelet != NULL && RCA195ShouldSkip' not in threading:
            failures.append('skip hook is not guarded on the admitted entry')

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / 'copy'
        shutil.copytree(source, root / 'Codec')
        threading_path = root / 'Codec' / 'entropy_threading.c'
        text = threading_path.read_text(encoding='utf-8', errors='replace')
        threading_path.write_text(
            text.replace('if(wavelet && BANDS_ALL_VALID(wavelet))',
                         'if(0 && wavelet && BANDS_ALL_VALID(wavelet))'),
            encoding='utf-8')
        try:
            apply(root)
        except SystemExit:
            pass
        else:
            failures.append('a removed anchor still applied successfully')

    if failures:
        for failure in failures:
            print('SELF-TEST FAIL %s' % failure, file=sys.stderr)
        return 1
    print('SELF-TEST PASS: 6 hook assertions, 1 live-counter check and 1 '
          'fail-closed anchor check')
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', nargs='?', help='private SDK copy root (contains Codec/)')
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--repo', default=str(Path(__file__).resolve().parents[1]),
                        help=argparse.SUPPRESS)
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test(args.repo)
    if not args.root:
        parser.error('root is required unless --self-test is given')

    for name, digest in apply(args.root).items():
        print('%s %s' % (digest, name))
    return 0


if __name__ == '__main__':
    sys.exit(main())
