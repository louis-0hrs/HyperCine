#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Bounded fresh-process SDK observations; never retries or selects a best result."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess
import tempfile


def native_json(text):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('duplicate native JSON key')
            result[key] = value
        return result
    def reject_constant(value):
        raise ValueError('non-finite native JSON number')
    return json.loads(text, object_pairs_hook=unique, parse_constant=reject_constant)


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


BASELINE = '11574d0295771edccadd17af14af74a539f924c7'
THREAD_LABEL = 'INCONCLUSIVE: public metadata request is not worker-count telemetry'
HEADER_LABEL = 'CFHDDecoder.h @version 1.0.0 (header label only)'
STAGES = {'arguments', 'affinity', 'input', 'open', 'prepare', 'metadata', 'decode', 'save',
          'malformed_probe_json'}
INTEGER_BOUNDS = {
    'requested_threads': (1, 64), 'width': (1, 65536), 'height': (1, 65536),
    'pitch': (1, 2**31 - 1), 'sdk_error': (-(2**31), 2**31 - 1),
    'decoder_sdk_version_packed': (1, 2**31 - 1),
    'decoder_sdk_version_query_status': (-(2**31), 2**31 - 1),
}


def mask(value):
    return (isinstance(value, str) and re.fullmatch(r'[0-9a-f]{1,16}', value) is not None
            and int(value, 16) > 0)


def sanitize_inventory(raw):
    if (not isinstance(raw, dict) or raw.get('status') != 'PASS'
            or not mask(raw.get('process_affinity')) or not isinstance(raw.get('cpu_isa'), dict)):
        raise ValueError('invalid native platform inventory')
    result = dict(status='PASS', process_affinity=format(int(raw['process_affinity'], 16), 'x'),
                  cpu_isa={key: raw['cpu_isa'][key] for key in
                           ('sse2', 'avx2_os_enabled', 'avx512f_os_enabled')
                           if type(raw['cpu_isa'].get(key)) is bool},
                  topology_status='INCONCLUSIVE', physical_cores=[])
    groups = raw.get('processor_groups')
    if type(groups) is int and 1 <= groups <= 64:
        result['processor_groups'] = groups
    cores = raw.get('physical_cores', [])
    if (isinstance(cores, list) and len(cores) <= 64
            and all(isinstance(c, dict) and mask(c.get('logical_mask'))
                    and type(c.get('smt')) is bool for c in cores)):
        result['physical_cores'] = [dict(logical_mask=format(int(c['logical_mask'], 16), 'x'),
                                         smt=c['smt']) for c in cores]
        if raw.get('topology_status') == 'PASS' and result.get('processor_groups') == 1:
            result['topology_status'] = 'PASS'
    return result


def sanitize_observation(raw):
    # No free-form subprocess strings or nested values cross the export boundary.
    result = {}
    for key, expected in (('thread_observation', THREAD_LABEL), ('sdk_api_header_version', HEADER_LABEL),
                          ('sdk_source_baseline', BASELINE), ('pixel_format', 'YU64')):
        if raw.get(key) == expected:
            result[key] = expected
    for key, (lower, upper) in INTEGER_BOUNDS.items():
        value = raw.get(key)
        if type(value) is int and lower <= value <= upper:
            result[key] = value
    if 'observed_decoder_threads' in raw and raw['observed_decoder_threads'] is None:
        result['observed_decoder_threads'] = None
    if mask(raw.get('process_affinity')):
        result['process_affinity'] = raw['process_affinity']
    if isinstance(raw.get('error_stage'), str) and raw['error_stage'] in STAGES:
        result['error_stage'] = raw['error_stage']
    return result


def valid_observation(observation, cell):
    return (isinstance(observation, dict) and observation.get('status') == 'PASS'
            and all(type(observation.get(key)) is int for key in
                    ('requested_threads', 'width', 'height', 'pitch',
                     'decoder_sdk_version_packed', 'decoder_sdk_version_query_status'))
            and type(observation.get('requested_threads')) is int
            and observation['requested_threads'] == cell['threads']
            and observation.get('process_affinity') == cell['affinity']
            and observation.get('pixel_format') == 'YU64'
            and observation.get('width') == 1920 and observation.get('height') == 1080
            and observation.get('pitch') == 7680
            and 'observed_decoder_threads' in observation
            and observation['observed_decoder_threads'] is None
            and observation.get('thread_observation') == THREAD_LABEL
            and observation.get('sdk_source_baseline') == BASELINE
            and observation.get('sdk_api_header_version') == HEADER_LABEL
            and type(observation.get('decoder_sdk_version_packed')) is int
            and 0 < observation['decoder_sdk_version_packed'] < 2**31
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
    inventory = sanitize_inventory(native_json(inventory_call.stdout))
    available = int(inventory['process_affinity'], 16)
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
                        observation = native_json(child.stdout)
                    except (ValueError, TypeError):
                        observation = dict(status='FAIL', error_stage='malformed_probe_json')
                    if not isinstance(observation, dict):
                        observation = dict(status='FAIL', error_stage='malformed_probe_json')
                    # Only known fields: never publish paths/raw process output.
                    row.update(sanitize_observation(observation))
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
    try:
        result = run(args.probe.resolve(), args.sample.resolve(), args.input_sha256, args.timeout)
    except (OSError, ValueError, TypeError, subprocess.SubprocessError):
        # Inventory failures, raw process text and exception paths are private.
        result = dict(copyright='Copyright (c) 2026 HyperCine Contributors.',
                      license='MIT OR Apache-2.0', schema_version=1, status='FAIL',
                      error_stage='input_inventory_or_process_contract', records=[])
    with args.output.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    return (1 if result['status'] == 'FAIL' else 77 if result['status'].startswith('BLOCKED')
            else 2 if result['status'] == 'INCONCLUSIVE' else 0)


if __name__ == '__main__':
    raise SystemExit(main())
