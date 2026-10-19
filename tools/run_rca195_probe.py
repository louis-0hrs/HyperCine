#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Run the RCA-195 Milestone-1 completion arms and emit a ledger.

Each arm is one invocation of the private probe harness per call, so every
record belongs to exactly one decode. The harness prints one JSON line and the
probe writes one log line set; both are reduced here.

Arms (fixed order, fixed targets, no reshuffling):

  A   control            no skip, no withhold
  B   skip ch1/index2    the temporal producer of wavelet[1] band 0
  C   skip ch0/index2    same producer on the luma channel
  D   skip ch2/index2    same producer on the second chroma channel
  E   skip ch1/index1    the consumer site that #160 measured

Classification per call, in this order:
  HANG           no record within the per-call timeout
  NO_OUTPUT      SDK reported an error
  FALSE_OKAY     SDK reported OKAY and the digest differs from the modal
                 control digest, with the queue reset showing an executed=0 skip
  OK             SDK OKAY and digest equals the modal control digest

This tool never decides that the decoder is wrong; it only records. The verdict
is computed from the registered rules and printed with its reason list.
"""
import argparse
import collections
import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

ARMS = (
    ('A', None),
    ('B', '1:2'),
    ('C', '0:2'),
    ('D', '2:2'),
    ('E', '1:1'),
)

RESET = re.compile(
    r'QUEUE_RESET num_entries=(\d+) free_entry=(\d+) next_entry=(\d+) '
    r'admitted=(\d+) unadmitted=(\d+) withheld=(\d+) skipped=(\d+) '
    r'claimed=(\d+) executed=(\d+)')
SKIP = re.compile(r'SKIP ch=(\d+) idx=(\d+) entry_consumed=1 executed=0')


def run_call(harmness, packet, cpu_max, resolution, outdir, tag, skip, timeout):
    env = dict(os.environ)
    env['RCA195_PROBE_LOG'] = str(Path(outdir) / ('probe_%s.log' % tag))
    if skip:
        env['RCA195_SKIP'] = skip
    else:
        env.pop('RCA195_SKIP', None)
    env.pop('RCA195_WITHHOLD', None)

    cmd = ['taskset', '-c', '0', harmness, str(packet), str(cpu_max),
           resolution, str(outdir), tag]
    try:
        done = subprocess.run(cmd, env=env, capture_output=True, text=True,
                              timeout=timeout)
    except subprocess.TimeoutExpired:
        return {'tag': tag, 'status': 'HANG'}

    record = None
    for line in done.stdout.splitlines():
        line = line.strip()
        if line.startswith('{'):
            try:
                record = json.loads(line)
            except ValueError:
                continue
    if record is None:
        return {'tag': tag, 'status': 'NO_RECORD', 'stderr': done.stderr[:200]}

    log_path = Path(env['RCA195_PROBE_LOG'])
    queue = None
    skips = []
    if log_path.is_file():
        for line in log_path.read_text(encoding='utf-8', errors='replace').splitlines():
            m = RESET.search(line)
            if m:
                queue = {
                    'num_entries': int(m.group(1)),
                    'free_entry': int(m.group(2)),
                    'next_entry': int(m.group(3)),
                    'withheld': int(m.group(6)),
                    'skipped': int(m.group(7)),
                    'claimed': int(m.group(8)),
                    'executed': int(m.group(9)),
                }
            s = SKIP.search(line)
            if s:
                skips.append({'ch': int(s.group(1)), 'idx': int(s.group(2))})

    record['queue'] = queue
    record['skips'] = skips
    record['status'] = 'OK' if record.get('sdk_ok') else 'NO_OUTPUT'
    return record


def classify(records, control_digest):
    """Label each call. The witness is the claim/execute counter, not the hash.

    Registered in the v2 pre-registration: natural control calls and induced
    calls reach the same byte states, so a digest match is corroboration only.
    A call is only "dropped" when the worker claimed a transform out of the queue
    and never executed it.
    """
    for record in records:
        if record.get('status') in ('HANG', 'NO_RECORD'):
            continue
        if not record.get('sdk_ok'):
            record['status'] = 'NO_OUTPUT'
            continue

        queue = record.get('queue') or {}
        claimed = queue.get('claimed', 0)
        executed = queue.get('executed', 0)
        dropped = claimed > executed
        differs = record.get('sha256') != control_digest

        if dropped and differs:
            record['status'] = 'DROPPED_FALSE_OKAY'
        elif dropped:
            record['status'] = 'DROPPED_IDENTICAL'
        elif differs:
            record['status'] = 'NATURAL_VARIANT'
        else:
            record['status'] = 'OK'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--harness', required=True)
    parser.add_argument('--packet', required=True)
    parser.add_argument('--outdir', required=True)
    parser.add_argument('--calls', type=int, default=30)
    parser.add_argument('--cpu-max', type=int, default=4)
    parser.add_argument('--resolution', default='HALF')
    parser.add_argument('--timeout', type=float, default=60.0)
    parser.add_argument('--json', required=True)
    parser.add_argument('--arms', default='A,B,C,D,E',
                        help='comma-separated subset of A,B,C,D,E')
    args = parser.parse_args(argv)

    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    ledger = {'arms': {}, 'packet': str(args.packet),
              'resolution': args.resolution, 'cpu_max': args.cpu_max,
              'calls_per_arm': args.calls}
    control_records = []

    wanted = {a.strip().upper() for a in args.arms.split(',') if a.strip()}
    for name, skip in ARMS:
        if name not in wanted:
            continue
        arm_dir = outdir / ('arm_%s' % name)
        arm_dir.mkdir(parents=True, exist_ok=True)
        records = []
        for index in range(1, args.calls + 1):
            tag = '%s%03d' % (name, index)
            record = run_call(args.harness, args.packet, args.cpu_max,
                              args.resolution, arm_dir, tag, skip, args.timeout)
            records.append(record)
        ledger['arms'][name] = {'skip': skip, 'records': records}
        if name == 'A':
            control_records = records

    digests = [r['sha256'] for r in control_records if r.get('sha256')]
    modal = collections.Counter(digests).most_common(1)[0][0] if digests else None
    ledger['control_modal_sha256'] = modal

    for name, arm in ledger['arms'].items():
        classify(arm['records'], modal)
        counts = collections.Counter(r['status'] for r in arm['records'])
        arm['counts'] = dict(counts)
        arm['distinct_outputs'] = len({r['sha256'] for r in arm['records']
                                       if r.get('sha256')})
        arm['queue_reset_all_zero'] = all(
            (r.get('queue') or {}).get('num_entries', 0) == 0
            for r in arm['records'] if r.get('queue'))

    # Verdict rules from docs/validation/RCA195_PROBE_PREREGISTRATION_V2.md.
    reasons = []
    a = ledger['arms']['A']
    induced = tuple(n for n in ('B', 'C', 'D', 'E') if n in ledger['arms'])

    control_dropped = sum(
        1 for r in a['records']
        if r.get('status') in ('DROPPED_FALSE_OKAY', 'DROPPED_IDENTICAL'))
    if control_dropped:
        reasons.append('control arm already shows %d dropped calls, so the '
                       'effect is not attributable to the induction'
                       % control_dropped)

    digests = [r['sha256'] for r in a['records'] if r.get('sha256')]
    if digests:
        modal_count = collections.Counter(digests).most_common(1)[0][1]
        if modal_count < 25:
            reasons.append('control arm is too scattered to serve as a mode '
                           '(%d/%d on the modal digest)' % (modal_count, len(digests)))
    else:
        reasons.append('control arm produced no digests')

    evidence = []
    for name in induced:
        arm = ledger['arms'][name]
        hits = [r for r in arm['records']
                if r.get('status') == 'DROPPED_FALSE_OKAY']
        if hits:
            evidence.append({'arm': name, 'calls': [r['tag'] for r in hits],
                             'skipped': [(r.get('queue') or {}).get('skipped', 0)
                                         for r in hits]})
    ledger['false_okay_evidence'] = evidence

    if not evidence:
        reasons.append('no call combined SDK OKAY, a differing digest and '
                       'claimed > executed')

    ledger['control_natural_variant_rate'] = '%d/%d' % (
        a['counts'].get('NATURAL_VARIANT', 0), len(a['records']))

    ledger['verdict'] = 'FALSE_OKAY_DEMONSTRATED' if not reasons else 'INCONCLUSIVE'
    ledger['reasons'] = reasons

    Path(args.json).write_text(json.dumps(ledger, indent=1) + '\n', encoding='utf-8')

    print('control modal: %s' % (modal[:16] + '…' if modal else 'none'))
    for name in ('A', 'B', 'C', 'D', 'E'):
        arm = ledger['arms'][name]
        print('  %s skip=%-6s %s  distinct=%d'
              % (name, arm['skip'] or '-', arm['counts'], arm['distinct_outputs']))
    print('verdict: %s %s' % (ledger['verdict'], reasons or ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
