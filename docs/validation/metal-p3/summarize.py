#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Validate same-input/state packet identities and summarize native P2/P3 ABBA trials."""
import json
import math
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parent
NAMES = ['flat', 'gradient', 'noise', '16-frame-stateful', '120-frame-stateful']

def load(name):
    data = json.loads((ROOT / name).read_text())
    assert data['packet_equality'] == 'PASS'
    rows = data['runs']
    assert len(rows) == 232
    assert len({(r['scenario'], r['frame']) for r in rows}) == 232
    return data

def identity(row):
    return tuple(row[k] for k in ['scenario', 'frame', 'input_sha256', 'packet_sha256', 'sample_bytes', 'limiter'])

def stats(rows, key):
    vals = sorted(r[key] for r in rows)
    avg = statistics.mean(vals)
    return dict(n=len(vals), median_ms=statistics.median(vals),
                p95_ms=vals[math.ceil(len(vals)*0.95)-1], avg_ms=avg,
                min_ms=vals[0], max_ms=vals[-1], fps=1000/avg,
                megapixels_per_sec=2.0736*1000/avg)

def main():
    # A/B/B/A, repeated twice. Each process has 30 warm fixed-pattern pairs
    # and complete cold-start 16/120-frame sequences; no sequence-state resets.
    files = ['p2-abba-1.json', 'p3-abba-1.json', 'p3-abba-2.json', 'p2-abba-2.json',
             'p2-abba-3.json', 'p3-abba-3.json', 'p3-abba-4.json', 'p2-abba-4.json']
    data = [load(f) for f in files]
    initial = load('p2-before.json')
    expected = [identity(r) for r in initial['runs']]
    for f, d in zip(files, data):
        assert [identity(r) for r in d['runs']] == expected, f + ': input/state/packet mismatch'
    old = [d for f, d in zip(files, data) if f.startswith('p2-')]
    new = [d for f, d in zip(files, data) if f.startswith('p3-')]
    p1 = [load('p1-before.json'), load('p1-after.json')]
    for d in p1:
        assert [identity(r) for r in d['runs']] == expected, 'P1 input/state/packet mismatch'
    report = {'method': 'same host native P2/P3 ABBA/ABBA; production benchmark unchanged',
              'packet_identity_cases_per_process': len(expected), 'packet_identity': 'PASS',
              'process_order': files, 'benchmarks': [],
              'peak_rss_bytes': {f: d['peak_rss_bytes'] for f, d in zip(files, data)}}
    for scenario, name in enumerate(NAMES):
        def select(ds):
            return [r for d in ds for r in d['runs'] if r['scenario'] == scenario
                    and (scenario >= 3 or r['frame'] >= 2)]
        a, b = select(old), select(new)
        aa, bb = stats(a, 'metal_ms'), stats(b, 'metal_ms')
        entry = {'name': name, 'p2': aa, 'p3': bb,
                 'median_reduction_percent': 100*(1-bb['median_ms']/aa['median_ms']),
                 'p2_entropy_mean_ms': statistics.mean(r['entropy_ms'] for r in a),
                 'p3_entropy_mean_ms': statistics.mean(r['entropy_ms'] for r in b)}
        for tag, rows in [('p2', a), ('p3', b)]:
            entry[tag]['sample_bytes_mean'] = statistics.mean(r['sample_bytes'] for r in rows)
            entry[tag]['host_stage_mean_ms'] = {key: statistics.mean(r[key] for r in rows)
                for key in ['quant_ms', 'coefficient_hash_ms', 'entropy_ms', 'framing_ms']}
            cold = [r for d in (old if tag == 'p2' else new) for r in d['runs']
                    if r['scenario'] == scenario and r['frame'] == 0]
            entry[tag]['cold_first_frame'] = stats(cold, 'metal_ms')
        entry['p1'] = stats(select(p1), 'metal_ms')
        entry['per_process_median_ms'] = {f: stats(select([d]), 'metal_ms')['median_ms']
                                          for f, d in zip(files, data)}
        report['benchmarks'].append(entry)
    windows = json.loads((ROOT.parents[2] / 'benchmarks/results/reference_windows_msvc.json').read_text())
    report['windows_baseline'] = {'comparison': 'NOT_COMPARABLE: different host, SDK, threads and input generator',
                                  'reference_1080p': windows['benchmarks'][0]}
    (ROOT / 'benchmark-summary.json').write_text(json.dumps(report, indent=2)+'\n')
    for row in report['benchmarks']:
        print(row['name'], row['p2']['median_ms'], row['p3']['median_ms'], row['median_reduction_percent'])

if __name__ == '__main__':
    main()
