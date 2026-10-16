#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Repeat the real SDK row diagnostic; publish only portable evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--asan-probe', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    runs = []
    for _ in range(5):
        output = subprocess.run([str(args.probe.resolve())], check=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout
        # Canonicalize line endings so the checksum is independently repeatable.
        result = json.loads(output)
        assert result['result'] == 'PASS' and result['case_count'] == 720
        runs.append(sha(json.dumps(result, sort_keys=True, separators=(',', ':')).encode()))
    assert len(set(runs)) == 1, 'fresh-process fixed-guard results differ'
    root = Path(__file__).resolve().parents[1]
    paths = ['upstream/Codec/spatial.c', 'upstream/Codec/filter.h',
             'upstream/Codec/config.h', 'tests/test_small_chroma_oracle.cpp',
             'tests/collect_small_chroma_evidence.py']
    report = {
        'copyright': 'Copyright (c) 2026 HyperCine Contributors.',
        'license': 'MIT OR Apache-2.0',
        'schema': 1,
        'outcome': 'B',
        'width16_oracle': 'UPSTREAM_ORACLE_UNDEFINED',
        'case_count_per_process': result['case_count'],
        'fresh_process_count': len(runs),
        'canonical_complete_rows_sha256': runs[0],
        'all_fixed_guard_processes_equal': True,
        'sources': [{'path': path, 'sha256': sha((root/path).read_bytes())} for path in paths],
        'flat512_boundary': [
            {'row_width': width, 'guards_and_highpass0': [
                {'guard': row['guard'], 'highpass0': row['high'][0]}
                for row in result['cases'] if row['width'] == width and row['pattern'] == 0]}
            for width in sorted({row['width'] for row in result['cases']})],
        'width16_flat_complete_rows': [row for row in result['cases']
                                      if row['width'] == 16 and row['pattern'] == 0],
        'sanitizer': {'status': 'NOT_RUN'},
        'scope': 'Direct pristine SDK horizontal-row function; no full-frame encoder heap experiment or CUDA execution',
    }
    if args.asan_probe:
        guarded = subprocess.run([str(args.asan_probe.resolve())], check=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        guarded_hash = sha(json.dumps(json.loads(guarded.stdout), sort_keys=True,
                                     separators=(',', ':')).encode())
        assert guarded_hash == runs[0], 'instrumented guarded rows differ'
        probe = subprocess.run([str(args.asan_probe.resolve()), '--asan-prefix'],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        diagnostic = (probe.stdout + probe.stderr).decode(errors='replace')
        assert probe.returncode != 0, 'expected ASan failure was absent'
        assert 'ERROR: AddressSanitizer: use-after-poison' in diagnostic
        assert 'FilterHorizontalRow16s' in diagnostic and 'spatial.c:' in diagnostic
        # No raw stacks, executable paths, process IDs or addresses are published.
        report['sanitizer'] = {
            'status': 'EXPECTED_UPSTREAM_PREFIX_READ_DETECTED',
            'allocated_guard_cases': '720 PASS; complete rows identical to uninstrumented probe',
            'kind': 'use-after-poison', 'function': 'FilterHorizontalRow16s',
            'source': 'upstream/Codec/spatial.c', 'exit_code': probe.returncode,
            'instrumentation': 'MSVC AddressSanitizer on pristine spatial.c; 16-byte prefix explicitly poisoned',
            'limitations': 'Logical-row boundary experiment; not a full SDK heap sanitizer qualification',
        }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print('PASS: 5 fresh processes, 720 cases each; sanitized evidence written')


if __name__ == '__main__':
    main()
