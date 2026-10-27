#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Pre-registered RCA-160 Windows cross-host A/B/A-prime protocol.

One binary is used for all arms. RCA160_STRICT_GATE is the only arm variable:
O and A-prime have the gate off (original admission), B has the gate on.
Every call is a fresh process with one decode. The script records full-image
SHA-256 over the written active bytes, the gate ledger and producer coverage,
and fails closed to INCONCLUSIVE when the registered conditions are not met.
"""
import argparse
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

PROTOCOL_ID = 'rca160-windows-cross-host-band0-20261019-v1'
PACKET_BYTES = 900396
PACKET_SHA = '2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52'
SOURCE_BYTES = 8294400
SOURCE_SHA = '45c166c3d6a3710d620ff5d738980224edf53cf9ce1bceb87267e474294afff1'
WIDTH, HEIGHT = 1920, 1080
ARM_ORDER = ('O', 'B', 'AP')
GATE_SETTING = {'O': '0', 'B': '1', 'AP': '0'}
CALLS_PER_ARM = 120
CHILD_TIMEOUT_SECONDS = 60
GATE_MAX_SPINS = 50000000
GATE_MAX_WAIT_MS = 5000
EXPECTED_GATE_ENTRIES = 9  # 3 channels x (one index1 + two index2 sites)
# Original pre-registration value, retained only to re-evaluate that rule.
# It was derived from the two spatial lowpass sites and omitted the entropy
# lowpass and frame-wavelet publishers, so the literal rule is reported
# separately and is expected to be INCONCLUSIVE on the published ledger.
REGISTERED_PUBLISH_BAND0 = 6

LINE = re.compile(r'^(\w+) (.*)$')
FIELD = re.compile(r'(\w+)=([^\s]+)')


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def parse_record(line):
    match = LINE.match(line.strip())
    if not match:
        return None
    fields = dict(FIELD.findall(match.group(2)))
    fields['event'] = match.group(1)
    for key in ('ch', 'idx', 'strict', 'spins', 'elapsed_ms', 'band',
                'num_bands', 't_ns', 'tid'):
        if key in fields:
            try:
                fields[key] = int(fields[key], 10)
            except ValueError:
                pass
    for key in ('wptr', 'old_valid', 'old_started', 'new_valid', 'new_started',
                'valid', 'started'):
        if key in fields:
            try:
                fields[key] = int(fields[key], 16)
            except ValueError:
                pass
    return fields


def parse_probe_log(path):
    records = []
    with path.open('r', encoding='utf-8', errors='replace') as stream:
        for line in stream:
            record = parse_record(line)
            if record:
                records.append(record)
    return records


def producer_coverage(events):
    """Match every real wait to a band-0 publication on the same IMAGE object.

    The publication count is descriptive. The probe observes every call to the
    flag publisher, including calls that do not change band 0, so no fixed
    per-call count is a source-derived invariant. Operation identity is:
    gate entry, then a same-IMAGE band-0 invalid->valid transition inside the
    wait window.
    """
    publishes = [e for e in events if e['event'] == 'BAND_PUBLISH' and e.get('band') == 0]
    transitions = [e for e in publishes
                   if (e.get('old_valid', 0) & 1) == 0 and (e.get('new_valid', 0) & 1) == 1]
    entry_events = [e for e in events if e['event'] == 'GATE_ENTRY']
    first_entry_ns = min((e.get('t_ns', 0) for e in entry_events), default=None)
    entropy_transitions = (sum(1 for e in transitions
                               if first_entry_ns is not None and
                               e.get('t_ns', 0) < first_entry_ns)
                           if first_entry_ns is not None else 0)
    entries = {}
    waits = []
    coverage = []
    for event in events:
        if event['event'] == 'GATE_ENTRY':
            entries[(event.get('wptr'), event.get('ch'), event.get('idx'))] = event
        elif event['event'] == 'GATE_ADMIT' and event.get('spins', 0) > 0:
            entry = entries.get((event.get('wptr'), event.get('ch'), event.get('idx')))
            match = None
            if entry is not None:
                for publish in publishes:
                    if (publish.get('wptr') == event.get('wptr') and
                            publish.get('band') == 0 and
                            (publish.get('old_valid', 0) & 1) == 0 and
                            (publish.get('new_valid', 0) & 1) == 1 and
                            entry.get('t_ns', 0) <= publish.get('t_ns', 0) <= event.get('t_ns', 0)):
                        match = publish
                        break
            waits.append(event)
            coverage.append({
                'ch': event.get('ch'), 'idx': event.get('idx'),
                'spins': event.get('spins'), 'elapsed_ms': event.get('elapsed_ms'),
                'entry_valid': entry.get('valid') if entry else None,
                'entry_started': entry.get('started') if entry else None,
                'admit_valid': event.get('final_valid'),
                'admit_started': event.get('final_started'),
                'producer_publish_matched': match is not None,
            })
    timeouts = [e for e in events if e['event'] == 'GATE_TIMEOUT']
    premature_entries = [e for e in events
                         if e['event'] == 'GATE_ENTRY' and ((e.get('valid', 0) & 1) == 0)]
    return {
        'gate_entries': len(entry_events),
        'waits': waits,
        'coverage': coverage,
        'timeouts': timeouts,
        'premature_entries': premature_entries,
        'publishes': len(publishes),
        'publish_calls_band0': len(publishes),
        'transitions_band0': len(transitions),
        'transition_pointers_band0': len({e.get('wptr') for e in transitions}),
        'entropy_transitions_band0': entropy_transitions,
    }


def run_call(worker, packet, out_dir, arm, index):
    prefix = out_dir / arm / f'{index:03d}'
    prefix.parent.mkdir(parents=True, exist_ok=True)
    probe_log = prefix.with_suffix('.probe.log')
    output = Path(str(prefix) + '-1.bgra')
    for stale in (probe_log, output, prefix.with_suffix('.stdout')):
        if stale.exists():
            stale.unlink()
    env = dict(os.environ)
    env.update({
        'HYPERCINE_RESOLUTION': 'HALF',
        'HYPERCINE_OPENS': '1',
        'HYPERCINE_CPU_MAX': '4',
        'RCA160_STRICT_GATE': GATE_SETTING[arm],
        'RCA160_PROBE_LOG': str(probe_log),
    })
    command = [str(worker), 'decode-bgra-on', '1080', str(packet), str(prefix)]
    timed_out = False
    try:
        result = subprocess.run(command, env=env, capture_output=True,
                                timeout=CHILD_TIMEOUT_SECONDS)
        stdout = result.stdout.decode('utf-8', 'replace')
        stderr = result.stderr.decode('utf-8', 'replace')
        returncode = result.returncode
    except subprocess.TimeoutExpired:
        timed_out = True
        stdout = ''
        stderr = ''
        returncode = None
    if stdout.strip():
        prefix.with_suffix('.stdout').write_text(stdout, encoding='utf-8')

    summary = None
    for line in reversed(stdout.strip().splitlines()):
        try:
            candidate = json.loads(line)
        except ValueError:
            continue
        if isinstance(candidate, dict) and 'ordinal' in candidate:
            summary = candidate
            break

    events = parse_probe_log(probe_log) if probe_log.exists() else []
    coverage = producer_coverage(events)
    output_valid = bool(summary and summary.get('output_valid') and
                        summary.get('decoder_status') == 0 and output.is_file())
    record = {
        'arm': arm, 'call': index, 'returncode': returncode,
        'timed_out': timed_out, 'output_valid': output_valid,
        'worker': summary, 'stderr': stderr.strip()[:400],
        'gate': {
            'gate_entries': coverage['gate_entries'],
            'premature_entries': len(coverage['premature_entries']),
            'waits': len(coverage['waits']),
            'timeouts': len(coverage['timeouts']),
            'publishes_band0': coverage['publishes'],
            'publish_calls_band0': coverage['publish_calls_band0'],
            'transitions_band0': coverage['transitions_band0'],
            'transition_pointers_band0': coverage['transition_pointers_band0'],
            'entropy_transitions_band0': coverage['entropy_transitions_band0'],
            'waits_matched': sum(1 for c in coverage['coverage']
                                 if c['producer_publish_matched']),
            'coverage': coverage['coverage'],
        },
    }
    if output_valid:
        size = output.stat().st_size
        width = summary.get('width')
        height = summary.get('height')
        pitch = summary.get('pitch')
        record['output'] = {
            'bytes': size,
            'sha256': sha256_file(output),
            'domain': {
                'width': width, 'height': height, 'pitch': abs(pitch) if pitch else pitch,
                'active_row_bytes': width * 4 if width else None,
                'padding_bytes': size - width * height * 4 if width and height else None,
                'note': 'full written buffer; one decode per fresh process',
            },
        }
    return record


def recorded_digest(record):
    """Return a usable output digest, or None when the record has no output."""
    if not record.get('output_valid'):
        return None
    output = record.get('output')
    if not isinstance(output, dict):
        return None
    digest = output.get('sha256')
    return digest if isinstance(digest, str) and digest else None


def summarize(records):
    arms = {}
    for arm in ARM_ORDER:
        calls = [r for r in records if r['arm'] == arm]
        digests = [recorded_digest(r) for r in calls]
        hashes = Counter(digest for digest in digests if digest is not None)
        modal = hashes.most_common(1)[0][0] if hashes else None
        corrupt = sum(1 for digest in digests
                      if digest is not None and digest != modal)
        publish_counts = Counter(
            r['gate'].get('publish_calls_band0', r['gate'].get('publishes_band0', 0))
            for r in calls)
        transition_counts = Counter(
            r['gate'].get('transitions_band0', 0) for r in calls)
        pointer_counts = Counter(
            r['gate'].get('transition_pointers_band0', 0) for r in calls)
        arms[arm] = {
            'calls': len(calls),
            'valid_outputs': sum(1 for r in calls if r['output_valid']),
            'decode_failures': sum(1 for r in calls if not r['output_valid']),
            'timeouts': sum(1 for r in calls if r['timed_out']),
            'distinct_outputs': len(hashes),
            'modal_sha256': modal,
            'output_counts': {digest: count for digest, count in hashes.most_common()},
            'corrupt': corrupt,
            'gate_entries': sum(r['gate']['gate_entries'] for r in calls),
            'premature_entries': sum(r['gate']['premature_entries'] for r in calls),
            'real_waits': sum(r['gate']['waits'] for r in calls),
            'gate_timeouts': sum(r['gate']['timeouts'] for r in calls),
            'waits_matched': sum(r['gate']['waits_matched'] for r in calls),
            'publishes_band0': sum(r['gate']['publishes_band0'] for r in calls),
            'publish_calls_band0': sum(r['gate'].get('publish_calls_band0',
                                                     r['gate'].get('publishes_band0', 0))
                                       for r in calls),
            'publish_call_counts': {str(key): value
                                    for key, value in sorted(publish_counts.items())},
            'transition_counts': {str(key): value
                                  for key, value in sorted(transition_counts.items())},
            'transition_pointer_counts': {str(key): value
                                          for key, value in sorted(pointer_counts.items())},
            'entropy_transitions_band0': sum(r['gate'].get('entropy_transitions_band0', 0)
                                             for r in calls),
        }
    return arms


def classify(arms, records=None):
    """Apply the corrected rule: operation identity, not a fixed count."""
    reasons = []
    o, b, ap = arms['O'], arms['B'], arms['AP']
    if o['decode_failures'] or ap['decode_failures'] or b['decode_failures']:
        reasons.append('decode failure in one or more arms')
    if o['corrupt'] == 0:
        reasons.append('original arm O did not reproduce corruption')
    if ap['corrupt'] == 0:
        reasons.append('gate-disabled return arm A-prime did not reproduce corruption')
    if b['corrupt'] != 0:
        reasons.append('gate-on arm B still corrupted')
    if b['gate_timeouts']:
        reasons.append('gate-on arm B hit the bounded gate timeout')
    if b['real_waits'] == 0:
        reasons.append('gate-on arm B never actually waited')
    if b['real_waits'] != b['waits_matched']:
        reasons.append('at least one B wait has no matched band-0 publication')
    if b['gate_entries'] != b['calls'] * EXPECTED_GATE_ENTRIES:
        reasons.append('B gate-entry coverage incomplete')
    if records is not None:
        if any(r['gate'].get('gate_entries', 0) == 0 for r in records):
            reasons.append('probe records missing in at least one call')
        if any(r['gate'].get('gate_entries', 0) != EXPECTED_GATE_ENTRIES
               for r in records):
            reasons.append('per-call gate-entry coverage is not exactly %d'
                           % EXPECTED_GATE_ENTRIES)
        if any(r.get('output_valid') and recorded_digest(r) is None
               for r in records):
            reasons.append('output digest missing in at least one call')
    for arm in ARM_ORDER:
        if arms[arm]['gate_entries'] == 0:
            reasons.append(f'probe records missing in arm {arm}')
    if reasons:
        return 'INCONCLUSIVE', reasons
    return 'CROSS_HOST_CONFIRMATION_PASS', []


def classify_registered_publication_rule(arms):
    """Re-evaluate the literal pre-registered fixed-count rule.

    This is disclosure, not the accepted rule. The registered expectation of
    six band-0 publications per call omitted the entropy and frame publishers.
    """
    reasons = []
    for arm in ARM_ORDER:
        counts = arms[arm].get('publish_call_counts')
        if not counts:
            reasons.append(f'publication-count distribution unavailable in arm {arm}')
            continue
        observed = sorted({int(key) for key in counts})
        if observed != [REGISTERED_PUBLISH_BAND0]:
            reasons.append(
                f'registered rule expected {REGISTERED_PUBLISH_BAND0} band-0 '
                f'publications per call in arm {arm}; observed {observed}')
    return ('INCONCLUSIVE' if reasons else 'CROSS_HOST_CONFIRMATION_PASS', reasons)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--worker', required=True)
    parser.add_argument('--packet', required=True)
    parser.add_argument('--source', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--json', required=True)
    parser.add_argument('--calls', type=int, default=CALLS_PER_ARM)
    parser.add_argument('--preflight-only', action='store_true')
    args = parser.parse_args()

    worker = Path(args.worker).resolve()
    packet = Path(args.packet).resolve()
    source = Path(args.source).resolve()
    out_dir = Path(args.out).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    checks = {
        'worker_sha256': sha256_file(worker),
        'worker_bytes': worker.stat().st_size,
        'packet_sha256': sha256_file(packet),
        'packet_bytes': packet.stat().st_size,
        'source_sha256': sha256_file(source),
        'source_bytes': source.stat().st_size,
    }
    expected = {
        'packet_sha256': PACKET_SHA, 'packet_bytes': PACKET_BYTES,
        'source_sha256': SOURCE_SHA, 'source_bytes': SOURCE_BYTES,
    }
    mismatch = {key: {'expected': value, 'actual': checks[key]}
                for key, value in expected.items() if checks[key] != value}
    if mismatch:
        print(json.dumps({'preflight': 'FAIL', 'mismatch': mismatch}, indent=1))
        return 2
    preflight = {
        'protocol': PROTOCOL_ID,
        'preflight': 'PASS',
        'platform': platform.platform(),
        'python': platform.python_version(),
        'checks': checks,
        'condition': {
            'resolution': 'HALF', 'output_format': 'BGRA 8-bit',
            'cpu_max': 4, 'opens_per_process': 1, 'fresh_process_per_call': True,
            'gate_max_spins': GATE_MAX_SPINS,
            'gate_max_wait_ms': GATE_MAX_WAIT_MS,
            'arms': {'O': 'gate off', 'B': 'gate on', 'A-prime': 'gate off'},
            'calls_per_arm': args.calls,
            'child_timeout_seconds': CHILD_TIMEOUT_SECONDS,
        },
        'digest_domain': 'SHA-256 over the complete written output buffer; '
                         'width/height/pitch recorded per call',
        'coverage_rule': 'corrected operation-identity rule: 9 gate entries per '
                         'call; every B wait matched to a same-IMAGE band-0 '
                         'invalid->valid transition inside the wait window; no '
                         'gate timeout; missing per-call probe log is INCONCLUSIVE. '
                         'Band-0 publication counts are descriptive, not a fixed '
                         'pass condition.',
    }
    if args.preflight_only:
        print(json.dumps(preflight, indent=1))
        return 0

    records = []
    for arm in ARM_ORDER:
        for index in range(1, args.calls + 1):
            records.append(run_call(worker, packet, out_dir, arm, index))

    arms = summarize(records)
    verdict, reasons = classify(arms, records)
    registered_verdict, registered_reasons = classify_registered_publication_rule(arms)
    report = dict(preflight)
    report.update({
        'arms': arms,
        'verdict': verdict,
        'reasons': reasons,
        'registered_rule_verdict': registered_verdict,
        'registered_rule_reasons': registered_reasons,
        'ledger': records,
        'note': 'Diagnostic only. Not a production decoder patch or release gate. '
                'The corrected operation-identity rule is authoritative; the '
                'original six-publication rule is re-evaluated for disclosure.',
    })
    Path(args.json).write_text(json.dumps(report, indent=1), encoding='utf-8')
    concise = {arm: {key: value for key, value in arms[arm].items()
                     if key != 'output_counts'} for arm in ARM_ORDER}
    print(json.dumps({'verdict': verdict, 'reasons': reasons,
                      'registered_rule_verdict': registered_verdict,
                      'registered_rule_reasons': registered_reasons,
                      'arms': concise,
                      'worker_sha256': checks['worker_sha256']}, indent=1))
    return 0 if verdict == 'CROSS_HOST_CONFIRMATION_PASS' else 1


if __name__ == '__main__':
    sys.exit(main())
