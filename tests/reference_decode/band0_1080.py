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
EXPECTED_PUBLISH_BAND0 = 6

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
    """Match every real wait to a band-0 publication on the same IMAGE object."""
    publishes = [e for e in events if e['event'] == 'BAND_PUBLISH' and e.get('band') == 0]
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
        'gate_entries': sum(1 for e in events if e['event'] == 'GATE_ENTRY'),
        'waits': waits,
        'coverage': coverage,
        'timeouts': timeouts,
        'premature_entries': premature_entries,
        'publishes': len(publishes),
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


def summarize(records):
    arms = {}
    for arm in ARM_ORDER:
        calls = [r for r in records if r['arm'] == arm]
        hashes = Counter(r['output']['sha256'] for r in calls
                         if 'output' in r and r['output_valid'])
        modal = hashes.most_common(1)[0][0] if hashes else None
        corrupt = sum(1 for r in calls
                      if 'output' in r and r['output_valid'] and
                      r['output']['sha256'] != modal)
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
        }
    return arms


def classify(arms):
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
    for arm in ARM_ORDER:
        if arms[arm]['gate_entries'] == 0:
            reasons.append(f'probe records missing in arm {arm}')
    if reasons:
        return 'INCONCLUSIVE', reasons
    return 'CROSS_HOST_CONFIRMATION_PASS', []


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
    }
    if args.preflight_only:
        print(json.dumps(preflight, indent=1))
        return 0

    records = []
    for arm in ARM_ORDER:
        for index in range(1, args.calls + 1):
            records.append(run_call(worker, packet, out_dir, arm, index))

    arms = summarize(records)
    verdict, reasons = classify(arms)
    report = dict(preflight)
    report.update({
        'arms': arms,
        'verdict': verdict,
        'reasons': reasons,
        'ledger': records,
        'note': 'Diagnostic only. Not a production decoder patch or release gate.',
    })
    Path(args.json).write_text(json.dumps(report, indent=1), encoding='utf-8')
    concise = {arm: {key: value for key, value in arms[arm].items()
                     if key != 'output_counts'} for arm in ARM_ORDER}
    print(json.dumps({'verdict': verdict, 'reasons': reasons, 'arms': concise,
                      'worker_sha256': checks['worker_sha256']}, indent=1))
    return 0 if verdict == 'CROSS_HOST_CONFIRMATION_PASS' else 1


if __name__ == '__main__':
    sys.exit(main())
