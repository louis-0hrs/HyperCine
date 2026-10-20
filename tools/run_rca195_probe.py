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

Classification per call, in this order (pre-registration v3):

  HANG                no record within the per-call timeout
  NO_RECORD           the harness produced no JSON line
  NO_QUEUE_RECORD     no queue-reset line: the call cannot be scored at all
  NO_OUTPUT           SDK reported an error
  UNMITTED_DISCARD_FALSE_OKAY
                      an entry was never admitted and the queue was discarded
                      anyway, SDK OKAY, digest differs
  DROPPED_FALSE_OKAY  a claimed transform never executed, SDK OKAY, digest differs
  DROPPED_IDENTICAL   a claimed transform never executed, digest equal
  NATURAL_VARIANT     SDK OKAY, digest differs, nothing dropped
  OK                  SDK OKAY and digest equals the modal control digest

Why v3 exists: v2's ledger carried a *flat* record schema that the v2 runner
could not emit (the runner writes a nested ``queue`` object), so re-scoring the
committed record with the committed code returned NATURAL_VARIANT for all four
induced arms. A verdict that does not reproduce from its own published
artefacts is not evidence. v3 accepts both shapes, refuses to score a record
without a queue line, enforces the registered SKIP-match criterion that v2
stated but never checked, and adds ``--rescore`` so this failure is caught by
CI instead of by review. See docs/validation/RCA195_PROBE_PREREGISTRATION_V3.md.

This tool never decides that the decoder is wrong; it only records. The verdict
is computed from the registered rules and printed with its reason list.
"""
import argparse
import collections
import json
import os
import re
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

# The queue line is written by tools/rca195_probe.h. `withheld` only exists in
# the superseded revision's logs, so it is optional here; everything after it is
# not. A parser that disagrees with the emitter silently drops every record --
# which is exactly what happened once, so the self-test parses a literal line.
RESET = re.compile(
    r'QUEUE_RESET num_entries=(\d+) free_entry=(\d+) next_entry=(\d+) '
    r'admitted=(\d+) unadmitted=(\d+) (?:withheld=\d+ )?skipped=(\d+) '
    r'claimed=(\d+) executed=(\d+)')
SKIP = re.compile(r'SKIP ch=(\d+) idx=(\d+) entry_consumed=1 executed=0')

DROPPED = ('DROPPED_FALSE_OKAY', 'DROPPED_IDENTICAL')


def run_call(harmess, packet, cpu_max, resolution, outdir, tag, skip, timeout):
    env = dict(os.environ)
    env['RCA195_PROBE_LOG'] = str(Path(outdir) / ('probe_%s.log' % tag))
    if skip:
        env['RCA195_SKIP'] = skip
    else:
        env.pop('RCA195_SKIP', None)
    env.pop('RCA195_WITHHOLD', None)

    cmd = ['taskset', '-c', '0', harmess, str(packet), str(cpu_max),
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
                    'admitted': int(m.group(4)),
                    'unadmitted': int(m.group(5)),
                    'skipped': int(m.group(6)),
                    'claimed': int(m.group(7)),
                    'executed': int(m.group(8)),
                }
            s = SKIP.search(line)
            if s:
                skips.append({'ch': int(s.group(1)), 'idx': int(s.group(2))})

    # The harness log path is a local absolute path: never publish it.
    record.pop('log', None)
    record['queue'] = queue
    record['skips'] = skips
    if queue is not None:
        # Flat mirrors, so a reader (and --rescore) can see the witness without
        # knowing the nesting. Both are derived from the same queue line.
        record['claimed'] = queue['claimed']
        record['executed'] = queue['executed']
        record['skipped'] = queue['skipped']
        record['num_entries'] = queue['num_entries']
    record['status'] = 'OK' if record.get('sdk_ok') else 'NO_OUTPUT'
    return record


def queue_of(record):
    """Queue state, accepting the nested shape and the older flat shape."""
    queue = record.get('queue')
    if isinstance(queue, dict):
        return queue
    if all(k in record for k in ('claimed', 'executed')):
        return {
            'claimed': record.get('claimed', 0),
            'executed': record.get('executed', 0),
            'skipped': record.get('skipped', 0),
            'num_entries': record.get('num_entries', 0),
        }
    return None


def skips_of(record):
    """Skip lines, accepting [(ch, idx)] or [{'ch':..,'idx':..}]."""
    out = []
    for item in record.get('skips') or []:
        if isinstance(item, dict):
            out.append((item.get('ch'), item.get('idx')))
        elif isinstance(item, (list, tuple)) and len(item) == 2:
            out.append((item[0], item[1]))
    return out


def classify(records, control_digest, skip_target=None):
    """Label each call. The witness is the claim/execute counter, not the hash.

    Registered in the v3 pre-registration: natural control calls and induced
    calls reach the same byte states, so a digest match is corroboration only.
    A call is only "dropped" when the worker claimed a transform out of the queue
    and never executed it. A call whose queue was discarded with an entry that was
    never admitted at all is scored separately and more severely, because that is
    the production state the maintainer review describes.
    """
    target = None
    if skip_target:
        parts = skip_target.split(':')
        if len(parts) == 2:
            target = (int(parts[0]), int(parts[1]))

    for record in records:
        if record.get('status') in ('HANG', 'NO_RECORD'):
            continue
        queue = queue_of(record)
        if queue is None:
            record['status'] = 'NO_QUEUE_RECORD'
            continue
        if not record.get('sdk_ok'):
            record['status'] = 'NO_OUTPUT'
            continue

        claimed = queue.get('claimed', 0)
        executed = queue.get('executed', 0)
        unadmitted = queue.get('num_entries', 0)
        dropped = claimed > executed
        differs = record.get('sha256') != control_digest
        matched = target is not None and target in skips_of(record)

        if unadmitted > 0 and differs:
            record['status'] = 'UNMITTED_DISCARD_FALSE_OKAY'
        elif dropped and differs:
            record['status'] = 'DROPPED_FALSE_OKAY'
        elif dropped:
            record['status'] = 'DROPPED_IDENTICAL'
        elif differs:
            record['status'] = 'NATURAL_VARIANT'
        else:
            record['status'] = 'OK'
        record['skip_target_matched'] = matched


def score(ledger):
    """Apply the v3 registered rules in order. Returns (verdict, reasons)."""
    reasons = []
    arms = ledger['arms']
    a = arms['A']
    induced = tuple(n for n in ('B', 'C', 'D', 'E') if n in arms)

    # R1 -- coverage: a record without a queue line cannot be scored.
    missing = [(n, r['tag']) for n, arm in arms.items()
               for r in arm['records'] if queue_of(r) is None]
    if missing:
        reasons.append('%d record(s) carry no queue-reset line, so the arm cannot '
                       'be scored: %s' % (len(missing),
                                          ', '.join('%s/%s' % m for m in missing[:6])))

    # R2 -- the injection must be what causes the drop, not the decoder itself.
    control_bad = [r['tag'] for r in a['records']
                   if r.get('status') in DROPPED
                   or r.get('status') == 'UNMITTED_DISCARD_FALSE_OKAY'
                   or (queue_of(r) or {}).get('num_entries', 0) > 0]
    if control_bad:
        reasons.append('control arm already discards entries in %d call(s) (%s), so '
                       'the effect is not attributable to the induction'
                       % (len(control_bad), ', '.join(control_bad[:6])))

    # R3 -- the control must have a usable mode.
    digests = [r['sha256'] for r in a['records'] if r.get('sha256')]
    if not digests:
        reasons.append('control arm produced no digests')
    else:
        modal_count = collections.Counter(digests).most_common(1)[0][1]
        if modal_count < 25:
            reasons.append('control arm is too scattered to serve as a mode '
                           '(%d/%d on the modal digest)' % (modal_count, len(digests)))

    # R4 -- a hit must carry the registered SKIP record for its own target.
    evidence = []
    for name in induced:
        arm = arms[name]
        hits = [r for r in arm['records']
                if r.get('status') in ('DROPPED_FALSE_OKAY',
                                       'UNMITTED_DISCARD_FALSE_OKAY')]
        matched = [r for r in hits if r.get('skip_target_matched')]
        if hits and not matched:
            reasons.append('arm %s has %d dropped call(s) but none logs the SKIP '
                           'line for its own target %s' % (name, len(hits), arm['skip']))
        for r in matched:
            evidence.append({
                'arm': name,
                'tag': r['tag'],
                'status': r['status'],
                'skipped': (queue_of(r) or {}).get('skipped', 0),
                'num_entries_at_reset': (queue_of(r) or {}).get('num_entries', 0),
            })

    if not evidence:
        reasons.append('no call combined SDK OKAY, a differing digest, a claimed '
                       'transform that never executed and its matching SKIP line')

    # R5 -- instrument integrity: no skip may fire outside the arm's own target.
    for name in induced:
        arm = arms[name]
        target = tuple(int(x) for x in arm['skip'].split(':'))
        stray = [r['tag'] for r in arm['records']
                 if any(s != target for s in skips_of(r))]
        if stray:
            reasons.append('arm %s skipped an entry outside its target %s in %d '
                           'call(s) (%s)' % (name, arm['skip'], len(stray),
                                             ', '.join(stray[:6])))

    return ('FALSE_OKAY_DEMONSTRATED' if not reasons else 'INCONCLUSIVE'), reasons


def summarize(ledger):
    modal = ledger['control_modal_sha256']
    for name, arm in ledger['arms'].items():
        classify(arm['records'], modal, arm.get('skip'))
        arm['counts'] = dict(collections.Counter(r['status'] for r in arm['records']))
        arm['distinct_outputs'] = len({r['sha256'] for r in arm['records']
                                       if r.get('sha256')})
        scored = [r for r in arm['records'] if queue_of(r) is not None]
        arm['queue_reset_all_zero'] = bool(scored) and all(
            queue_of(r).get('num_entries', 0) == 0 for r in scored)
        arm['scored_calls'] = len(scored)

    ledger['verdict'], ledger['reasons'] = score(ledger)
    return ledger


def build_ledger(harness, packet, outdir, calls, cpu_max, resolution, timeout, wanted):
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    ledger = {'arms': {}, 'packet_sha256': None,
              'resolution': resolution, 'cpu_max': cpu_max,
              'calls_per_arm': calls}
    ledger['packet_sha256'] = sha256_of(packet)

    control_records = []
    for name, skip in ARMS:
        if name not in wanted:
            continue
        arm_dir = outdir / ('arm_%s' % name)
        arm_dir.mkdir(parents=True, exist_ok=True)
        records = []
        for index in range(1, calls + 1):
            tag = '%s%03d' % (name, index)
            record = run_call(harness, packet, cpu_max, resolution,
                              arm_dir, tag, skip, timeout)
            records.append(record)
        ledger['arms'][name] = {'skip': skip, 'records': records}
        if name == 'A':
            control_records = records

    digests = [r['sha256'] for r in control_records if r.get('sha256')]
    ledger['control_modal_sha256'] = (
        collections.Counter(digests).most_common(1)[0][0] if digests else None)

    summarize(ledger)
    a = ledger['arms']['A']
    ledger['control_natural_variant_rate'] = '%d/%d' % (
        a['counts'].get('NATURAL_VARIANT', 0), len(a['records']))
    return ledger


def sha256_of(path):
    import hashlib
    h = hashlib.sha256()
    with open(path, 'rb') as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


# ----------------------------------------------------------------- self-test

def _rec(tag, claimed=9, executed=9, num_entries=0, digest='aa', sdk_ok=1,
         skips=None):
    return {'tag': tag, 'sha256': digest, 'sdk_ok': sdk_ok, 'sdk_error': 0,
            'guard_ok': 1, 'queue': {'claimed': claimed, 'executed': executed,
                                     'num_entries': num_entries,
                                     'skipped': len(skips or []),
                                     'free_entry': 9, 'next_entry': 9},
            'skips': skips or []}


def _ledger(control, arms):
    ledger = {'arms': {'A': {'skip': None, 'records': control}},
              'control_modal_sha256': 'aa'}
    for name, (target, records) in arms.items():
        ledger['arms'][name] = {'skip': target, 'records': records}
    summarize(ledger)
    return ledger


def parse_queue_line(line):
    """Parse one QUEUE_RESET line exactly as tools/rca195_probe.h writes it."""
    m = RESET.search(line)
    if not m:
        return None
    return {
        'num_entries': int(m.group(1)),
        'free_entry': int(m.group(2)),
        'next_entry': int(m.group(3)),
        'admitted': int(m.group(4)),
        'unadmitted': int(m.group(5)),
        'skipped': int(m.group(6)),
        'claimed': int(m.group(7)),
        'executed': int(m.group(8)),
    }


def self_test():
    """Synthetic ledgers that MUST come back INCONCLUSIVE.

    A verifier that cannot return INCONCLUSIVE is not a check.
    """
    controls = [_rec('A%03d' % i) for i in range(1, 31)]
    good_hit = _rec('B001', claimed=9, executed=7, digest='bb',
                    skips=[{'ch': 1, 'idx': 2}])
    failures = []

    # The emitter in tools/rca195_probe.h and this parser must agree. A literal
    # line, not a round trip through this module, so a format change on either
    # side fails here instead of producing 150 unscored records.
    emitted = ('QUEUE_RESET num_entries=0 free_entry=9 next_entry=9 admitted=9 '
               'unadmitted=0 skipped=2 claimed=9 executed=7 unexecuted=2')
    parsed = parse_queue_line(emitted)
    if parsed != {'num_entries': 0, 'free_entry': 9, 'next_entry': 9,
                  'admitted': 9, 'unadmitted': 0, 'skipped': 2,
                  'claimed': 9, 'executed': 7}:
        failures.append('queue-line parser does not accept the emitted format: %r'
                        % (parsed,))

    def expect(label, ledger, verdict, needle=None):
        got = ledger['verdict']
        if got != verdict:
            failures.append('%s: expected %s, got %s (%s)'
                            % (label, verdict, got, ledger['reasons']))
        elif needle and not any(needle in r for r in ledger['reasons']):
            failures.append('%s: reason does not mention %r: %s'
                            % (label, needle, ledger['reasons']))

    expect('acceptance', _ledger(controls, {'B': ('1:2', [good_hit])}),
           'FALSE_OKAY_DEMONSTRATED')

    # A control that already drops removes attribution.
    bad_control = [_rec('A%03d' % i) for i in range(1, 31)]
    bad_control[3] = _rec('A004', claimed=9, executed=8, digest='cc')
    expect('control-drop', _ledger(bad_control, {'B': ('1:2', [good_hit])}),
           'INCONCLUSIVE', 'not attributable')

    # A control with no usable mode.
    scattered = [_rec('A%03d' % i, digest='d%02d' % i) for i in range(1, 31)]
    expect('scattered', _ledger(scattered, {'B': ('1:2', [good_hit])}),
           'INCONCLUSIVE', 'too scattered')

    # A hit without its own SKIP line.
    no_skip = _rec('B001', claimed=9, executed=7, digest='bb', skips=[])
    expect('no-skip-line', _ledger(controls, {'B': ('1:2', [no_skip])}),
           'INCONCLUSIVE', 'SKIP')

    # A hit whose SKIP line names a different entry.
    wrong_skip = _rec('B001', claimed=9, executed=7, digest='bb',
                      skips=[{'ch': 0, 'idx': 2}])
    expect('stray-skip', _ledger(controls, {'B': ('1:2', [wrong_skip])}),
           'INCONCLUSIVE', 'outside its target')

    # A record with no queue evidence at all: nothing to score.
    blind = {'tag': 'B001', 'sha256': 'bb', 'sdk_ok': 1, 'sdk_error': 0,
             'guard_ok': 1}
    expect('no-queue-line', _ledger(controls, {'B': ('1:2', [blind])}),
           'INCONCLUSIVE', 'no queue-reset line')

    # A record carrying the flat counter witness but no SKIP enumeration --
    # exactly the shape of the committed v2 ledger. The witness is visible, but
    # the registered SKIP-match criterion cannot be satisfied from the record.
    flat_only = {'tag': 'B001', 'sha256': 'bb', 'sdk_ok': 1, 'sdk_error': 0,
                 'guard_ok': 1, 'claimed': 9, 'executed': 7, 'skipped': 2}
    expect('flat-witness-only', _ledger(controls, {'B': ('1:2', [flat_only])}),
           'INCONCLUSIVE', 'SKIP')

    # SDK error is not a false OKAY.
    errored = _rec('B001', claimed=9, executed=7, digest='bb', sdk_ok=0,
                   skips=[{'ch': 1, 'idx': 2}])
    expect('sdk-error', _ledger(controls, {'B': ('1:2', [errored])}),
           'INCONCLUSIVE', 'no call combined')

    if failures:
        for failure in failures:
            print('SELF-TEST FAIL %s' % failure, file=sys.stderr)
        return 1
    print('SELF-TEST PASS: 1 log-format round trip, 1 acceptance and 6 rejection '
          'cases')
    return 0


# --------------------------------------------------------------------- main

def rescore(path):
    """Re-derive a committed ledger's verdict from its own records."""
    ledger = json.loads(Path(path).read_text(encoding='utf-8'))
    recorded = ledger.get('verdict')
    summarize(ledger)
    print('recorded verdict: %s' % recorded)
    print('re-derived      : %s' % ledger['verdict'])
    for name in ('A', 'B', 'C', 'D', 'E'):
        if name in ledger['arms']:
            arm = ledger['arms'][name]
            print('  %s skip=%-6s %s  scored=%s'
                  % (name, arm.get('skip') or '-', arm['counts'],
                     arm.get('scored_calls')))
    for reason in ledger['reasons']:
        print('  reason: %s' % reason)
    if recorded != ledger['verdict']:
        print('RESCORE FAIL: the published ledger does not reproduce from its own '
              'records', file=sys.stderr)
        return 1
    print('RESCORE PASS')
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--harness')
    parser.add_argument('--packet')
    parser.add_argument('--outdir')
    parser.add_argument('--calls', type=int, default=30)
    parser.add_argument('--cpu-max', type=int, default=4)
    parser.add_argument('--resolution', default='HALF')
    parser.add_argument('--timeout', type=float, default=60.0)
    parser.add_argument('--json')
    parser.add_argument('--arms', default='A,B,C,D,E',
                        help='comma-separated subset of A,B,C,D,E')
    parser.add_argument('--rescore', metavar='LEDGER',
                        help='re-derive the verdict of an existing ledger')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()
    if args.rescore:
        return rescore(args.rescore)
    for required in ('harness', 'packet', 'outdir', 'json'):
        if not getattr(args, required):
            parser.error('--%s is required unless --rescore/--self-test is given'
                         % required)

    wanted = {a.strip().upper() for a in args.arms.split(',') if a.strip()}
    unknown = wanted - {name for name, _ in ARMS}
    if unknown:
        parser.error('unknown arm(s): %s' % ', '.join(sorted(unknown)))

    ledger = build_ledger(args.harness, args.packet, args.outdir, args.calls,
                          args.cpu_max, args.resolution, args.timeout, wanted)
    Path(args.json).write_text(json.dumps(ledger, indent=1) + '\n',
                               encoding='utf-8')

    modal = ledger['control_modal_sha256']
    print('control modal: %s' % (modal[:16] + '\u2026' if modal else 'none'))
    for name in ('A', 'B', 'C', 'D', 'E'):
        if name in ledger['arms']:
            arm = ledger['arms'][name]
            print('  %s skip=%-6s %s  distinct=%d'
                  % (name, arm['skip'] or '-', arm['counts'],
                     arm['distinct_outputs']))
    print('verdict: %s %s' % (ledger['verdict'], ledger['reasons'] or ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
