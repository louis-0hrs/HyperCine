#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Run the RCA-195 M2 scratch-SDK contract arms and score them.

Reads only. The runner never patches anything: it drives an already-built
private copy (see tools/apply_rca195_contract.py) and writes its ledger outside
the repository.

Arms and verdict rules are fixed by
docs/validation/RCA195_SCRATCH_CONTRACT_PREREGISTRATION.md and are not
re-derived here. This tool records; the registered rules decide.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ARMS = (
    ('A', {}, 'valid control'),
    ('B', {'RCA195_CONTRACT_SKIP': '1:2'}, 'skip temporal producer ch1'),
    ('C', {'RCA195_CONTRACT_SKIP': '0:1'}, 'skip spatial producer ch0'),
    ('D', {'RCA195_CONTRACT_STALE': '1:2'}, 'forced stale token ch1'),
    ('E', {'RCA195_CONTRACT_SKIP': '1:1'}, 'skip consumer site ch1'),
)

RESOLUTIONS = ('FULL', 'HALF')
WORKERS = (2, 4)

# Registered control digest for HALF, measured on the M1 v3 probe copy with the
# same packet. It is the *default* expectation only: a second configuration
# (other packet, other CPU, other compiler) has its own valid control digest, so
# R-F compares against this value when it is registered and otherwise against
# the modal digest of this run's own control arm. Hard-coding it for every run
# would make R-F fail on every second packet for the wrong reason.
CONTROL_HALF_DIGEST = ('38d1c1498d3a990a141b622b8758a49fd1b634aa8042ef402'
                       'd219fc9ee68c686')

DRAIN = re.compile(r'DRAIN entries=(\d+) claimed=(\d+) executed=(\d+)')
DRAIN_SHORT = re.compile(r'DRAIN_SHORT remaining=(\d+) claimed=(\d+) executed=(\d+)')
STALE = re.compile(r'STALE ch=(\d+) idx=(\d+)')
CLAIM = re.compile(r'CLAIM ch=(\d+) idx=(\d+)')
EXEC = re.compile(r'EXEC ch=(\d+) idx=(\d+)')
SKIP = re.compile(r'SKIP ch=(\d+) idx=(\d+)')
PRECEDENCE = re.compile(r'PRECEDENCE kept=(\d+) dropped=(\d+)')
WFREE = re.compile(r'WFREE ptr=(\S+)')
WALLOC = re.compile(r'WALLOC ptr=(\S+)')
ABA_REUSE = re.compile(r'ABA_REUSE ptr=(\S+)')
RESULT = re.compile(
    r'RESULT sdk_error=(-?\d+) result_false=(-?\d+) claims=(\d+) executed=(\d+) '
    r'stale=(\d+) drain_short=(\d+) skipped=(\d+) walloc=(\d+) wfree=(\d+) '
    r'aba=(\d+) precedence=(\d+) realloc_enter=(\d+) realloc_early=(\d+)')

RESULT_FIELDS = ('claims', 'executed', 'stale', 'drain_short', 'skipped',
                 'walloc', 'wfree', 'aba', 'precedence',
                 'realloc_enter', 'realloc_early')


def parse_log(path):
    out = {'claims': [], 'executes': [], 'stales': [], 'skips': [],
           'drain': None, 'drain_short': None, 'precedence': [],
           'wfree': 0, 'walloc': 0, 'aba_reuse': 0, 'result': None,
           'realloc_enter': 0, 'realloc_early': 0}
    if not path.is_file():
        return out
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        m = RESULT.search(line)
        if m:
            out['result'] = dict(zip(
                ('sdk_error', 'result_false') + RESULT_FIELDS,
                (int(g) for g in m.groups())))
            continue
        m = PRECEDENCE.search(line)
        if m:
            out['precedence'].append({'kept': int(m.group(1)),
                                      'dropped': int(m.group(2))})
            continue
        m = ABA_REUSE.search(line)
        if m:
            out['aba_reuse'] += 1
            continue
        m = WFREE.search(line)
        if m:
            out['wfree'] += 1
            continue
        m = WALLOC.search(line)
        if m:
            out['walloc'] += 1
            continue
        if line.strip().startswith('WREALLOC_ENTER'):
            out['realloc_enter'] += 1
            continue
        if line.strip().startswith('WREALLOC_EARLY'):
            out['realloc_early'] += 1
            continue
        m = DRAIN.search(line)
        if m:
            out['drain'] = {'entries': int(m.group(1)),
                            'claimed': int(m.group(2)),
                            'executed': int(m.group(3))}
            continue
        m = DRAIN_SHORT.search(line)
        if m:
            out['drain_short'] = {'remaining': int(m.group(1)),
                                  'claimed': int(m.group(2)),
                                  'executed': int(m.group(3))}
            continue
        m = STALE.search(line)
        if m:
            out['stales'].append({'ch': int(m.group(1)), 'idx': int(m.group(2))})
            continue
        m = CLAIM.search(line)
        if m:
            out['claims'].append({'ch': int(m.group(1)), 'idx': int(m.group(2))})
            continue
        m = EXEC.search(line)
        if m:
            out['executes'].append({'ch': int(m.group(1)), 'idx': int(m.group(2))})
            continue
        m = SKIP.search(line)
        if m:
            out['skips'].append({'ch': int(m.group(1)), 'idx': int(m.group(2))})
    return out


def run_call(binary, packet, workers, resolution, outdir, tag, injections,
             timeout):
    env = dict(os.environ)
    for key in ('RCA195_CONTRACT_SKIP', 'RCA195_CONTRACT_STALE',
                'RCA195_CONTRACT_LOG'):
        env.pop(key, None)
    log_path = Path(outdir) / ('contract_%s.log' % tag)
    env['RCA195_CONTRACT_LOG'] = str(log_path)
    env.update(injections)

    started = time.time()
    try:
        done = subprocess.run(
            [binary, str(packet), str(workers), resolution, str(outdir), tag],
            env=env, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {'tag': tag, 'resolution': resolution, 'workers': workers,
                'status': 'HANG', 'injections': injections,
                'elapsed_s': round(time.time() - started, 2)}

    record = None
    for line in done.stdout.splitlines():
        line = line.strip()
        if line.startswith('{'):
            try:
                record = json.loads(line)
            except ValueError:
                continue
    if record is None:
        return {'tag': tag, 'resolution': resolution, 'workers': workers,
                'status': 'NO_RECORD', 'injections': injections,
                'stderr': done.stderr[:200]}

    record.pop('log', None)
    record['resolution'] = resolution
    record['workers'] = workers
    record['injections'] = injections
    record['elapsed_s'] = round(time.time() - started, 2)
    record['observed'] = parse_log(log_path)
    record['status'] = 'OK'
    return record


def score(ledger):
    """Apply the pre-registered rules in order; return (verdict, reasons)."""
    reasons = []
    calls = ledger['calls']
    control = [c for c in calls if c['arm'] == 'A']
    induced = [c for c in calls if c['arm'] != 'A']

    # R-A: the control must be clean everywhere, or nothing else is interpretable.
    broken = []
    for c in control:
        if c.get('status') != 'OK':
            broken.append('%s: %s' % (c['tag'], c.get('status')))
            continue
        obs = c['observed']
        if not c.get('sdk_ok'):
            broken.append('%s: control not OKAY' % c['tag'])
        if not obs.get('drain'):
            broken.append('%s: no DRAIN line' % c['tag'])
        elif obs['drain']['entries'] != 0 or \
                obs['drain']['claimed'] != obs['drain']['executed']:
            broken.append('%s: drain entries=%d claimed=%d executed=%d'
                          % (c['tag'], obs['drain']['entries'],
                             obs['drain']['claimed'], obs['drain']['executed']))
        if obs.get('drain_short'):
            broken.append('%s: DRAIN_SHORT fired on a valid decode' % c['tag'])
    if broken:
        reasons.append('R-A CONTROL_BROKEN: %s' % '; '.join(broken[:5]))

    # R-E: no call may corrupt the guard band.
    dirty = [c['tag'] for c in calls
             if c.get('status') == 'OK' and not c.get('guard_ok', 1)]
    if dirty:
        reasons.append('R-E MEMORY_CORRUPTION in %s' % ', '.join(dirty[:5]))

    # R-F: the candidate may not change valid-decode output.
    #
    # Two comparisons, because the expectation depends on what was registered:
    # with a registered digest, each control must reproduce it exactly; without
    # one (a new packet or host), the control arm must at least be internally
    # consistent -- one modal digest per resolution. Both catch a candidate that
    # perturbs valid output; only the first pins it to a known value.
    registered = ledger.get('control_digest')
    for resolution in sorted({c['resolution'] for c in control}):
        digests = [c.get('sha256') for c in control
                   if c.get('status') == 'OK' and c['resolution'] == resolution]
        if not digests:
            reasons.append('R-F %s: no control digest to compare' % resolution)
            continue
        if registered:
            for c in control:
                if c.get('status') != 'OK' or c['resolution'] != resolution:
                    continue
                if c.get('sha256') != registered:
                    reasons.append('R-F %s: digest changed from the registered '
                                   'control digest' % c['tag'])
        elif len(set(digests)) != 1:
            reasons.append('R-F %s: control digests are not consistent (%d '
                           'distinct in %d calls)'
                           % (resolution, len(set(digests)), len(digests)))

    # R-D: no hangs.
    hangs = [c['tag'] for c in calls if c.get('status') == 'HANG']
    if hangs:
        reasons.append('R-D HANG in %s' % ', '.join(hangs[:5]))

    # R-B: every injected call must be non-OKAY.
    survived = [c['tag'] for c in induced
                if c.get('status') == 'OK' and c.get('sdk_ok')]
    if survived:
        reasons.append('R-B FALSE_OKAY_SURVIVES in %s' % ', '.join(survived[:5]))

    # R-C: an injected non-OKAY must name the hook that closed it.
    unattributed = []
    for c in induced:
        if c.get('status') != 'OK' or c.get('sdk_ok'):
            continue
        obs = c['observed']
        fired = bool(obs.get('drain_short')) or bool(obs.get('stales'))
        if not fired:
            unattributed.append(c['tag'])
    if unattributed:
        reasons.append('R-C UNATTRIBUTED failures in %s'
                       % ', '.join(unattributed[:5]))

    # R-G: the instrument must account for itself. Every call needs its RESULT
    # summary, and each counter in it must equal the number of log lines that
    # counter is supposed to have produced. This exists because a counter that
    # reads zero can mean "it never happened" *or* "its line was lost" -- and
    # the first ABA run produced exactly that ambiguity (900 allocations, 0
    # frees, one call with 38 entries against 28+9). A call whose summary and
    # tally disagree is an instrument failure, not a measurement.
    unaccounted = []
    for c in calls:
        if c.get('status') != 'OK':
            continue
        obs = c['observed']
        res = obs.get('result')
        if not res:
            unaccounted.append('%s: no RESULT line' % c['tag'])
            continue
        tallied = {
            'claims': len(obs.get('claims') or []),
            'executed': len(obs.get('executes') or []),
            'stale': len(obs.get('stales') or []),
            'drain_short': 1 if obs.get('drain_short') else 0,
            'skipped': len(obs.get('skips') or []),
            'walloc': obs.get('walloc', 0),
            'wfree': obs.get('wfree', 0),
            'aba': obs.get('aba_reuse', 0),
            'precedence': len(obs.get('precedence') or []),
            'realloc_enter': obs.get('realloc_enter', 0),
            'realloc_early': obs.get('realloc_early', 0),
        }
        for name, count in tallied.items():
            if res.get(name) != count:
                unaccounted.append('%s: %s summary=%s lines=%d'
                                   % (c['tag'], name, res.get(name), count))
    if unaccounted:
        reasons.append('R-G INSTRUMENT_UNACCOUNTED: %s'
                       % '; '.join(unaccounted[:5]))

    if any(r.startswith(('R-A', 'R-E', 'R-F')) for r in reasons):
        return 'CONTRACT_CANDIDATE_REJECTED', reasons
    if any(r.startswith('R-B') for r in reasons):
        return 'FALSE_OKAY_SURVIVES', reasons
    if any(r.startswith(('R-C', 'R-D', 'R-G')) for r in reasons):
        return 'UNATTRIBUTED', reasons
    return 'CONTRACT_CANDIDATE_QUALIFIED', reasons


def self_test():
    """The scorer must fail the shapes the rules name, not only pass good ones."""
    def call(arm, tag, ok, entries=0, claimed=6, executed=6, sdk_ok=1,
             digest=None, guard_ok=1, status='OK', stales=0, skips=0,
             walloc=0, wfree=0, aba=0, precedence=0, realloc_enter=0,
             realloc_early=0, result_override=None):
        drain_short = (None if entries == 0 and claimed == executed
                       else {'remaining': entries, 'claimed': claimed,
                             'executed': executed})
        observed = {
            'drain': {'entries': entries, 'claimed': claimed,
                      'executed': executed},
            'drain_short': drain_short,
            'stales': [{'ch': 1, 'idx': 2}] * stales,
            'claims': [{'ch': 0, 'idx': i} for i in range(claimed)],
            'executes': [{'ch': 0, 'idx': i} for i in range(executed)],
            'skips': [{'ch': 0, 'idx': 0}] * skips,
            'walloc': walloc, 'wfree': wfree, 'aba_reuse': aba,
            'precedence': [{'kept': 58, 'dropped': 36}] * precedence,
            'realloc_enter': realloc_enter, 'realloc_early': realloc_early,
        }
        # R-G cross-checks these counters against the tallied lines, so the
        # fixture has to build a consistent pair; an inconsistent one is the
        # negative case, not the default.
        result = {'sdk_error': 0 if sdk_ok else 2048,
                  'result_false': 0 if sdk_ok else 1,
                  'claims': claimed, 'executed': executed, 'stale': stales,
                  'drain_short': 1 if drain_short else 0, 'skipped': skips,
                  'walloc': walloc, 'wfree': wfree, 'aba': aba,
                  'precedence': precedence, 'realloc_enter': realloc_enter,
                  'realloc_early': realloc_early}
        if result_override:
            result.update(result_override)
        observed['result'] = result
        return {'arm': arm, 'tag': tag, 'status': status, 'sdk_ok': sdk_ok,
                'resolution': 'HALF', 'sha256': digest or CONTROL_HALF_DIGEST,
                'guard_ok': guard_ok, 'observed': observed}

    failures = []

    good = {'calls': [call('A', 'a1', True),
                      call('B', 'b1', False, entries=1, claimed=6, executed=5,
                           sdk_ok=0)]}
    v, r = score(good)
    if v != 'CONTRACT_CANDIDATE_QUALIFIED':
        failures.append('clean ledger did not qualify: %s %s' % (v, r))

    broken = {'calls': [call('A', 'a1', True, claimed=6, executed=5)]}
    if score(broken)[0] != 'CONTRACT_CANDIDATE_REJECTED':
        failures.append('a broken control was not rejected')

    survived = {'calls': [call('A', 'a1', True),
                          call('B', 'b1', True, entries=1, sdk_ok=1)]}
    # R-B has its own verdict on purpose: "the candidate did not stop it" is a
    # different finding from "the candidate broke the control".
    if score(survived)[0] != 'FALSE_OKAY_SURVIVES':
        failures.append('a surviving injected OKAY was not flagged')

    unattributed = {'calls': [call('A', 'a1', True),
                              call('B', 'b1', False, sdk_ok=0)]}
    # entries=0, claimed==executed -> no DRAIN_SHORT, no STALE -> R-C.
    if score(unattributed)[0] != 'UNATTRIBUTED':
        failures.append('an injected failure without its hook was not flagged')

    hang = {'calls': [call('A', 'a1', True),
                      {'arm': 'B', 'tag': 'b1', 'status': 'HANG',
                       'observed': {'drain': None, 'drain_short': None,
                                    'stales': []}}]}
    if score(hang)[0] != 'UNATTRIBUTED':
        failures.append('a hang was not flagged')

    guard = {'calls': [call('A', 'a1', True, guard_ok=0)]}
    if score(guard)[0] != 'CONTRACT_CANDIDATE_REJECTED':
        failures.append('a dirty guard band was not rejected')

    # R-F has two modes, so it gets two cases. With a registered digest the
    # control must reproduce it exactly; without one, the control arm may only
    # be required to agree with itself -- a new packet has its own valid digest,
    # and pinning every run to the first packet's value would fail R-F for the
    # wrong reason.
    digest = {'calls': [call('A', 'a1', True, digest='0' * 64)],
              'control_digest': CONTROL_HALF_DIGEST}
    if score(digest)[0] != 'CONTRACT_CANDIDATE_REJECTED':
        failures.append('a changed control digest was not rejected against a '
                        'registered digest')

    digest_registered_ok = {'calls': [call('A', 'a1', True)],
                            'control_digest': CONTROL_HALF_DIGEST}
    if score(digest_registered_ok)[0] != 'CONTRACT_CANDIDATE_QUALIFIED':
        failures.append('a matching registered digest was not accepted')

    digest_scattered = {'calls': [call('A', 'a1', True),
                                  call('A', 'a2', True, digest='0' * 64)],
                        'control_digest': None}
    if score(digest_scattered)[0] != 'CONTRACT_CANDIDATE_REJECTED':
        failures.append('an internally inconsistent control arm was not rejected '
                        'without a registered digest')

    # R-G: a summary that disagrees with its own log lines is an instrument
    # failure. This is the case that mattered in practice -- a counter reading
    # zero because its line vanished looks exactly like a clean measurement.
    unaccounted = {'calls': [call('A', 'a1', True,
                                  result_override={'walloc': 9, 'wfree': 9})]}
    if score(unaccounted)[0] != 'UNATTRIBUTED':
        failures.append('a summary that disagrees with its log lines was not flagged')

    missing_result = {'calls': [call('A', 'a1', True)]}
    missing_result['calls'][0]['observed']['result'] = None
    if score(missing_result)[0] != 'UNATTRIBUTED':
        failures.append('a call without a RESULT line was not flagged')

    if failures:
        for failure in failures:
            print('RUNNER SELF-TEST FAIL %s' % failure, file=sys.stderr)
        return 1
    print('RUNNER SELF-TEST PASS: 11 scorer cases')
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--copy', help='private SDK copy root (built)')
    parser.add_argument('--packet', help='pinned .cfhd packet')
    parser.add_argument('--out', help='output directory (must be outside the repo)')
    parser.add_argument('--calls', type=int, default=5, help='calls per arm/resolution')
    parser.add_argument('--control-digest', default=None,
                        help='registered control digest; omit to require only '
                             'internal consistency of the control arm')
    parser.add_argument('--timeout', type=float, default=120.0)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()
    for required in ('copy', 'packet', 'out'):
        if not getattr(args, required):
            parser.error('--%s is required' % required)

    outdir = Path(args.out).resolve()
    if 'projects/HyperCine' in str(outdir):
        parser.error('refusing to write the ledger inside the repository')
    outdir.mkdir(parents=True, exist_ok=True)
    binary = Path(args.copy).resolve() / 'harness_contract'
    if not binary.is_file():
        parser.error('harness_contract not found in %s' % args.copy)
    packet = Path(args.packet).resolve()
    if not packet.is_file():
        parser.error('packet not found: %s' % packet)

    calls = []
    for arm, injections, label in ARMS:
        for resolution in RESOLUTIONS:
            for workers in WORKERS:
                for i in range(args.calls):
                    tag = '%s_%s_w%d_%02d' % (arm, resolution, workers, i)
                    record = run_call(binary, packet, workers, resolution,
                                      outdir, tag, injections, args.timeout)
                    record['arm'] = arm
                    record['arm_label'] = label
                    calls.append(record)
                    print('%s sdk_ok=%s %s' % (tag, record.get('sdk_ok'),
                                               record.get('status')),
                          flush=True)

    ledger = {'packet_sha256': __import__('hashlib').sha256(
                  packet.read_bytes()).hexdigest(),
              'control_digest': args.control_digest,
              'calls_per_arm': args.calls,
              'resolutions': list(RESOLUTIONS),
              'workers': list(WORKERS),
              'arms': [{'arm': a, 'injections': i, 'label': l}
                       for a, i, l in ARMS],
              'calls': calls}
    ledger['verdict'], ledger['reasons'] = score(ledger)

    path = outdir / 'rca195_contract_ledger.json'
    path.write_text(json.dumps(ledger, indent=2, sort_keys=True) + '\n',
                    encoding='utf-8')
    print('%s: %s' % (ledger['verdict'], ledger['reasons'] or 'no reasons'))
    print('wrote %s' % path)
    return 0 if ledger['verdict'] == 'CONTRACT_CANDIDATE_QUALIFIED' else 2


if __name__ == '__main__':
    sys.exit(main())
