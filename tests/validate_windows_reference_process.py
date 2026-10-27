#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Bounded fresh-process SDK observations; never retries or selects a best result."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def matrix(available, physical_cores=()):
    if available <= 0:
        raise ValueError('empty process affinity')
    first = available & -available
    remaining = available & ~first
    # Thread-request and affinity experiments are separate axes.
    masks = [(first, 'one_logical_processor')]
    cores = [int(core['logical_mask'], 16) & available for core in physical_cores]
    cores = [core for core in cores if core]
    if len(cores) >= 2:
        masks.append(((cores[0] & -cores[0]) | (cores[1] & -cores[1]), 'two_physical_cores_one_logical_each'))
    # bin/count also supports Python 3.8 used by portable CI.
    smt = next((core for core in cores if bin(core).count('1') >= 2), None)
    if smt:
        one = smt & -smt
        rest = smt & ~one
        masks.append((one | (rest & -rest), 'one_physical_core_two_smt_siblings'))
    if len(masks) == 1 and remaining:
        masks.append((remaining & -remaining, 'second_logical_processor_topology_unqualified'))
    return ([dict(axis='thread_request', threads=n, affinity=format(available, 'x'), affinity_kind='fixed_available_mask')
             for n in (1, 2, 4, 8)] +
            [dict(axis='affinity', threads=2, affinity=format(mask, 'x'), affinity_kind=kind) for mask, kind in masks])


def classify(records, cells=None):
    if not records:
        return 'INCONCLUSIVE'
    if any(row['status'] == 'FAIL' for row in records):
        return 'FAIL'
    if any(row['status'] != 'PASS' for row in records):
        return 'INCONCLUSIVE'
    groups = {}
    for row in records:
        key = row['axis'], row['threads'], row['affinity']
        groups.setdefault(key, set()).add(row['output_sha256'])
    counts = {}
    for row in records:
        key = row['axis'], row['threads'], row['affinity']
        counts[key] = counts.get(key, 0) + 1
    if any(count != 3 for count in counts.values()):
        return 'INCONCLUSIVE'
    if cells is not None and set(groups) != {(c['axis'], c['threads'], c['affinity']) for c in cells}:
        return 'INCONCLUSIVE'
    if cells is not None:
        identities = [(r['axis'], r['threads'], r['affinity'], r.get('repeat')) for r in records]
        expected = {(c['axis'], c['threads'], c['affinity'], repeat) for c in cells for repeat in range(3)}
        if len(set(identities)) != len(identities) or set(identities) != expected:
            return 'INCONCLUSIVE'
    if any(len(hashes) > 1 for hashes in groups.values()):
        return 'BLOCKED_NONDETERMINISTIC_REFERENCE'
    if len({row['output_sha256'] for row in records}) > 1:
        return 'BLOCKED_CONFIGURATION_DEPENDENT_REFERENCE'
    return 'REPEATABLE_OBSERVATION_ONLY'


def valid_observation(observation, cell):
    return (isinstance(observation, dict) and observation.get('status') == 'PASS'
            and type(observation.get('requested_threads')) is int
            and observation['requested_threads'] == cell['threads']
            and observation.get('process_affinity') == cell['affinity']
            and observation.get('pixel_format') == 'YU64'
            and observation.get('width') == 1920 and observation.get('height') == 1080
            and observation.get('pitch') == 7680
            and 'observed_decoder_threads' in observation
            and observation['observed_decoder_threads'] is None
            and isinstance(observation.get('thread_observation'), str)
            and observation['thread_observation'].startswith('INCONCLUSIVE:')
            and observation.get('sdk_source_baseline') == '11574d0295771edccadd17af14af74a539f924c7'
            and isinstance(observation.get('sdk_api_header_version'), str)
            and type(observation.get('decoder_sdk_version_packed')) is int
            and observation['decoder_sdk_version_packed'] > 0
            and type(observation.get('decoder_sdk_version_query_status')) is int
            and observation['decoder_sdk_version_query_status'] == 0)


def run(probe, sample, expected_sha, timeout=30):
    if sha(sample) != expected_sha:
        raise ValueError('input SHA-256 does not match the predeclared sample')
    build_sha = sha(probe)
    inventory_call = subprocess.run([str(probe), '--inventory'], capture_output=True,
                                    text=True, timeout=timeout)
    if inventory_call.returncode != 0:
        raise ValueError('native platform inventory failed')
    inventory = json.loads(inventory_call.stdout)
    available = int(inventory['process_affinity'], 16)
    if inventory.get('status') != 'PASS' or not isinstance(inventory.get('cpu_isa'), dict):
        raise ValueError('missing native platform inventory evidence')
    qualified_topology = inventory.get('topology_status') == 'PASS' and inventory.get('processor_groups') == 1
    cells = matrix(available, inventory.get('physical_cores', []) if qualified_topology else ())
    records = []
    # Fixed before decoding: three fresh processes per cell, maximum 21 decodes.
    with tempfile.TemporaryDirectory(prefix='hypercine-sdk-process-') as temporary:
        for cell in cells:
            for repeat in range(3):
                output = Path(temporary) / f'{len(records)}.yu64'
                row = dict(cell, repeat=repeat, input_sha256=expected_sha,
                           decoder_build_sha256=build_sha, status='FAIL', exit_code=None,
                           timed_out=False, cpu_isa=inventory['cpu_isa'])
                try:
                    child = subprocess.run([str(probe), str(sample), str(output),
                                            str(cell['threads']), cell['affinity']],
                                           capture_output=True, text=True, timeout=timeout)
                    row['exit_code'] = child.returncode
                    try:
                        observation = json.loads(child.stdout)
                    except (ValueError, TypeError):
                        observation = dict(status='FAIL', error_stage='malformed_probe_json')
                    if not isinstance(observation, dict):
                        observation = dict(status='FAIL', error_stage='malformed_probe_json')
                    # Only known fields: never publish paths/raw process output.
                    fields = ('requested_threads', 'observed_decoder_threads', 'thread_observation',
                              'process_affinity', 'pixel_format', 'width', 'height', 'pitch',
                              'sdk_api_header_version', 'sdk_source_baseline', 'error_stage', 'sdk_error',
                              'decoder_sdk_version_packed', 'decoder_sdk_version_query_status')
                    row.update({key: observation[key] for key in fields if key in observation})
                    valid = (child.returncode == 0 and valid_observation(observation, cell) and output.is_file()
                             and output.stat().st_size == 7680 * 1080)
                    if valid:
                        row['output_sha256'] = sha(output)
                        row['status'] = 'PASS'
                    else:
                        row.setdefault('error_stage', 'probe_contract')
                except subprocess.TimeoutExpired:
                    row.update(timed_out=True, error_stage='process_timeout')
                except OSError:
                    row['error_stage'] = 'process_launch_or_output'
                records.append(row)
    if sha(sample) != expected_sha or sha(probe) != build_sha:
        records.append(dict(status='FAIL', error_stage='input_or_executable_changed'))
    return dict(copyright='Copyright (c) 2026 HyperCine Contributors.', license='MIT OR Apache-2.0',
                schema_version=1, status=classify(records, cells),
                scope='Repeatability observations; no pixel correctness or decoder RCA claim',
                preregistered_repeats=3, preregistered_cells=cells,
                affinity_topology_status=inventory.get('topology_status', 'INCONCLUSIVE'),
                topology=inventory, records=records)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--sample', type=Path, required=True)
    parser.add_argument('--input-sha256', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=30)
    args = parser.parse_args()
    if args.timeout <= 0 or args.timeout > 120:
        parser.error('timeout must be in (0,120] seconds')
    result = run(args.probe.resolve(), args.sample.resolve(), args.input_sha256, args.timeout)
    with args.output.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    return (1 if result['status'] == 'FAIL' else 77 if result['status'].startswith('BLOCKED')
            else 2 if result['status'] == 'INCONCLUSIVE' else 0)


if __name__ == '__main__':
    raise SystemExit(main())
