#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Re-evaluate the preserved RCA-160 Windows ledger without decoding.

Inputs are the sanitized ledger from the published run, its retained BGRA
outputs and its retained probe logs. The tool recomputes output digests,
re-derives both the literal pre-registered rule and the corrected
operation-identity rule, and records post-conversion lane digests for every
corrupt call. It does not create a new decoder run and does not claim codec
channel identity: the retained artifacts contain converted BGRA only.
"""
import argparse
import hashlib
import json
import math
import sys
from collections import Counter
from pathlib import Path

import band0_1080 as protocol

WIDTH, HEIGHT = 960, 540
LANE_NAMES = 'BGRA'
ARTIFACT_MISSING = 'artifact missing'


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def read_lanes(path):
    data = path.read_bytes()
    if len(data) != WIDTH * HEIGHT * 4:
        raise SystemExit(f'{path.name}: expected {WIDTH * HEIGHT * 4} bytes, '
                         f'got {len(data)}')
    lanes = [bytearray(WIDTH * HEIGHT) for _ in range(4)]
    for index in range(0, len(data), 4):
        pixel = index >> 2
        for lane in range(4):
            lanes[lane][pixel] = data[index + lane]
    return data, lanes


def rank1_energy_fraction(diff, rows, cols, iterations=40):
    """Fraction of squared energy captured by the first singular value."""
    vector = [1.0 / math.sqrt(cols)] * cols
    sigma = 0.0
    for _ in range(iterations):
        left = [0.0] * rows
        for row in range(rows):
            base = row * cols
            left[row] = sum(diff[base + col] * vector[col] for col in range(cols))
        left_norm = math.sqrt(sum(value * value for value in left))
        if left_norm == 0.0:
            return 0.0
        left = [value / left_norm for value in left]
        right = [0.0] * cols
        for row in range(rows):
            weight = left[row]
            if weight:
                base = row * cols
                for col in range(cols):
                    right[col] += diff[base + col] * weight
        right_norm = math.sqrt(sum(value * value for value in right))
        if right_norm == 0.0:
            return 0.0
        vector = [value / right_norm for value in right]
        sigma = right_norm
    energy = sum(value * value for value in diff)
    return (sigma * sigma) / energy if energy else 0.0


def lane_diff_stats(corrupt_lane, modal_lane):
    diffs = [a - b for a, b in zip(corrupt_lane, modal_lane)]
    nonzero = [(index, value) for index, value in enumerate(diffs) if value]
    if not nonzero:
        return {'identical': True, 'diff_bytes': 0, 'max_abs': 0,
                'rows_touched': 0, 'rank1_energy_fraction': 0.0}
    absolute = [abs(value) for _, value in nonzero]
    rows = sorted({index // WIDTH for index, _ in nonzero})
    return {
        'identical': False,
        'diff_bytes': len(nonzero),
        'max_abs': max(absolute),
        'mean_abs': round(sum(absolute) / len(absolute), 3),
        'rows_touched': len(rows),
        'row_first': rows[0],
        'row_last': rows[-1],
        'rank1_energy_fraction': round(
            rank1_energy_fraction(diffs, HEIGHT, WIDTH), 6),
    }


def recorded_digest(record):
    """Return a usable output digest, or None when the record has no output."""
    if not record.get('output_valid'):
        return None
    output = record.get('output')
    if not isinstance(output, dict):
        return None
    digest = output.get('sha256')
    return digest if isinstance(digest, str) and digest else None


def modal_outputs(records, arm_order):
    """Map each arm to its modal digest and one representative call.

    Only records with a recorded digest participate, so a failed or
    digest-less call cannot abort the re-evaluation.
    """
    modal = {}
    for arm in arm_order:
        calls = [(record['call'], recorded_digest(record))
                 for record in records if record['arm'] == arm]
        digests = [digest for _, digest in calls if digest]
        if not digests:
            continue
        digest = Counter(digests).most_common(1)[0][0]
        representative = next(call for call, value in calls if value == digest)
        modal[arm] = (digest, representative)
    return modal


def parse_probe(path):
    events = []
    with path.open('r', encoding='utf-8', errors='replace') as stream:
        for line in stream:
            record = protocol.parse_record(line)
            if record:
                events.append(record)
    return events


def topology(events):
    coverage = protocol.producer_coverage(events)
    premature = []
    for entry in coverage['premature_entries']:
        premature.append({
            'ch': entry.get('ch'), 'idx': entry.get('idx'),
            'valid': f"0x{entry.get('valid', 0):08x}",
            'started': f"0x{entry.get('started', 0):08x}",
        })
    return {
        'gate_entries': coverage['gate_entries'],
        'publish_calls_band0': coverage['publish_calls_band0'],
        'transitions_band0': coverage['transitions_band0'],
        'transition_pointers_band0': coverage['transition_pointers_band0'],
        'entropy_transitions_band0': coverage['entropy_transitions_band0'],
        'waits': len(coverage['waits']),
        'waits_matched': sum(1 for item in coverage['coverage']
                             if item['producer_publish_matched']),
        'timeouts': len(coverage['timeouts']),
        'premature_entries': premature,
    }


def lane_digest_record(output_path, modal_path):
    data, lanes = read_lanes(output_path)
    _, modal_lanes = read_lanes(modal_path)
    return {
        'image_sha256': sha256_bytes(data),
        'lane_domain': {'name': LANE_NAMES, 'bytes_per_lane': WIDTH * HEIGHT,
                        'width': WIDTH, 'height': HEIGHT,
                        'note': 'post-conversion interleaved BGRA byte lanes; '
                                'not internal codec channel planes'},
        'lane_sha256': {name: sha256_bytes(lane)
                        for name, lane in zip(LANE_NAMES, lanes)},
        'diff_vs_modal': {name: lane_diff_stats(lane, modal_lane)
                          for name, lane, modal_lane
                          in zip(LANE_NAMES, lanes, modal_lanes)},
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--ledger', required=True)
    parser.add_argument('--output-root', required=True)
    parser.add_argument('--probe-root', required=True)
    parser.add_argument('--json', required=True)
    args = parser.parse_args()

    ledger_path = Path(args.ledger).resolve()
    output_root = Path(args.output_root).resolve()
    probe_root = Path(args.probe_root).resolve()
    report = json.loads(ledger_path.read_text(encoding='utf-8'))
    records = report['ledger']

    problems = []
    topology_by_call = {}
    outputs_checked = 0
    probes_checked = 0
    for record in records:
        arm = record['arm']
        call = record['call']
        label = f'{arm}/{call:03d}'
        output_path = output_root / arm / f'{call:03d}-1.bgra'
        probe_path = probe_root / arm / f'{call:03d}.probe.log'
        if not output_path.is_file():
            problems.append(f'{ARTIFACT_MISSING}: output {label}')
        else:
            outputs_checked += 1
            if record.get('output_valid'):
                recorded = recorded_digest(record)
                if recorded is None:
                    problems.append(f'output digest missing: {label}')
                else:
                    digest = sha256_file(output_path)
                    if digest != recorded:
                        problems.append(f'output digest mismatch: {label}')
                    record['output']['sha256'] = digest
        if not probe_path.is_file():
            problems.append(f'{ARTIFACT_MISSING}: probe {label}')
            record['gate']['gate_entries'] = 0
            topology_by_call[label] = {'gate_entries': 0}
        else:
            probes_checked += 1
            found = topology(parse_probe(probe_path))
            topology_by_call[label] = found
            for key in ('gate_entries', 'publish_calls_band0', 'transitions_band0',
                        'transition_pointers_band0', 'entropy_transitions_band0',
                        'waits', 'waits_matched', 'timeouts'):
                record['gate'][key] = found[key]
            record['gate']['premature_entries'] = len(found['premature_entries'])

    arms = protocol.summarize(records)
    corrected_verdict, corrected_reasons = protocol.classify(arms, records)
    if problems:
        corrected_verdict = 'INCONCLUSIVE'
        corrected_reasons = corrected_reasons + problems
    registered_verdict, registered_reasons = (
        protocol.classify_registered_publication_rule(arms))

    # Recompute the corrupt calls from retained output digests, arm by arm.
    modal_by_arm = modal_outputs(records, protocol.ARM_ORDER)
    corrupt_by_arm = {}
    for arm, (modal, _) in modal_by_arm.items():
        corrupt_by_arm[arm] = sorted(
            record['call'] for record in records
            if record['arm'] == arm and recorded_digest(record) not in (None, modal))

    corrupt_records = []
    for arm, calls in corrupt_by_arm.items():
        modal_call = modal_by_arm[arm][1]
        modal_path = output_root / arm / f'{modal_call:03d}-1.bgra'
        for call in calls:
            output_path = output_root / arm / f'{call:03d}-1.bgra'
            entry = {
                'arm': arm, 'call': call,
                'premature_entries': topology_by_call.get(
                    f'{arm}/{call:03d}', {}).get('premature_entries', []),
            }
            if output_path.is_file() and modal_path.is_file():
                entry.update(lane_digest_record(output_path, modal_path))
            else:
                entry['lane_identity_status'] = ARTIFACT_MISSING
            corrupt_records.append(entry)

    premature_channels = Counter()
    for record in corrupt_records:
        for entry in record['premature_entries']:
            premature_channels[f"ch{entry['ch']}/idx{entry['idx']}"] += 1

    def distribution(key):
        return {arm: arms[arm].get(key, {}) for arm in protocol.ARM_ORDER}

    checks = report.get('checks') or {}
    evidence = {
        'schema': 'rca160_windows_coverage_correction_v2',
        'issue': 158,
        'pr': 192,
        'diagnostic_only': True,
        'source_ledger_sha256': sha256_file(ledger_path),
        'worker_sha256': checks.get('worker_sha256'),
        'source_head': None,
        'provenance_note': 'worker_sha256 is the diagnostic worker binary digest '
                           'recorded by the ledger; the ledger records no source '
                           'commit, so source_head is null',
        'artifacts': {
            'calls': len(records),
            'output_files_checked': outputs_checked,
            'probe_logs_checked': probes_checked,
            'problems': problems,
        },
        'registered_rule': {
            'expected_publishes_band0_per_call': protocol.REGISTERED_PUBLISH_BAND0,
            'derivation_error': 'the count covered only the spatial lowpass sites; '
                                'it omitted entropy lowpass and frame-wavelet '
                                'publishers and assumed one publisher call per site',
            'publish_call_counts': distribution('publish_call_counts'),
            'verdict': registered_verdict,
            'reasons': registered_reasons,
        },
        'corrected_rule': {
            'invariants': [
                'exactly %d gate entries per call' % protocol.EXPECTED_GATE_ENTRIES,
                'every real wait matched to a same-IMAGE band-0 invalid->valid '
                'transition inside the wait window',
                'no gate timeout',
                'no missing per-call probe log',
            ],
            'publication_count_is_descriptive': True,
            'verdict': corrected_verdict,
            'reasons': corrected_reasons,
        },
        'topology': {
            'gate_entries_per_call': {
                arm: dict(Counter(
                    topology_by_call[f"{record['arm']}/{record['call']:03d}"]
                    .get('gate_entries') for record in records
                    if record['arm'] == arm))
                for arm in protocol.ARM_ORDER},
            'publish_call_counts': distribution('publish_call_counts'),
            'transition_counts': distribution('transition_counts'),
            'transition_pointer_counts': distribution('transition_pointer_counts'),
            'entropy_transitions_band0': {
                arm: arms[arm].get('entropy_transitions_band0') for arm in protocol.ARM_ORDER},
            'waits': {arm: sum(
                topology_by_call[f"{record['arm']}/{record['call']:03d}"]
                .get('waits', 0) for record in records if record['arm'] == arm)
                for arm in protocol.ARM_ORDER},
            'waits_matched': {arm: sum(
                topology_by_call[f"{record['arm']}/{record['call']:03d}"]
                .get('waits_matched', 0) for record in records if record['arm'] == arm)
                for arm in protocol.ARM_ORDER},
            'gate_timeouts': {arm: sum(
                topology_by_call[f"{record['arm']}/{record['call']:03d}"]
                .get('timeouts', 0) for record in records if record['arm'] == arm)
                for arm in protocol.ARM_ORDER},
            'premature_entries_by_channel_idx': {
                arm: dict(Counter(
                    f"ch{e.get('ch')}/idx{e.get('idx')}"
                    for record in records if record['arm'] == arm
                    for e in topology_by_call.get(
                        f"{record['arm']}/{record['call']:03d}", {}
                    ).get('premature_entries', [])))
                for arm in protocol.ARM_ORDER},
        },
        'channel_identity': {
            'status': 'CHANNEL_IDENTITY_UNAVAILABLE',
            'reason': 'retained artifacts are converted BGRA outputs and probe '
                      'events; no internal ch0/ch1/ch2 lowpass coefficient-plane '
                      'files were retained, so codec channel identity cannot be '
                      'reconstructed exactly from the converted output',
            'premature_entry_channels_by_corrupt_call': dict(premature_channels),
            'linux_characterized_states': {
                'ch1': {'unchanged_output_lane': 'B',
                        'max_abs_by_lane': {'B': 0, 'G': 79, 'R': 134}},
                'ch2': {'unchanged_output_lane': 'R',
                        'max_abs_by_lane': {'B': 237, 'G': 49, 'R': 0}},
            },
            'reconciliation': 'all five Windows corrupt outputs change B, G and R '
                              'and leave A identical; no color lane is unchanged, '
                              'so neither characterized Linux chroma state matches. '
                              'Every corrupt call had a premature ch0/index1 entry '
                              '(AP/37 also ch2/index1). The cross-host statement '
                              'remains the same admission window, not the same '
                              'corruption state.',
        },
        'corrupt_calls': corrupt_records,
    }
    Path(args.json).write_text(json.dumps(evidence, indent=1), encoding='utf-8')
    print(json.dumps({
        'registered_rule_verdict': registered_verdict,
        'corrected_rule_verdict': corrected_verdict,
        'corrected_rule_reasons': corrected_reasons,
        'corrupt_calls': {arm: calls for arm, calls in corrupt_by_arm.items()},
        'channel_identity': evidence['channel_identity']['status'],
    }, indent=1))
    return 0 if corrected_verdict == 'CROSS_HOST_CONFIRMATION_PASS' else 1


if __name__ == '__main__':
    sys.exit(main())
