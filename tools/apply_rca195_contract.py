#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Apply the RCA-195 Milestone-2 scratch-SDK *contract candidate* to a private copy.

This is the one narrow experiment the fourth review gate allows: R1 read-set
admission, R3/R5 token re-check, R4 missing-input failure propagation and R6
drain verification, measured against a valid control at 2 and 4 workers.

It is NOT a fix and NOT a production patch. The frozen tree is never touched;
the script refuses to run against it.

What it installs, and why each hook exists
------------------------------------------
R3/R5  The queue lock is dropped between claim and execution. The candidate
       captures the wavelet pointer at claim time and re-checks it before
       executing; a mismatch is a stale token and fails closed. Pointer
       identity is the real token here because the decoder has no generation
       counter (`grep -rni generation Codec/*.c` -> two unrelated comments).
R4     The temporal branch guards its input with `assert` only, which Release
       compiles out. A temporal wavelet missing a band then reaches
       TransformInverseTemporalQuant, whose band guards return silently, and
       the caller still reports OKAY. The candidate makes that check real.
R6     WaitForTransformThread zeroes the queue without asking whether anything
       in it was never claimed or never executed. The candidate checks first
       and fails closed.

Anchoring is line based, on stripped text, because the frozen sources carry
CRLF and "blank" lines that consist of a single tab.
"""
import argparse
import hashlib
import shutil
import sys
import tempfile
from pathlib import Path

PROBE_HEADER = Path(__file__).with_name('rca195_contract.h')

CLAIM_BLOCK = '''							/* RCA-195 contract candidate: record the claim. A claim
							   that is never followed by an execution is the state R6
							   has to reject. */
							RCA195ContractClaim(channel, index);
'''

SKIP_BLOCK = '''						/* RCA-195 contract candidate: failure injection only.
						   Consume the admitted entry without executing it. This
						   fabricates the claimed-but-not-executed input; it is not a
						   fix and must never be enabled outside this experiment. */
						if (wavelet != NULL && RCA195ContractShouldSkip(channel, index))
						{
							RCA195ContractLog("SKIP ch=%d idx=%d executed=0", channel, index);
							wavelet = NULL;
						}
'''

STALE_BLOCK = '''							/* RCA-195 contract candidate (R3/R5): the queue lock is
							   released between claim and execution. If the wavelet was
							   reallocated in that window, the claimed pointer names an
							   object the decoder no longer owns. Fail closed rather
							   than publish onto it. The injected arm forces the same
							   condition on one target so this path is measured. */
							if (transform == NULL || transform->wavelet[index] != wavelet ||
								RCA195ContractForceStale(channel, index))
							{
								RCA195ContractStale(channel, index);
								if (decoder->error == CODEC_ERROR_OKAY)
									decoder->error = CODEC_ERROR_BAD_FRAME;
								break;
							}
'''

EXEC_BLOCK = '''							/* RCA-195 contract candidate: the reconstruction runs. */
							RCA195ContractExecute(channel, index);
'''

DRAIN_BLOCK = '''		/* RCA-195 contract candidate (R6): the queue is about to be zeroed.
		   Ask first whether anything in it was never claimed, or claimed and
		   never executed. A valid decode reaches this point with num_entries
		   zero and claims == executed; measured on FULL/HALF/QUARTER at 2 and
		   4 workers before this candidate was written. */
		RCA195ContractLog("DRAIN entries=%d claimed=%llu executed=%llu",
						  decoder->transform_queue.num_entries,
						  rca195_contract_claims, rca195_contract_executed);
		if (decoder->transform_queue.num_entries != 0 ||
			rca195_contract_claims != rca195_contract_executed)
		{
			RCA195ContractDrainShort(decoder->transform_queue.num_entries,
									 rca195_contract_claims,
									 rca195_contract_executed);
			RCA195_CONTRACT_KEEP_ERROR(decoder->error, CODEC_ERROR_BAD_FRAME);
		}
'''

# R4: replace the assert-only temporal guard with a real one.
TEMPORAL_ASSERT = 'assert(BANDS_ALL_VALID(temporal));'
TEMPORAL_REPLACEMENT = '''/* RCA-195 contract candidate (R4): the frozen code guards this with assert
			   only, which Release compiles out. A temporal wavelet missing a band
			   then reaches TransformInverseTemporalQuant, whose band guards return
			   silently, and the caller still reports OKAY. */
			if (!BANDS_ALL_VALID(temporal))
			{
				RCA195_CONTRACT_KEEP_ERROR(decoder->error, CODEC_ERROR_BAD_FRAME);
				return;
			}'''

# R6b: the drain's error must not be erased by a later local assignment,
# and a second failure must not displace the first.
# ReconstructSampleFrameToBuffer starts `CODEC_ERROR error = CODEC_ERROR_OKAY;`,
# and its final `decoder->error = error;` writes that local value over whatever
# the drain just recorded. Measured: DRAIN_SHORT fires, the next CHECK still
# reads err=0. The first fix refused to replace a real error with OKAY, which a
# reviewer correctly pointed out is *not* first-error semantics -- with two
# non-OKAY values the later one still won. The macro keeps OKAY out and keeps
# the first non-OKAY code sticky, logging any precedence conflict.
CLOBBER_BLOCK = '''\t\t// RCA-195 contract candidate (R6b): first-error precedence.
\t\t// This routine keeps its own local `error` and writes it here. Without the
\t\t// guard it erases what the drain recorded; with a plain "!= OKAY" test the
\t\t// later of two failures would still displace the earlier one.
\t\tRCA195_CONTRACT_KEEP_ERROR(decoder->error, error);'''

# Per-call terminator: the counters the runner cross-checks against the log
# lines. Placed at the single return of DecodeSample, the public path's exit.
RESULT_HOOK = '''\t/* RCA-195 contract candidate: per-call summary. The runner compares these\n\t   counters against the tallied log lines; a disagreement means a line was\n\t   lost and the call is an instrument failure, not a measurement. */\n\tRCA195ContractResult((int)decoder->error, (int)!result);'''

INCLUDE_LINE = '#include "rca195_contract.h"'
OWNER_LINE = '#define RCA195_PROBE_OWNER 1'
INCLUDE_ANCHOR = '#include "timing.h"'

# ABA probe: record the free and the allocation so address reuse is counted.
# Two extra hooks make the probe's own reachability measurable: a zero counter is
# evidence only if the path was entered at all, and a path that early-returns on
# every call is not the same finding as a path that was never reached.
FREE_HOOK = '''\t\t\tRCA195ContractWaveletFree(wavelet);'''
ALLOC_HOOK = '''\tRCA195ContractWaveletAlloc(wavelet);'''
REALLOC_ENTER_HOOK = '''\tRCA195ContractReallocEnter((void *)wavelet);'''
REALLOC_EARLY_HOOK = '''\t\t\tRCA195ContractReallocEarly((void *)wavelet);'''


def read_lines(path):
    return path.read_text(encoding='utf-8', errors='replace').replace('\r\n', '\n').split('\n')


def write_lines(path, lines):
    path.write_text('\n'.join(lines), encoding='utf-8', newline='\n')


def find_unique_statement(lines, statement, start=0, end=None):
    end = len(lines) if end is None else end
    hits = [i for i in range(start, end) if lines[i].strip() == statement]
    if len(hits) != 1:
        raise SystemExit('anchor %r is not unique (found %d)' % (statement, len(hits)))
    return hits[0]


def insert_include(lines, label, anchor=INCLUDE_ANCHOR):
    """Insert the contract include after a unique anchor line.

    The anchor is a parameter because wavelet.c includes "timing.h" twice (once
    at the top, once in a second block), so the default would be ambiguous
    there. The self-test caught that; guessing a different anchor silently would
    have put the include in the wrong block.
    """
    hits = [i for i, line in enumerate(lines) if line.strip() == anchor]
    if len(hits) != 1:
        raise SystemExit('%s: include anchor %r is not unique (found %d)'
                         % (label, anchor, len(hits)))
    if INCLUDE_LINE in [line.strip() for line in lines]:
        raise SystemExit('%s: contract include already present' % label)
    lines.insert(hits[0] + 1, INCLUDE_LINE)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def find_endif_after(lines, start, limit=12):
    """Index of the `#endif` that closes the conditional opened at/near `start`.

    The free/alloc sites sit inside `#if _ALLOCATOR / #else / #endif`. Hooks
    inserted next to the concrete statement land in ONE branch, and the build
    defines `-D_ALLOCATOR=1`, so a hook placed in the `#else` branch is compiled
    away. That happened: the first ABA run logged 3950 realloc entries and zero
    free/alloc lines, which reads as "no reuse" but was dead instrumentation.
    Placing the hook after the `#endif` makes it configuration-independent.
    """
    for i in range(start, min(start + limit, len(lines))):
        if lines[i].strip() == '#endif':
            return i
    raise SystemExit('closing #endif not found within %d lines of %d'
                     % (limit, start + 1))


def guard_frozen(root):
    """Refuse to patch a tree that looks like the frozen upstream itself."""
    root = Path(root).resolve()
    parts = set(root.parts)
    if root.name == 'upstream' or ('upstream' in parts and not (root / 'Codec').is_dir()):
        raise SystemExit('refusing to patch %s: that is the frozen tree' % root)
    if (root / '.git').exists() and (root / 'upstream').is_dir():
        raise SystemExit('refusing to patch %s: that is the repository root' % root)


def apply(root):
    root = Path(root)
    codec = root / 'Codec'
    threading_path = codec / 'entropy_threading.c'
    decoder_path = codec / 'decoder.c'
    for path in (threading_path, decoder_path):
        if not path.is_file():
            raise SystemExit('missing %s' % path)

    shutil.copyfile(PROBE_HEADER, codec / 'rca195_contract.h')

    # --- worker: claim token, skip injection, stale re-check, execute --------
    lines = read_lines(threading_path)
    claim = find_unique_statement(lines, 'data->queue[curr_entry].done = 1;')
    lines[claim + 1:claim + 1] = CLAIM_BLOCK.rstrip('\n').split('\n')

    gate = find_unique_statement(lines, 'if(wavelet && BANDS_ALL_VALID(wavelet))')
    lines[gate:gate] = SKIP_BLOCK.rstrip('\n').split('\n')

    # Re-locate the gate after the insertion, then insert just inside its brace.
    gate = find_unique_statement(lines, 'if(wavelet && BANDS_ALL_VALID(wavelet))')
    brace = gate + 1
    if lines[brace].strip() != '{':
        raise SystemExit('gate brace not where expected (found %r)' % lines[brace])
    lines[brace + 1:brace + 1] = STALE_BLOCK.rstrip('\n').split('\n')

    execute = find_unique_statement(
        lines, 'ReconstructWaveletBand(decoder, transform, channel, wavelet, index, precision,')
    lines[execute:execute] = EXEC_BLOCK.rstrip('\n').split('\n')
    insert_include(lines, 'entropy_threading.c')
    write_lines(threading_path, lines)

    # --- decoder: R4 real guard, R6 drain verification ----------------------
    lines = read_lines(decoder_path)
    if TEMPORAL_ASSERT not in [line.strip() for line in lines]:
        raise SystemExit('temporal assert anchor missing')
    idx = find_unique_statement(lines, TEMPORAL_ASSERT)
    lines[idx:idx + 1] = TEMPORAL_REPLACEMENT.split('\n')

    wait = find_unique_statement(
        lines, 'ThreadPoolWaitAllDone(&decoder->entropy_worker_new.pool);')
    reset = find_unique_statement(lines, 'decoder->transform_queue.started = 0;',
                                  start=wait)
    lines[reset:reset] = DRAIN_BLOCK.rstrip('\n').split('\n')
    insert_include(lines, 'decoder.c')
    hit = [i for i, line in enumerate(lines) if line.strip() == INCLUDE_LINE][0]
    lines.insert(hit, OWNER_LINE)
    write_lines(decoder_path, lines)

    # R6b: the drain's error must not be erased by a later local assignment.
    lines = read_lines(decoder_path)
    idx = None
    for i, line in enumerate(lines):
        if line.strip() == '// Save the codec error code in the decoder state and return':
            idx = i
            break
    if idx is None:
        raise SystemExit('clobber-site anchor missing')
    if lines[idx + 1].strip() != 'decoder->error = error;':
        raise SystemExit('clobber-site assignment not where expected')
    lines[idx + 1:idx + 2] = CLOBBER_BLOCK.rstrip('\n').split('\n')

    # RESULT summary at the one return of DecodeSample.
    ret_comment = find_unique_statement(
        lines, '// Return indication of whether decoding succeeded or failed')
    lines[ret_comment:ret_comment] = RESULT_HOOK.rstrip('\n').split('\n')
    write_lines(decoder_path, lines)

    # --- wavelet: ABA probe (free/alloc address reuse) ----------------------
    # The reviewer required that the pointer-identity limitation be made
    # explicit rather than silently promoted to a generation counter. Recording
    # every wavelet free and allocation turns "address reuse could happen" into
    # a counted observation.
    wavelet_path = codec / 'wavelet.c'
    if not wavelet_path.is_file():
        raise SystemExit('missing %s' % wavelet_path)
    lines = read_lines(wavelet_path)
    free_anchor = find_unique_statement(
        lines, '// Need to free this wavelet and create another')
    free_at = None
    for i in range(free_anchor, free_anchor + 8):
        if lines[i].strip() == 'DeleteImage(wavelet);':
            free_at = i
            break
    if free_at is None:
        raise SystemExit('wavelet free site not found after its anchor')
    free_end = find_endif_after(lines, free_at)
    lines[free_end + 1:free_end + 1] = FREE_HOOK.rstrip('\n').split('\n')

    alloc = find_unique_statement(
        lines, 'wavelet = CreateWaveletEx(width, height, level, type);')
    alloc_end = find_endif_after(lines, alloc)
    lines[alloc_end + 1:alloc_end + 1] = ALLOC_HOOK.rstrip('\n').split('\n')

    # Reachability: is ReallocWaveletEx entered at all, and does it always take
    # the "same wavelet" early return? Without these two, a zero reuse count is
    # indistinguishable from an unreachable probe.
    re_sig = find_unique_statement(
        lines, 'IMAGE *ReallocWaveletEx(IMAGE *wavelet, int width, int height, int level, int type)')
    re_brace = None
    for i in range(re_sig, re_sig + 5):
        if lines[i].strip() == '{':
            re_brace = i
            break
    if re_brace is None:
        raise SystemExit('ReallocWaveletEx body brace not found')
    lines[re_brace + 1:re_brace + 1] = REALLOC_ENTER_HOOK.rstrip('\n').split('\n')

    # The body has TWO `return wavelet;` lines -- the early "same wavelet" return
    # and the final one after allocation. The probe wants the early one, so the
    # search is bounded by the "Need to free this wavelet" comment; without that
    # bound the anchor is ambiguous and the applier refuses (which is correct).
    free_comment = find_unique_statement(
        lines, '// Need to free this wavelet and create another')
    early = find_unique_statement(lines, 'return wavelet;', start=re_brace,
                                  end=free_comment)
    lines[early:early] = REALLOC_EARLY_HOOK.rstrip('\n').split('\n')
    insert_include(lines, 'wavelet.c', anchor='#include "config.h"')
    write_lines(wavelet_path, lines)

    return {
        'entropy_threading.c': sha256(threading_path),
        'decoder.c': sha256(decoder_path),
        'wavelet.c': sha256(wavelet_path),
        'rca195_contract.h': sha256(codec / 'rca195_contract.h'),
    }


def self_test(repo):
    """A missing anchor must be rejected, not silently skipped."""
    repo = Path(repo).resolve()
    failures = []
    source = repo / 'upstream' / 'Codec'

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / 'copy'
        shutil.copytree(source, root / 'Codec')
        digests = apply(root)
        if len(digests) != 4:
            failures.append('expected four instrumented files, got %d' % len(digests))

        threading = (root / 'Codec' / 'entropy_threading.c').read_text(encoding='utf-8')
        decoder = (root / 'Codec' / 'decoder.c').read_text(encoding='utf-8')
        wavelet = (root / 'Codec' / 'wavelet.c').read_text(encoding='utf-8')
        for needle, where in (('RCA195ContractClaim(', 'claim hook'),
                              ('RCA195ContractShouldSkip(', 'skip hook'),
                              ('RCA195ContractStale(', 'stale re-check'),
                              ('RCA195ContractForceStale(', 'stale injection'),
                              ('RCA195ContractExecute(', 'execute hook')):
            if needle not in threading:
                failures.append('%s missing from entropy_threading.c' % where)
        for needle, where in (('RCA195ContractDrainShort(', 'drain check'),
                              ('RCA195_PROBE_OWNER', 'owning translation unit'),
                              ('if (!BANDS_ALL_VALID(temporal))', 'R4 temporal guard'),
                              ('RCA195_CONTRACT_KEEP_ERROR(decoder->error, error)',
                               'R6b first-error precedence'),
                              ('RCA195ContractResult((int)decoder->error',
                               'per-call summary')):
            if needle not in decoder:
                failures.append('%s missing from decoder.c' % where)

        if TEMPORAL_ASSERT in [line.strip() for line in decoder.split('\n')]:
            failures.append('R4 still guards the temporal input with assert only')

        # ABA probe must sit on both sides of the free/alloc pair, or the reuse
        # count is a constant zero that looks like a clean measurement.
        for needle, where in (('RCA195ContractWaveletFree(wavelet);', 'wavelet free'),
                              ('RCA195ContractWaveletAlloc(wavelet);', 'wavelet alloc'),
                              ('RCA195ContractReallocEnter(', 'realloc reachability'),
                              ('RCA195ContractReallocEarly(', 'realloc early-return')):
            if needle not in wavelet:
                failures.append('%s hook missing from wavelet.c' % where)
        free_at = wavelet.find('RCA195ContractWaveletFree(wavelet);')
        alloc_at = wavelet.find('RCA195ContractWaveletAlloc(wavelet);')
        if not (0 <= free_at < alloc_at):
            failures.append('wavelet free hook does not precede the allocation hook '
                            '(free at %d, alloc at %d)' % (free_at, alloc_at))
        # Order: claim -> skip -> stale -> execute.
        claim = threading.find('RCA195ContractClaim(')
        skip = threading.find('RCA195ContractShouldSkip(')
        stale = threading.find('RCA195ContractStale(')
        execute = threading.find('RCA195ContractExecute(')
        if not (0 < claim < skip < stale < execute):
            failures.append('hook order wrong (claim/skip/stale/execute at %d/%d/%d/%d)'
                            % (claim, skip, stale, execute))

        # The drain check must read the live counters, not a literal.
        if 'RCA195ContractDrainShort(decoder->transform_queue.num_entries' not in decoder:
            failures.append('drain check does not pass the live entry count')
        if 'rca195_contract_claims != rca195_contract_executed' not in decoder:
            failures.append('drain check does not compare claimed against executed')

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / 'copy'
        shutil.copytree(source, root / 'Codec')
        path = root / 'Codec' / 'entropy_threading.c'
        text = path.read_text(encoding='utf-8', errors='replace')
        path.write_text(text.replace('if(wavelet && BANDS_ALL_VALID(wavelet))',
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
    print('SELF-TEST PASS: 14 hook assertions (claim/skip/stale/force/execute, '
          'drain/owner/R4/R6b/result, wavelet free/alloc, realloc enter/early), '
          'live-counter drain check, free-before-alloc order and 1 '
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
        parser.error('a private SDK copy root is required (or --self-test)')
    guard_frozen(args.root)
    digests = apply(args.root)
    for name in sorted(digests):
        print('%s %s' % (name, digests[name]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
