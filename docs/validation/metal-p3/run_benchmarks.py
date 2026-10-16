#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Run frozen native benchmarks sequentially; correctness suites must be idle."""
import datetime
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent
BINS = {'p1': Path('/private/tmp/hypercine-benchmark-p1-sha'),
        'p2': Path('/private/tmp/hypercine-metal-p2/build/benchmark_codec'),
        'p3': ROOT.parents[2] / 'build/benchmark_codec'}
ORDER = [('p1', 'p1-before.json'),
         ('p2', 'p2-abba-1.json'), ('p3', 'p3-abba-1.json'),
         ('p3', 'p3-abba-2.json'), ('p2', 'p2-abba-2.json'),
         ('p2', 'p2-abba-3.json'), ('p3', 'p3-abba-3.json'),
         ('p3', 'p3-abba-4.json'), ('p2', 'p2-abba-4.json'),
         ('p1', 'p1-after.json')]

def now(): return datetime.datetime.now(datetime.timezone.utc).isoformat()

def main():
    report = {'started_utc': now(), 'order': [],
              'binary_sha256': {k: hashlib.sha256(p.read_bytes()).hexdigest() for k,p in BINS.items()},
              'p3_source_sha': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
              'overlap': 'No task-launched builds/tests overlap; uncontrolled background activity remains possible'}
    for variant, name in ORDER:
        start = now()
        with (ROOT / name).open('w') as out:
            subprocess.run([str(BINS[variant])], stdout=out, check=True)
        data = json.loads((ROOT / name).read_text())
        if data.get('packet_equality') != 'PASS':
            raise ValueError(name + ' failed CPU/Metal packet equality')
        report['order'].append({'file': name, 'started_utc': start, 'finished_utc': now()})
        print('Completed', name, flush=True)
    report['finished_utc'] = now()
    (ROOT / 'benchmark-provenance.json').write_text(json.dumps(report, indent=2)+'\n')
    subprocess.run(['python3', str(ROOT / 'summarize.py')], check=True)

if __name__ == '__main__': main()
