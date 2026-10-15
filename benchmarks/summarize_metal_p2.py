#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Validate identical P1/P2 benchmark inputs, state, bytes and resident counters."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics


def stats(values):
    ordered = sorted(values)
    mean = statistics.mean(values)
    return {'count': len(values), 'mean_ms': mean,
            'median_ms': statistics.median(values),
            'p95_ms': ordered[math.ceil(.95 * len(values)) - 1],
            'stddev_population_ms': statistics.pstdev(values),
            'min_ms': ordered[0], 'max_ms': ordered[-1],
            'fps': 1000 / mean, 'megapixels_per_second': 2073.6 / mean}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--p1', type=Path, nargs=2, required=True)
    parser.add_argument('--p2', type=Path, nargs=2, required=True)
    parser.add_argument('--windows-baseline', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    files = args.p1 + args.p2
    documents = [json.loads(p.read_text()) for p in files]
    expected = {(s, f) for s, n in enumerate((32, 32, 32, 16, 120)) for f in range(n)}
    indexed = []
    for doc in documents:
        assert doc['packet_equality'] == 'PASS'
        rows = {(r['scenario'], r['frame']): r for r in doc['runs']}
        assert len(rows) == len(doc['runs']) and set(rows) == expected
        for r in rows.values():
            assert r['device'] == 'Apple M2 Pro'
            for key in ('input_sha256', 'packet_sha256'):
                assert len(r[key]) == 64 and int(r[key], 16) >= 0
            for key, val in r.items():
                if isinstance(val, (float, int)):
                    assert math.isfinite(val) and val >= 0, (key, val)
        indexed.append(rows)
    for key in sorted(expected):
        for name in ('input_sha256', 'packet_sha256', 'sample_bytes', 'limiter'):
            assert len({rows[key][name] for rows in indexed}) == 1, (key, name)
    for rows in indexed[2:]:
        for (scenario, frame), r in rows.items():
            assert r['buffer_allocations'] == (42 if frame == 0 else 0)
            for name, val in {'command_buffers': 1, 'waits': 1, 'upload_copies': 1,
                              'readback_copies': 30, 'uploaded_bytes': 8294400,
                              'readback_bytes': 8294400, 'peak_buffer_bytes': 31622400}.items():
                assert r[name] == val, (scenario, frame, name, r[name], val)
    out = {'status': 'PASS', 'schema_version': 1, 'same_input_state_full_packet_sha':
           {'cases_per_process': len(expected), 'processes': 4, 'status': 'PASS'},
           'measurement_order': 'P1-before, P2-first, P2-second, P1-after; CPU/Metal alternating within each',
           'percentile_method': 'nearest-rank; population standard deviation', 'scenarios': []}
    for scenario, name in enumerate(('flat', 'gradient', 'noise', '16-frame-stateful', '120-frame-stateful')):
        # Two startup samples excluded for repeated-pattern warm distributions.
        # Sequence summaries retain every frame, with cold frame separately available.
        start = 2 if scenario < 3 else 0
        item = {'name': name, 'excluded_startup_frames': start, 'processes': []}
        for path, rows in zip(files, indexed):
            selected = [r for (s, f), r in sorted(rows.items()) if s == scenario and f >= start]
            process = {'file': path.name, 'cpu': stats([r['cpu_ms'] for r in selected]),
                       'metal': stats([r['metal_ms'] for r in selected]),
                       'cold_first_cpu_ms': rows[(scenario, 0)]['cpu_ms'],
                       'cold_first_metal_ms': rows[(scenario, 0)]['metal_ms']}
            process['cpu_stage_mean_ms'] = {k: statistics.mean(r[k] for r in selected)
                                            for k in ('cpu_conversion_ms', 'cpu_wavelet_ms')}
            if 'host_remaining_ms' in selected[0]:
                process['p1_host_quant_entropy_framing_orchestration_ms'] = statistics.mean(r['host_remaining_ms'] for r in selected)
            if 'batch_gpu_ms' in selected[0]:
                process['stage_mean_ms'] = {k: statistics.mean([r[k] for r in selected]) for k in (
                    'cpu_conversion_ms', 'cpu_wavelet_ms', 'batch_gpu_ms', 'allocation_ms',
                    'upload_ms', 'readback_ms', 'submit_ms', 'wait_ms', 'quant_ms',
                    'coefficient_hash_ms', 'entropy_ms', 'framing_ms')}
            item['processes'].append(process)
        p1 = statistics.mean(p['metal']['median_ms'] for p in item['processes'][:2])
        p2 = statistics.mean(p['metal']['median_ms'] for p in item['processes'][2:])
        item['mean_process_median_metal_ms'] = {'p1': p1, 'p2': p2}
        item['p2_reduction_percent'] = 100 * (1 - p2 / p1)
        out['scenarios'].append(item)
    out['artifacts'] = [{'file': p.name, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest(),
                         'peak_process_rss_bytes': d['peak_rss_bytes']} for p, d in zip(files, documents)]
    out['windows_reference'] = {'status': 'NOT_COMPARABLE', 'reason':
        'Different host, compiler, SDK execution and input workload; no cross-host speedup/regression claim',
        'path': str(args.windows_baseline),
        'sha256': hashlib.sha256(args.windows_baseline.read_bytes()).hexdigest(),
        'recorded_1080p': json.loads(args.windows_baseline.read_text())['benchmarks'][0]}
    args.output.write_text(json.dumps(out, indent=2) + '\n')
    print('PASS input/state/packet SHA across four processes; 232 cases/process; resident resource contracts')


if __name__ == '__main__':
    main()
