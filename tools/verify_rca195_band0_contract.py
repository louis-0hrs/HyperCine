#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Re-derive the RCA-195 band-0 publication contract from the frozen SDK source.

Issue #195 Milestone 1 asks for a source-anchored dependency map, a per-topology
required-bit matrix, and a liveness / memory-ordering statement -- and explicitly
forbids a mechanical "replace `valid == started` with 0xF everywhere".

This verifier does not trust the written report. It parses the frozen `upstream/`
tree, extracts the macro forms, the writer/reader sites and the per-type band
counts, re-evaluates the upstream per-topology mask logic in Python, and fails
closed if any anchor disappears or becomes ambiguous.

Nothing here is a decoder fix: the repository tree is only read.

Usage:
  python tools/verify_rca195_band0_contract.py --check
  python tools/verify_rca195_band0_contract.py --json docs/validation/<file>.json
  python tools/verify_rca195_band0_contract.py --self-test
"""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DECODER = Path('upstream/Codec/decoder.c')
IMAGE_H = Path('upstream/Codec/image.h')
WAVELET_C = Path('upstream/Codec/wavelet.c')
WAVELET_H = Path('upstream/Codec/wavelet.h')
THREADING = Path('upstream/Codec/entropy_threading.c')
CONFIG_H = Path('upstream/Codec/config.h')

MACRO_STRICT_COMMENTED = (
    '//#define BANDS_ALL_VALID(wavelet)'
    '\t((wavelet)->band_valid_flags == (uint32_t)((1 << (wavelet)->num_bands) - 1))'
)
MACRO_RELEASED = (
    '#define BANDS_ALL_VALID(wavelet)'
    '\t((wavelet)->band_valid_flags == (wavelet)->band_started_flags)'
)

CONSUMER_ANCHOR = (
    '\t\t\t// Check that all of the wavelet bands have been decoded\n'
    '\t\t\t//assert(BANDS_ALL_VALID(wavelet));\n'
    '\t\t\tif (!BANDS_ALL_VALID(wavelet))'
)

# Expected number of live `if (!BANDS_ALL_VALID(wavelet))` guards inside
# ReconstructWaveletBand. The temporal branch (index == 2) keeps an
# assert-only check instead, which is counted separately.
EXPECTED_SPATIAL_GUARDS = 3

# numWaveletBands[] as published by the frozen source, in declaration order.
# Twelve entries: image/horizontal/vertical/spatial, temporal/horztemp/verttemp/
# unimplemented, tempquad/horzquad/unimplemented/unimplemented. Only indices
# 0..9 resolve to a WAVELET_TYPE_* macro; the last two entries are inert.
EXPECTED_BAND_COUNTS = [1, 2, 2, 4, 2, 4, 4, 0, 4, 4, 0, 0]


class VerifyError(Exception):
    pass


def read(repo, rel):
    path = repo / rel
    if not path.is_file():
        raise VerifyError('missing frozen source file: %s' % rel)
    # The frozen SDK tree is stored with CRLF endings; normalise before any
    # anchor search so that block matching does not depend on the checkout's
    # autocrlf settings.
    return path.read_text(encoding='utf-8', errors='replace').replace('\r\n', '\n')


def sha256_file(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def line_of(text, needle):
    index = text.find(needle)
    if index < 0:
        return None
    return text.count('\n', 0, index) + 1


def require(condition, message):
    if not condition:
        raise VerifyError(message)


# ---------------------------------------------------------------- macro forms

def parse_macros(repo):
    text = read(repo, IMAGE_H)
    require(MACRO_RELEASED in text,
            'released BANDS_ALL_VALID form is not the equality of both flag words')
    require(MACRO_STRICT_COMMENTED in text,
            'the strict all-bands form is no longer present even as a comment')

    released_line = line_of(text, MACRO_RELEASED)
    strict_line = line_of(text, MACRO_STRICT_COMMENTED)
    require(released_line is not None and strict_line is not None,
            'macro line numbers could not be resolved')
    require(strict_line < released_line,
            'strict form is not the predecessor of the released form')

    # The released predicate is satisfied by a band that is neither started nor
    # valid, which is the whole defect: 0xE == 0xE passes with band 0 absent.
    require('band_started_flags' in MACRO_RELEASED,
            'released predicate does not reference the started word')
    require('==' in MACRO_RELEASED and '&' not in MACRO_RELEASED.split('==', 1)[1],
            'released predicate is no longer a plain equality')

    return {
        'file': str(IMAGE_H),
        'released_line': released_line,
        'strict_form_present_as_comment_line': strict_line,
        'released_form': MACRO_RELEASED.split('\t', 1)[1],
        'strict_form': MACRO_STRICT_COMMENTED.split('\t', 1)[1],
        'vacuous_state_4_band': '0x0000000E',
        'vacuous_state_2_band': '0x00000002',
    }


# ------------------------------------------------------------- band-count table

def parse_band_counts(repo):
    text = read(repo, WAVELET_C)
    match = re.search(r'static const int numWaveletBands\[\]\s*=\s*\{(.*?)\};',
                      text, re.S)
    require(match is not None, 'numWaveletBands[] table not found')
    body = re.sub(r'//[^\n]*', '', match.group(1))
    values = [int(v) for v in re.findall(r'-?\d+', body)]
    require(len(values) == len(EXPECTED_BAND_COUNTS),
            'numWaveletBands[] no longer has %d entries (got %d)'
            % (len(EXPECTED_BAND_COUNTS), len(values)))
    require(values == EXPECTED_BAND_COUNTS,
            'numWaveletBands[] changed: %r' % (values,))

    header = read(repo, WAVELET_H)
    expressions = {}
    for name in ('IMAGE', 'HORIZONTAL', 'VERTICAL', 'TEMPORAL', 'SPATIAL',
                 'HORZTEMP', 'VERTTEMP', 'FRAME'):
        pattern = r'#define WAVELET_TYPE_%s\b\s*(.+)$' % name
        found = re.search(pattern, header, re.M)
        require(found is not None, 'WAVELET_TYPE_%s is missing' % name)
        expressions[name] = found.group(1).split('//')[0].strip()

    resolved = {}

    def resolve(name, seen=()):
        if name in resolved:
            return resolved[name]
        require(name not in seen, 'WAVELET_TYPE_%s is self-referential' % name)
        expression = expressions[name]
        if expression.isdigit():
            value = int(expression)
        else:
            value = 0
            tokens = re.findall(r'WAVELET_TYPE_([A-Z]+)', expression)
            require(tokens, 'WAVELET_TYPE_%s has no resolvable value' % name)
            for token in tokens:
                value |= resolve(token, seen + (name,))
        resolved[name] = value
        return value

    for name in expressions:
        require(0 <= resolve(name) < 10,
                'WAVELET_TYPE_%s resolves out of range' % name)

    require(resolved['SPATIAL'] == 3, 'WAVELET_TYPE_SPATIAL is no longer 3')
    require(resolved['TEMPORAL'] == 4, 'WAVELET_TYPE_TEMPORAL is no longer 4')
    require(resolved['FRAME'] == 5, 'WAVELET_TYPE_FRAME is no longer 5')
    require(values[resolved['SPATIAL']] == 4, 'spatial wavelet no longer has 4 bands')
    require(values[resolved['TEMPORAL']] == 2, 'temporal wavelet no longer has 2 bands')
    require(values[resolved['FRAME']] == 4, 'frame wavelet no longer has 4 bands')

    return {
        'table_file': str(WAVELET_C),
        'values': values,
        'resolved_types': resolved,
        'bands': {
            'spatial': values[resolved['SPATIAL']],
            'temporal': values[resolved['TEMPORAL']],
            'frame': values[resolved['FRAME']],
        },
    }


# -------------------------------------------------------------------- writers

def parse_writers(repo):
    text = read(repo, DECODER)
    config = read(repo, CONFIG_H)

    require(re.search(r'#define _DELAYED_THREAD_START\s+1', config) is not None,
            '_DELAYED_THREAD_START is no longer 1; writer lock guards changed')

    valid_start = text.find('void UpdateWaveletBandValidFlags(')
    started_start = text.find('void UpdateWaveletBandStartedFlags(')
    require(valid_start >= 0, 'UpdateWaveletBandValidFlags missing')
    require(started_start >= 0, 'UpdateWaveletBandStartedFlags missing')
    require(valid_start < started_start, 'writer order changed unexpectedly')

    valid_body = text[valid_start:started_start]
    started_end = text.find('\n}\n', started_start)
    started_body = text[started_start:started_end]

    require('#if _THREADED_DECODER' in valid_body,
            'valid writer no longer guards its lock with _THREADED_DECODER')
    require('Lock(&decoder->entropy_worker_new.lock);' in valid_body,
            'valid writer no longer takes the entropy worker lock')
    require('pool.thread_count' in valid_body,
            'valid writer no longer conditions the lock on a live worker pool')

    # The started writer's lock is compiled out in the shipped configuration:
    # both its lock and unlock guards use `_DELAYED_THREAD_START==0`, while
    # config.h sets that macro to 1.
    require(started_body.count('#if _DELAYED_THREAD_START==0') == 2,
            'started writer lock guard changed; re-derive the ordering claim')
    require('Lock(&decoder->entropy_worker_new.lock);' in started_body,
            'started writer lock block vanished')
    require('Unlock(&decoder->entropy_worker_new.lock);' in started_body,
            'started writer unlock block vanished')

    require('wavelet->band_valid_flags |= BAND_VALID_MASK(band);' in valid_body,
            'valid writer no longer ORs the valid bit')
    require('wavelet->band_started_flags |= BAND_VALID_MASK(band);' in valid_body,
            'valid writer no longer ORs the started bit')
    require('wavelet->band_started_flags |= BAND_VALID_MASK(band);' in started_body,
            'started writer no longer ORs the started bit')

    return {
        'valid_writer': {
            'symbol': 'UpdateWaveletBandValidFlags',
            'line': line_of(text, 'void UpdateWaveletBandValidFlags('),
            'sets': ['band_valid_flags|bit', 'band_started_flags|bit'],
            'lock': 'entropy_worker_new.lock, guarded by _THREADED_DECODER and a live pool',
            'lock_compiled_in_default_config': True,
        },
        'started_writer': {
            'symbol': 'UpdateWaveletBandStartedFlags',
            'line': line_of(text, 'void UpdateWaveletBandStartedFlags('),
            'sets': ['band_started_flags|bit'],
            'lock': 'entropy_worker_new.lock guarded by _DELAYED_THREAD_START==0',
            'lock_compiled_in_default_config': False,
        },
        'ordering': {
            'claim': 'valid and started are written by different code paths under '
                     'different lock discipline in the shipped configuration',
            'why': 'config.h sets _DELAYED_THREAD_START to 1, so the started writer '
                   'does not take entropy_worker_new.lock, while the valid writer does',
            'consequence': 'a reader comparing the two words under no lock cannot '
                           'rely on a single consistent snapshot of the pair',
        },
    }


# -------------------------------------------------------------------- readers

def parse_readers(repo):
    decoder = read(repo, DECODER)
    threading = read(repo, THREADING)

    count = decoder.count(CONSUMER_ANCHOR)
    require(count == EXPECTED_SPATIAL_GUARDS,
            'expected %d live band-valid guards in the spatial/inverse branches, '
            'found %d' % (EXPECTED_SPATIAL_GUARDS, count))

    guards = []
    for match in re.finditer(re.escape(CONSUMER_ANCHOR), decoder):
        line = decoder.count('\n', 0, match.start()) + 1
        guards.append(line)

    # The temporal branch keeps only an assert, which NDEBUG removes.
    temporal_assert = '\t\t\tassert(BANDS_ALL_VALID(temporal));'
    require(decoder.count(temporal_assert) == 1,
            'the assert-only temporal readiness check is missing or duplicated')
    temporal_line = line_of(decoder, temporal_assert)

    # The queue drain gates, outside the lock.
    drain_guards = []
    for pattern in (r'if\(data->queue\[curr_entry\]\.done == 0 && BANDS_ALL_VALID\(wavelet\)\)',
                    r'if\(wavelet && BANDS_ALL_VALID\(wavelet\)\)',
                    r'\} while\(wavelet && BANDS_ALL_VALID\(wavelet\)\);'):
        found = re.search(pattern, threading)
        require(found is not None, 'queue drain guard missing: %s' % pattern)
        drain_guards.append(threading.count('\n', 0, found.start()) + 1)
    require(len(drain_guards) == 3, 'expected three queue drain guards')

    admission_under_lock = None
    drain_loop_start = None
    for match in re.finditer(r'if\(data->queue\[curr_entry\]\.done == 0 && BANDS_ALL_VALID\(wavelet\)\)', threading):
        admission_under_lock = threading.count('\n', 0, match.start()) + 1
        drain_loop_start = match.start()
        break
    require(admission_under_lock is not None, 'queue admission guard is missing')

    lock_before = threading.rfind('Lock(&decoder->entropy_worker_new.lock);',
                                  0, drain_loop_start)
    unlock_after = threading.find('Unlock(&decoder->entropy_worker_new.lock);',
                                  drain_loop_start)
    require(lock_before >= 0 and unlock_after > drain_loop_start,
            'queue admission is no longer evaluated between Lock and Unlock')

    recheck = threading.find('if(wavelet && BANDS_ALL_VALID(wavelet))', unlock_after)
    require(recheck >= 0,
            'the unconditional post-admission re-check is missing')
    execute = threading.find('ReconstructWaveletBand(decoder, transform, channel, wavelet',
                             recheck)
    require(execute > recheck,
            'the transform invoke is no longer after the unlocked re-check')

    return {
        'inverse_branch_guards': {
            'file': str(DECODER),
            'lines': guards,
            'count': count,
            'note': 'spatial lowpass (index > 0), field-plus lowpass (index > 3) '
                    'and spatial highpass (index == 3)',
        },
        'temporal_branch': {
            'file': str(DECODER),
            'line': temporal_line,
            'form': 'assert only',
            'survives_release_build': False,
            'reason': 'NDEBUG removes the assert, so the temporal inverse runs with '
                      'no runtime readiness check at all',
        },
        'queue_drain': {
            'file': str(THREADING),
            'lines': drain_guards,
            'admission_line': admission_under_lock,
            'admission_under_lock': True,
            'recheck_line': threading.count('\n', 0, recheck) + 1,
            'recheck_under_lock': False,
            'execute_line': threading.count('\n', 0, execute) + 1,
            'note': 'the admission decision is taken under the entropy worker lock, '
                    'but the transform itself runs after Unlock() and is re-gated by '
                    'an unlocked, non-atomic re-read of both flag words; the do/while '
                    'condition at the loop tail repeats that unlocked read',
        },
    }


# ------------------------------------------------- required-bit matrix (truth)

def evaluate_mask_logic(transform_type, index, num_bands):
    """Re-evaluate the upstream `DecodedBandsValid` mask logic.

    Returns the bitmask of bands the consumer must wait for, where bit 0 is the
    lowpass. A zero mask means "every band of this wavelet is required".
    """
    if transform_type == 'FIELDPLUS':
        if index == 2:
            return None  # earlier transforms in the queue compute both bands
        if index in (3, 5):
            threaded = 0
        else:
            threaded = 1
    elif transform_type == 'SPATIAL':
        if index == 2:
            threaded = 0
        else:
            threaded = 1
    else:
        raise VerifyError('unknown transform type: %s' % transform_type)

    decoded = (1 << num_bands) - 1
    decoded &= ~threaded
    return decoded


def parse_mask_logic(repo):
    text = read(repo, DECODER)
    start = text.find('bool DecodedBandsValid(')
    require(start >= 0, 'DecodedBandsValid is missing')
    end = text.find('\n}\n', start)
    body = text[start:end]

    # The function exists but every call site is commented out: the correct
    # per-topology matrix is upstream-supplied dead code.
    live_calls = []
    for match in re.finditer(r'DecodedBandsValid\(', text):
        line_start = text.rfind('\n', 0, match.start()) + 1
        line = text[line_start:text.find('\n', match.start())]
        stripped = line.strip()
        if 'bool DecodedBandsValid(' in stripped:
            continue  # the definition itself
        if stripped.startswith('//') or stripped.startswith('*') or stripped.startswith('/*'):
            continue
        live_calls.append(stripped)
    require(live_calls == [],
            'DecodedBandsValid gained a live call site; re-derive this report')
    commented = text.count('//if (DecodedBandsValid(')
    require(commented >= 2,
            'expected the two commented-out DecodedBandsValid call sites')

    require('threaded_band_mask = 0;' in body,
            'DecodedBandsValid no longer uses a zero required mask')
    require('threaded_band_mask = BAND_VALID_MASK(0);' in body,
            'DecodedBandsValid no longer exempts the computed lowpass')
    require('decoded_band_mask = ((1 << wavelet->num_bands) - 1);' in body,
            'DecodedBandsValid no longer derives the mask from num_bands')

    require("assert(wavelet->num_bands == 2);" in body,
            'the FIELDPLUS index==2 assertion changed')

    bands = {'spatial': 4, 'temporal': 2, 'frame': 4}
    matrix = []
    for transform_type, index in (('SPATIAL', 1), ('SPATIAL', 2),
                                  ('FIELDPLUS', 2), ('FIELDPLUS', 3),
                                  ('FIELDPLUS', 5), ('FIELDPLUS', 4)):
        num_bands = (bands['spatial'] if not (transform_type == 'FIELDPLUS' and index == 2)
                     else bands['temporal'])
        required = evaluate_mask_logic(transform_type, index, num_bands)
        matrix.append({
            'transform': transform_type,
            'index': index,
            'num_bands': num_bands,
            'required_mask': None if required is None else '0x%X' % required,
            'required_bits': (None if required is None
                              else [b for b in range(num_bands) if required & (1 << b)]),
            'waits_for_lowpass': None if required is None else bool(required & 1),
        })

    return {
        'function': 'DecodedBandsValid',
        'file': str(DECODER),
        'line': line_of(text, 'bool DecodedBandsValid('),
        'live_call_sites': live_calls,
        'commented_call_sites': commented,
        'status': 'upstream-supplied but unreachable in the shipped build',
        'matrix': matrix,
    }


# --------------------------------------------------------------- liveness

def parse_liveness(repo):
    threading = read(repo, THREADING)
    decoder = read(repo, DECODER)

    # The queue drain loop and the inverse transform run inside one worker
    # function, so a spinning consumer cannot drain the entry it waits for.
    worker = threading.find('THREAD_PROC(EntropyWorkerThreadProc, lpParam)')
    require(worker >= 0, 'EntropyWorkerThreadProc is missing')
    decode_entropy = threading.find('DecodeEntropy(decoder, work_index, thread_index')
    require(decode_entropy > worker,
            'EntropyWorkerThreadProc no longer drives DecodeEntropy')

    reconstruct = decoder.find('void ReconstructWaveletBand(')
    require(reconstruct >= 0, 'ReconstructWaveletBand is missing')

    producers = decoder.count('UpdateWaveletBandValidFlags(')
    require(producers >= 5, 'unexpectedly few valid-flag writer call sites')

    queue_len = re.search(r'#define DECODING_QUEUE_LENGTH\s+(.+)', read(repo, 'upstream/Codec/codec.h'))
    require(queue_len is not None, 'DECODING_QUEUE_LENGTH is missing')

    return {
        'same_thread_claim': {
            'drain_site': str(THREADING),
            'transform_site': str(decoder[:0] + 'upstream/Codec/decoder.c'),
            'statement': 'the transform-queue drain loop and the inverse transform '
                         'invocation are both inside EntropyWorkerThreadProc, so a '
                         'consumer that blocks inside ReconstructWaveletBand removes '
                         'one of the workers that could publish its input',
            'construction_reachable': True,
        },
        'producer_call_sites': producers,
        'queue_length_expression': queue_len.group(1).strip(),
        'worker_pool': {
            'created_when': 'cpus > 1, capped at 4 threads (decoder.c, AllocDecoderBuffer)',
            'note': 'a blocked worker reduces the drain capacity of a pool of at most 4',
        },
        'bounded_wait_required': True,
        'unbounded_spin_acceptable': False,
    }


# ------------------------------------------------------- queue shutdown path

def parse_queue_shutdown(repo):
    """The path that discards the queue, and the absence of an error for it.

    This is the anchor set the maintainer review asked to be mutated and tested:
    if someone later adds a completeness check before the reset, or adds an error
    assignment in the drain loop, this verifier must fail so the finding is
    re-derived rather than silently kept.
    """
    decoder = read(repo, DECODER)
    threading = read(repo, THREADING)

    start = decoder.find('void WaitForTransformThread(DECODER *decoder)')
    require(start >= 0, 'WaitForTransformThread is missing')
    end = decoder.find('\n}\n', start)
    require(end > start, 'WaitForTransformThread body could not be delimited')
    body = decoder[start:end]

    require('ThreadPoolWaitAllDone(&decoder->entropy_worker_new.pool);' in body,
            'WaitForTransformThread no longer waits for the pool')

    # The four resets must all be present, and none may be conditioned on a check.
    resets = (
        'decoder->transform_queue.started = 0;',
        'decoder->transform_queue.num_entries = 0;',
        'decoder->transform_queue.next_entry = 0;',
        'decoder->transform_queue.free_entry = 0;',
    )
    for statement in resets:
        require(statement in body, 'queue reset statement vanished: %s' % statement)

    # The whole point: no completeness test guards the discard.
    guard_markers = ('num_entries > 0', 'num_entries != 0', 'next_entry <',
                     'free_entry !=', 'unadmitted', 'CODEC_ERROR_BAD_FRAME')
    present = [marker for marker in guard_markers if marker in body]
    require(present == [],
            'WaitForTransformThread now appears to check completeness before the '
            'reset (%r); the silent-drop finding must be re-derived' % (present,))

    # The drain loop must not report an error for an entry it fails to admit.
    drain_start = threading.find('if(data->queue[curr_entry].done == 0 && BANDS_ALL_VALID(wavelet))')
    require(drain_start >= 0, 'the drain admission condition is missing')
    drain_end = threading.find('} while(wavelet && BANDS_ALL_VALID(wavelet));', drain_start)
    require(drain_end > drain_start, 'the drain loop tail could not be delimited')
    drain_body = threading[drain_start:drain_end]
    require('decoder->error' not in drain_body,
            'the drain loop now assigns decoder->error; the silent-drop finding '
            'must be re-derived')

    # done is a claim, not a completion record.
    done_writes = [i for i, line in enumerate(threading.split('\n'), 1)
                   if '.done = 1;' in line]
    done_reads = [i for i, line in enumerate(threading.split('\n'), 1)
                  if '.done == 0' in line]
    require(len(done_writes) == 1,
            'expected exactly one `done = 1` claim site, found %d' % len(done_writes))
    require(len(done_reads) == 1,
            'expected exactly one `done == 0` read, found %d' % len(done_reads))

    return {
        'function': 'WaitForTransformThread',
        'file': str(DECODER),
        'line': line_of(decoder, 'void WaitForTransformThread(DECODER *decoder)'),
        'waits_for_pool': True,
        'completeness_check_before_reset': False,
        'error_set_on_unadmitted_entry': False,
        'resets': list(resets),
        'done_is_a_claim_not_a_completion_record': True,
        'done_write_lines': done_writes,
        'done_read_lines': done_reads,
        'consequence': 'a transform that is never admitted or never executed is '
                       'discarded without a diagnostic, and the frame can still be '
                       'reported as decoded successfully',
    }


# ------------------------------------------------------------- output path

def preprocessor_contexts(text):
    """Map 1-based line number -> [(directive, branch), ...] nesting context.

    `branch` is 'if', 'elif' or 'else'. Nested conditionals are reported from
    outermost to innermost. A line inside `#if A` / `#else` yields both entries,
    which is what the callers need to ask "is this the else of A?".
    """
    contexts = {}
    stack = []
    for number, line in enumerate(text.split('\n'), 1):
        stripped = line.strip()
        if not stripped.startswith('#'):
            if stack:
                contexts[number] = list(stack)
            continue

        directive = stripped[1:].strip()
        keyword, _, rest = directive.partition(' ')
        keyword = keyword.strip()
        rest = rest.strip()

        if keyword in ('if', 'ifdef', 'ifndef'):
            stack.append((rest, 'if'))
        elif keyword == 'elif':
            if stack:
                inner = stack.pop()
                stack.append((inner[0], 'elif'))
        elif keyword == 'else':
            if stack:
                inner = stack.pop()
                stack.append((inner[0], 'else'))
        elif keyword == 'endif':
            if stack:
                stack.pop()

        if stack:
            contexts[number] = list(stack)

    require(not stack, 'unbalanced preprocessor conditionals in the parsed file')
    return contexts


def parse_output_path(repo):
    """The frame-output readiness gates, which the Milestone-1 report omitted.

    Two helpers dominate the output path and they fail in opposite directions:

    * `AllTransformBandsValid` is always compiled (`#if DEBUG || 1`) and is
      called at seven live runtime sites, but its real band check is commented
      out -- it only tests `wavelet != NULL`. A hollow gate that is always on
      and always passes.
    * `AllLowpassBandsValid` is the only helper that inspects
      `band_valid_flags & BAND_VALID_MASK(0)` per channel, and its single call
      site sits behind `#if DEBUG`. `DEBUG` is `(1 && _DEBUG)` and `_DEBUG` is
      an MSVC-only macro that no build file in this repository defines, so the
      only correct output check is compiled out everywhere.
    """
    text = read(repo, DECODER)

    # decoder.c defines DEBUG itself; capture the defining expression.
    match = re.search(r'#ifndef DEBUG\n#define DEBUG\s+(.+?)\n#endif', text)
    require(match is not None, 'the decoder DEBUG definition is missing')
    debug_def = match.group(1).strip()
    require('_DEBUG' in debug_def,
            'DEBUG no longer derives from _DEBUG; re-derive the output gates')

    # No build file in the tree may define _DEBUG, or the analysis inverts.
    defined_in_build = []
    for pattern in ('CMakeLists.txt', 'cmake/*.cmake', 'tests/CMakeLists.txt'):
        for path in sorted(repo.glob(pattern)):
            body = path.read_text(encoding='utf-8', errors='replace')
            for line in body.split('\n'):
                if '_DEBUG' in line and re.search(r'\bD_DEBUG\b|_DEBUG=', line):
                    defined_in_build.append('%s: %s' % (path.name, line.strip()[:60]))
    require(defined_in_build == [],
            'a build file now defines _DEBUG, which would enable the real output '
            'gate: %r' % (defined_in_build,))

    # The wrapper macro that keeps both helpers compiled unconditionally.
    require('#if DEBUG || 1' in text,
            'the always-true wrapper around the output helpers is gone')

    # --- hollow helper -----------------------------------------------------
    start = text.find('static bool AllTransformBandsValid(')
    require(start >= 0, 'AllTransformBandsValid is missing')
    end = text.find('\n}\n', start)
    body = text[start:end]

    require('//if (!AllBandsValid(wavelet))' in body,
            'the commented-out band check in AllTransformBandsValid changed; '
            're-derive whether the output gate is still hollow')
    require('band_valid_flags' not in body,
            'AllTransformBandsValid now inspects band flags; the hollow-gate '
            'finding no longer holds')
    require('if(wavelet == NULL)' in body or 'if (wavelet == NULL)' in body,
            'AllTransformBandsValid no longer even tests for NULL')

    live_calls = [line_of(text, 'if (!AllTransformBandsValid(transform_array')
                  for _ in [0]]
    call_lines = []
    for found in re.finditer(r'if \(!AllTransformBandsValid\(transform_array', text):
        call_lines.append(text.count('\n', 0, found.start()) + 1)
    require(len(call_lines) == 7,
            'expected 7 live AllTransformBandsValid call sites, found %d'
            % len(call_lines))

    # At least one of them must be on the HALF output path that #160 measured.
    half_dispatch = text.find('case DECODED_RESOLUTION_HALF:')
    require(half_dispatch > 0, 'DECODED_RESOLUTION_HALF dispatch is missing')
    require(any(line > half_dispatch for line in call_lines),
            'no live output gate on the HALF path')

    # --- the only correct helper, compiled out ----------------------------
    lp_start = text.find('static bool AllLowpassBandsValid(')
    require(lp_start >= 0, 'AllLowpassBandsValid is missing')
    lp_end = text.find('\n}\n', lp_start)
    lp_body = text[lp_start:lp_end]
    require('band_valid_flags & BAND_VALID_MASK(0)' in lp_body,
            'AllLowpassBandsValid no longer checks band 0')

    lp_calls = []
    for found in re.finditer(r'AllLowpassBandsValid\(transform_array', text):
        line = text.count('\n', 0, found.start()) + 1
        line_start = text.rfind('\n', 0, found.start()) + 1
        lp_calls.append((line, text[line_start:text.find('\n', found.start())].strip()))
    require(len(lp_calls) == 1,
            'expected exactly one AllLowpassBandsValid call site, found %d'
            % len(lp_calls))
    call_line, call_text = lp_calls[0]
    require(call_text.startswith('assert('),
            'the AllLowpassBandsValid call is no longer assert-only')

    # Require the call to be *inside* a `#if DEBUG` block, and that block to end
    # on the next line. A backwards scan for the nearest `#if` is not enough:
    # three `#if DEBUG` blocks sit within a dozen lines of each other in this
    # dispatch, so text proximity would keep passing after the guard is removed.
    contexts = preprocessor_contexts(text)
    call_context = contexts.get(call_line, [])
    require(call_context == [('DEBUG', 'if')],
            'the AllLowpassBandsValid call is no longer the sole body of a '
            '#if DEBUG block (actual nesting: %r)' % (call_context,))

    following = text.split('\n')[call_line].strip()  # 0-based index -> next line
    require(following == '#endif',
            'the #if DEBUG block no longer closes immediately after the call '
            '(next line: %r)' % (following,))

    # One more line of defence: the guard must disappear together with its call.
    require('#if DEBUG\n\t\t\tassert(AllLowpassBandsValid(transform_array, '
            'num_channels, frame_index));\n#endif' in text,
            'the guarded three-line unit around AllLowpassBandsValid changed')

    # --- the third dead-but-correct reader --------------------------------
    # Both call sites sit in an `#else` branch of a `#if _THREADED_DECODER`
    # conditional. Resolve the actual preprocessor nesting instead of trusting
    # a text offset: the file contains many earlier `#if _THREADED_DECODER`
    # blocks, so "after the first one" proves nothing.
    dead_reader = []
    for found in re.finditer(r'if \(AllBandsValid\(wavelet\)\)', text):
        dead_reader.append(text.count('\n', 0, found.start()) + 1)
    require(len(dead_reader) == 2,
            'expected the two non-threaded AllBandsValid call sites, found %d'
            % len(dead_reader))

    contexts = preprocessor_contexts(text)
    for line in dead_reader:
        branches = contexts.get(line, [])
        in_else_of_threaded = any(
            directive == '_THREADED_DECODER' and branch == 'else'
            for directive, branch in branches)
        require(in_else_of_threaded,
                'AllBandsValid call at line %d is no longer inside the #else of a '
                '#if _THREADED_DECODER conditional (context: %r)'
                % (line, branches))

    config = read(repo, CONFIG_H)
    require(re.search(r'#ifndef _THREADED_DECODER\n#define _THREADED_DECODER\s+1',
                      config) is not None,
            '_THREADED_DECODER is no longer defaulted to 1; the two AllBandsValid '
            'sites would become reachable')

    return {
        'debug_macro': {
            'definition': '#define DEBUG ' + debug_def,
            'defined_by_build_file': defined_in_build,
            'evaluates_true_on_linux': False,
            'reason': '_DEBUG is an MSVC Debug-configuration macro; gcc and clang '
                      'builds in this tree never define it, so `(1 && _DEBUG)` is 0',
        },
        'hollow_gate': {
            'symbol': 'AllTransformBandsValid',
            'line': line_of(text, 'static bool AllTransformBandsValid('),
            'always_compiled': True,
            'wrapper': '#if DEBUG || 1',
            'checks_band_flags': False,
            'real_check_present_as_comment': True,
            'tests_only': 'wavelet != NULL',
            'live_call_sites': call_lines,
            'note': 'seven runtime guards on the frame-output path that cannot '
                    'detect an unreconstructed band',
        },
        'correct_gate_compiled_out': {
            'symbol': 'AllLowpassBandsValid',
            'line': line_of(text, 'static bool AllLowpassBandsValid('),
            'checks_band0': True,
            'call_site_line': call_line,
            'call_site_form': 'assert()',
            'guard': '#if DEBUG',
            'nesting': call_context,
            'active_in_any_build': False,
            'note': 'the only per-channel band-0 output check is behind #if DEBUG, '
                    'which no configuration in this tree enables',
        },
        'dead_reader': {
            'symbol': 'AllBandsValid',
            'line': line_of(text, 'static bool AllBandsValid('),
            'call_sites': dead_reader,
            'reachable': False,
            'reason': 'both call sites are in the #else branch of '
                      '#if _THREADED_DECODER, which config.h enables',
            'still_vacuous': 'it forwards to BANDS_ALL_VALID, the released macro',
        },
    }


# ------------------------------------------------------------------- assemble

def build_report(repo):
    repo = Path(repo).resolve()
    report = {
        'issue': 195,
        'milestone': 1,
        'verdict': 'DESIGN_ONLY',
        'repo_head': repo_head(repo),
        'sources': {},
        'macros': parse_macros(repo),
        'band_counts': parse_band_counts(repo),
        'writers': parse_writers(repo),
        'readers': parse_readers(repo),
        'output_path': parse_output_path(repo),
        'queue_shutdown': parse_queue_shutdown(repo),
        'required_bit_matrix': parse_mask_logic(repo),
        'liveness': parse_liveness(repo),
    }
    report['sources'] = {
        str(rel): sha256_file(repo / rel)
        for rel in (DECODER, IMAGE_H, WAVELET_C, WAVELET_H, THREADING, CONFIG_H)
    }
    report['limitations'] = [
        'source reading only: no decode was executed by this pass',
        'the per-topology matrix is re-derived from DecodedBandsValid, which the '
        'shipped build never calls',
        'HALF 1080p BGRA on the pinned packet is the only measured condition in '
        'the referenced evidence; FULL was not exercised on either host',
        'the temporal branch has no runtime check at all in a release build, so its '
        'race is characterised from source, not measured',
        'no claim about other formats, resolutions or GOP structures',
    ]
    return report


def repo_head(repo):
    try:
        out = subprocess.run(['git', '-C', str(repo), 'rev-parse', 'HEAD'],
                             capture_output=True, text=True, check=True)
        return out.stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


# ----------------------------------------------------------------- self-test

SELF_TESTS = (
    # name, file, old, new, occurrences
    ('released macro replaced by the strict form',
     IMAGE_H,
     MACRO_RELEASED,
     '#define BANDS_ALL_VALID(wavelet)\t((wavelet)->band_valid_flags == '
     '(uint32_t)((1 << (wavelet)->num_bands) - 1))', 1),
    ('band-count table altered',
     WAVELET_C,
     '\t1, 2, 2, 4,',
     '\t1, 2, 2, 3,', 1),
    ('spatial guard deleted',
     DECODER,
     CONSUMER_ANCHOR,
     '\t\t\t// Check that all of the wavelet bands have been decoded\n'
     '\t\t\tif (!BANDS_ALL_VALID(wavelet))', 1),
    ('started writer takes the lock unconditionally',
     DECODER,
     '#if _DELAYED_THREAD_START==0\n\t\tif(decoder->entropy_worker_new.pool.thread_count)\n'
     '\t\t\tLock(&decoder->entropy_worker_new.lock);\n#endif\n',
     'if(decoder->entropy_worker_new.pool.thread_count)\n'
     '\t\t\tLock(&decoder->entropy_worker_new.lock);\n', 1),
    ('DecodedBandsValid gains a live call site',
     DECODER,
     '//if (DecodedBandsValid(wavelet, temporal_index))',
     'if (DecodedBandsValid(wavelet, temporal_index))', 1),
    ('delayed thread start disabled',
     CONFIG_H,
     '#define _DELAYED_THREAD_START\t1',
     '#define _DELAYED_THREAD_START\t0', 1),
    # --- output-path findings (Milestone 1 correction) --------------------
    ('hollow output gate actually checks bands',
     DECODER,
     '\t\t//if (!AllBandsValid(wavelet))\n\t\tif(wavelet == NULL)',
     '\t\tif (!AllBandsValid(wavelet))', 1),
    ('correct output gate moved out of #if DEBUG',
     DECODER,
     '#if DEBUG\n\t\t\tassert(AllLowpassBandsValid(transform_array, num_channels, frame_index));\n#endif',
     'assert(AllLowpassBandsValid(transform_array, num_channels, frame_index));', 1),
    ('build file starts defining _DEBUG',
     Path('CMakeLists.txt'),
     'add_subdirectory(src)',
     'add_compile_definitions(_DEBUG=1)\nadd_subdirectory(src)', 1),
    ('threaded decoder disabled, reviving AllBandsValid',
     CONFIG_H,
     '#define _THREADED_DECODER\t1',
     '#define _THREADED_DECODER\t0', 1),
    # --- queue shutdown path (Milestone-1 completion) ---------------------
    ('completeness check added before the queue reset',
     DECODER,
     'ThreadPoolWaitAllDone(&decoder->entropy_worker_new.pool);',
     'ThreadPoolWaitAllDone(&decoder->entropy_worker_new.pool);\n'
     '\t\tif (decoder->transform_queue.num_entries > 0)',
     1),
    ('drain loop starts reporting an error',
     THREADING,
     'data->queue[curr_entry].done = 1;',
     'data->queue[curr_entry].done = 1;\n'
     '\t\t\t\t\t\t\t\tdecoder->error = CODEC_ERROR_BAD_FRAME;',
     1),
    # The four reset statements also appear in the per-group init, so the
    # mutation is anchored on the whole block and must actually land in
    # WaitForTransformThread. An unanchored single-line mutation hit the other
    # copy and the verifier correctly kept passing.
    ('queue reset statement removed',
     DECODER,
     'ThreadPoolWaitAllDone(&decoder->entropy_worker_new.pool);\n'
     '\t\n'
     '\t\tdecoder->transform_queue.started = 0;\n'
     '\t\tdecoder->transform_queue.num_entries = 0;\n'
     '\t\tdecoder->transform_queue.next_entry = 0;\n'
     '\t\tdecoder->transform_queue.free_entry = 0;',
     'ThreadPoolWaitAllDone(&decoder->entropy_worker_new.pool);\n'
     '\t\n'
     '\t\tdecoder->transform_queue.started = 0;\n'
     '\t\tdecoder->transform_queue.num_entries = 0;\n'
     '\t\tdecoder->transform_queue.next_entry = 0;\n'
     '\t\t/* reset removed */',
     1),
)


def run_self_test(repo):
    repo = Path(repo).resolve()
    failures = []
    build_report(repo)  # baseline must pass first

    # Files the verifier reads. The self-test copies exactly these, so a
    # mutation can never be masked by an unrelated file that was left behind.
    VERIFIER_INPUTS = (DECODER, IMAGE_H, WAVELET_C, WAVELET_H, THREADING, CONFIG_H,
                       Path('upstream/Codec/codec.h'), Path('CMakeLists.txt'))

    for name, rel, old, new, count in SELF_TESTS:
        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / 'tree'
            for source in VERIFIER_INPUTS:
                target = copy / source
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(repo / source, target)
            target = copy / rel
            text = target.read_text(encoding='utf-8', errors='replace')
            text = text.replace('\r\n', '\n')
            if text.count(old) < count:
                failures.append('%s: anchor not found, self-test is stale' % name)
                continue
            target.write_text(text.replace(old, new, count), encoding='utf-8')
            try:
                build_report(copy)
            except VerifyError:
                continue
            failures.append('%s: verifier still passed a mutated tree' % name)

    if failures:
        for failure in failures:
            print('SELF-TEST FAIL %s' % failure, file=sys.stderr)
        return 1
    print('SELF-TEST PASS: %d mutations rejected' % len(SELF_TESTS))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true',
                        help='verify the frozen source against the contract')
    parser.add_argument('--json', metavar='PATH',
                        help='write the re-derived report as JSON')
    parser.add_argument('--self-test', action='store_true',
                        help='mutate a scratch copy and confirm the verifier fails')
    parser.add_argument('--repo', default=str(REPO), help=argparse.SUPPRESS)
    args = parser.parse_args(argv)

    if args.self_test:
        return run_self_test(args.repo)

    try:
        report = build_report(args.repo)
    except VerifyError as error:
        print('VERIFY FAIL: %s' % error, file=sys.stderr)
        return 2

    if args.json:
        out = Path(args.json)
        if not out.is_absolute():
            out = Path(args.repo).resolve() / out
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(report, indent=1) + '\n', encoding='utf-8')
        print('wrote %s' % out)

    matrix = report['required_bit_matrix']['matrix']
    print('verdict: %s' % report['verdict'])
    print('released predicate: valid == started (vacuous at %s for four bands)'
          % report['macros']['vacuous_state_4_band'])
    print('inverse-branch guards: %d, temporal branch: assert only (removed by NDEBUG)'
          % report['readers']['inverse_branch_guards']['count'])
    print('required-bit matrix (re-derived from DecodedBandsValid, %d live callers):'
          % len(report['required_bit_matrix']['live_call_sites']))
    for row in matrix:
        print('  %-9s index=%d bands=%d required=%s'
              % (row['transform'], row['index'], row['num_bands'], row['required_mask']))
    print('VERIFY PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
