#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Test-only classification of bounded immutable-sample decode observations.

This consumes observations; it does not run a decoder or certify their origin.
Missing evidence never becomes a successful encoder or release gate.
"""
import argparse
import json
import math
from pathlib import Path
import re

SHA256 = re.compile(r'[0-9a-f]{64}\Z')
MAX_CALLS = 150  # At most 30 independent processes, five calls each.


def classify(packet_equal, calls, minimum_psnr_db=35.0):
    """Classify observed reference gates; preserve encoder-failure precedence."""
    if packet_equal is False:
        return 'ENCODE_BITSTREAM_MISMATCH'
    if packet_equal is not None and packet_equal is not True:
        raise ValueError('packet_equal must be boolean or null')
    if not math.isfinite(minimum_psnr_db) or minimum_psnr_db < 35.0:
        raise ValueError('quality threshold must be finite and at least 35 dB')
    if len(calls) > MAX_CALLS:
        raise ValueError('repeat matrix exceeds diagnostic bound')
    if not calls:
        return 'REFERENCE_EVIDENCE_INCOMPLETE'
    seen = set()
    processes = set()
    pixel_hashes = set()
    incomplete = False
    quality_failed = False
    for call in calls:
        process, ordinal = call['process'], call['ordinal']
        if type(process) is not int or type(ordinal) is not int or not (1 <= process <= 30 and 1 <= ordinal <= 5):
            raise ValueError('invalid process/call ordinal')
        if (process, ordinal) in seen:
            raise ValueError('duplicate observation')
        seen.add((process, ordinal))
        processes.add(process)
        status = call['decoder_status']
        if type(status) is not int:
            raise ValueError('decoder_status must be an integer API code')
        if status != 0:
            return 'REFERENCE_DECODE_ERROR'
        # The consumer requires a successful buffer/geometry validation from
        # the measurement harness; an API success code alone is insufficient.
        if call.get('output_valid') is False:
            return 'REFERENCE_DECODE_ERROR'
        if call.get('output_valid') is not True:
            incomplete = True
        digest = call.get('pixel_sha256')
        if digest is None:
            incomplete = True
        elif not isinstance(digest, str) or not SHA256.fullmatch(digest):
            raise ValueError('invalid pixel SHA-256')
        else:
            pixel_hashes.add(digest)
        psnr = call.get('psnr_db')
        # JSON has no portable infinity: exact equality must be explicit.
        if psnr is None:
            if call.get('source_pixels_exact') is not True:
                incomplete = True
        elif type(psnr) not in (int, float) or not math.isfinite(psnr):
            raise ValueError('invalid PSNR measurement')
        elif psnr <= minimum_psnr_db:
            quality_failed = True
    if len(pixel_hashes) > 1:
        return 'REFERENCE_DECODER_NONDETERMINISTIC'
    if quality_failed:
        return 'DECODE_QUALITY_FAILURE_STABLE' if not incomplete and len(calls) >= 2 else 'REFERENCE_EVIDENCE_INCOMPLETE'
    # PASS requires both within-process and fresh-process observations.
    if incomplete or packet_equal is None or len(processes) < 2 or any(
            sum(p == process for p, _ in seen) < 2 for process in processes):
        return 'REFERENCE_EVIDENCE_INCOMPLETE'
    return 'PASS'


def report(document):
    if not isinstance(document, dict) or type(document.get('schema_version')) is not int or document['schema_version'] != 1:
        raise ValueError('unsupported evidence schema')
    packet = document.get('sample_sha256')
    if not isinstance(packet, str) or not SHA256.fullmatch(packet):
        raise ValueError('immutable sample SHA-256 is required')
    calls = document['calls']
    if not isinstance(calls, list) or any(not isinstance(call, dict) for call in calls):
        raise ValueError('calls must be observation objects')
    # Every observation must identify the same immutable packet. A changed
    # packet cannot be explained away as decoder nondeterminism.
    packet_equal = document.get('encoder_packets_equal')
    if any(call.get('sample_sha256') != packet for call in calls):
        packet_equal = False
    classification = classify(packet_equal, calls)
    observed_alternate = document.get('alternate_decoder')
    # Do not echo arbitrary provider metadata or paths into public summaries.
    alternate = {'status': 'UNAVAILABLE', 'unavailable_reason': 'not_measured'}
    if isinstance(observed_alternate, dict) and observed_alternate.get('status') == 'MEASURED':
        alternate = {'status': 'MEASURED', 'acceptance': 'UNREVIEWED'}
    return {'schema_version': 1, 'classification': classification,
            'encoder_packet_comparison': 'MISMATCH' if packet_equal is False else ('MATCH' if packet_equal is True else 'UNAVAILABLE'),
            'encoder_gate': 'PASS' if classification == 'PASS' else (
                'FAIL' if classification in ('ENCODE_BITSTREAM_MISMATCH', 'REFERENCE_DECODE_ERROR', 'DECODE_QUALITY_FAILURE_STABLE') else 'UNRESOLVED'),
            # This classifier alone cannot release a codec or replace the
            # independent encoder, Windows repetition and second-decoder gates.
            'release_gate': 'UNRESOLVED', 'release_blocked': True,
            'sample_sha256': packet, 'observed_calls': len(calls),
            'observed_processes': len({c['process'] for c in calls}),
            'alternate_decoder': alternate}


def strict_document(text):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError('duplicate evidence key')
            result[key] = value
        return result

    def invalid_constant(_):
        raise ValueError('non-finite evidence constant')

    def finite_float(value):
        number = float(value)
        if not math.isfinite(number):
            raise ValueError('non-finite evidence number')
        return number

    return json.loads(text, object_pairs_hook=pairs, parse_constant=invalid_constant,
                      parse_float=finite_float)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('evidence', type=Path)
    args = parser.parse_args()
    try:
        if args.evidence.stat().st_size > 1024 * 1024:
            raise ValueError('evidence exceeds diagnostic bound')
        result = report(strict_document(args.evidence.read_text(encoding='utf-8')))
    except (OSError, KeyError, TypeError, ValueError, RecursionError):
        # Never echo untrusted evidence or paths in a publishable diagnostic.
        print(json.dumps({'schema_version': 1, 'classification': 'INVALID_EVIDENCE',
                          'release_blocked': True}))
        return 2
    print(json.dumps(result, sort_keys=True, allow_nan=False))
    # Quarantined/incomplete remains nonzero, never CTest SKIP or success.
    return 0 if result['classification'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
