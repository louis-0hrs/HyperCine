#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Independent re-derivation of the RCA-160 Windows cross-host A/B/A-prime.

This tool does not trust the summary block of a submitted ledger. It recomputes
the per-arm counts, the corrupt/modal classification, the wait-to-producer
matching and the verdict from the raw per-call records, and it reports the
places where the registered protocol and the executed analysis differ.

Stdlib only, read-only, deterministic. Raw decoded frames are not needed and
are never read.

Usage:
    python tools/verify_rca160_cross_host.py --ledger <report.json> [--json out.json]

Optional source checks need the published helper files and, for the newline
reproducibility probe, a pristine SDK worktree:
    --gate-applier tests/reference_decode/apply_rca160_gate.py
    --probe-header tests/reference_decode/rca160_probe.h
    --pristine <worktree containing upstream/Codec/decoder.c>
    --patched-decoder-sha256 <hash published for the private patched decoder.c>
"""
import argparse
import hashlib
import json
import math
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

# Registered in #160 comment 6264354379 / 6264215257 before the Windows run.
REGISTERED = {
    'gate_entries_per_call': 9,
    'publishes_band0_per_call': 6,
    'calls_per_arm': 120,
    'arms': ('O', 'B', 'AP'),
    'gate_max_wait_ms': 5000,
    'gate_max_spins': 50000000,
    'packet_sha256': '2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52',
    'packet_bytes': 900396,
    'source_sha256': '45c166c3d6a3710d620ff5d738980224edf53cf9ce1bceb87267e474294afff1',
    'source_bytes': 8294400,
    'protocol_sha256': 'c6a52b610e0c55a2b8b10d0984df840913ac68b85ce3ae7fd7fbca15fdfb4c25',
    'gate_applier_sha256': '712fa0a81a939eb33e2a39ac2d29b38f79e7f47b123a8bbf2d1df0b7526b9220',
    'probe_header_sha256': '88031889d7e35d9dc355c492ab311a6d64bd44237628514c4db5ea85a23157c5',
}

HEX = re.compile(r'^(?:[0-9a-f]{16}|[0-9a-f]{64})$')


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def fisher_exact(a, b, c, d):
    """Two-sided Fisher exact test for [[a, b], [c, d]]."""
    n = a + b + c + d
    row1 = a + b
    col1 = a + c

    def probability(x):
        if x < 0 or x > row1 or col1 - x < 0 or col1 - x > c + d:
            return 0.0
        return (math.comb(row1, x) * math.comb(c + d, col1 - x)) / math.comb(n, col1)

    observed = probability(a)
    total = sum(probability(x) for x in range(0, min(row1, col1) + 1)
                if probability(x) <= observed + 1e-12)
    return total


def zero_probability(calls, rate):
    if rate <= 0 or rate >= 1:
        return None
    return math.exp(calls * math.log(1.0 - rate))


def rederive_arm(records):
    """Recompute one arm from its per-call records alone."""
    calls = [r['call'] for r in records]
    hashes = Counter(r['output']['sha256'] for r in records if r.get('output'))
    modal = hashes.most_common(1)[0][0] if hashes else None
    corrupt = [r['call'] for r in records
               if r.get('output') and r['output']['sha256'] != modal]
    premature = [r['call'] for r in records if r['gate']['premature_entries'] > 0]
    waits = [c for r in records for c in r['gate'].get('coverage', [])]
    return {
        'calls': len(records),
        'unique_call_numbers': len(set(calls)),
        'distinct_outputs': len(hashes),
        'output_counts': dict(hashes.most_common()),
        'modal_sha256': modal,
        'corrupt': len(corrupt),
        'corrupt_calls': corrupt,
        'valid_outputs': sum(1 for r in records if r.get('output_valid')),
        'decode_failures': sum(1 for r in records
                               if not r.get('output_valid')),
        'nonzero_returncodes': sum(1 for r in records
                                   if r.get('returncode') not in (0, None)),
        'timed_out': sum(1 for r in records if r.get('timed_out')),
        'gate_entries_per_call': sorted({r['gate']['gate_entries'] for r in records}),
        'premature_entries': sum(r['gate']['premature_entries'] for r in records),
        'calls_with_premature': len(premature),
        'calls_with_wait': sum(1 for r in records if r['gate'].get('waits')),
        'real_waits': len(waits),
        'waits_matched': sum(1 for w in waits if w.get('producer_publish_matched')),
        'gate_timeouts': sum(r['gate']['timeouts'] for r in records),
        'publishes_band0_per_call': sorted({r['gate']['publishes_band0'] for r in records}),
        'waits_by_channel_index': {
            f'ch{ch}_idx{idx}': count
            for (ch, idx), count in Counter((w.get('ch'), w.get('idx'))
                                            for w in waits).most_common()
        },
        'entry_flag_pairs': sorted({(w.get('entry_valid'), w.get('entry_started'))
                                    for w in waits}),
        'admit_flag_pairs': sorted({(w.get('admit_valid'), w.get('admit_started'))
                                    for w in waits}),
        'max_wait_ms': max((w.get('elapsed_ms') or 0) for w in waits) if waits else 0,
        'max_spins': max((w.get('spins') or 0) for w in waits) if waits else 0,
        'decode_ms_median': round(statistics.median(
            r['worker']['decode_ns'] / 1e6 for r in records), 3),
        'decode_ms_max': round(max(
            r['worker']['decode_ns'] / 1e6 for r in records), 3),
        '_premature_calls': set(premature),
    }


def registered_verdict(arms):
    """Re-implement the registered verdict rules, in the registered order."""
    reasons = []
    o, b, ap = arms['O'], arms['B'], arms['AP']
    if o['decode_failures'] or ap['decode_failures'] or b['decode_failures']:
        reasons.append('decode failure in one or more arms')
    if o['corrupt'] == 0:
        reasons.append('original arm O did not reproduce corruption')
    if ap['corrupt'] == 0:
        reasons.append('gate-disabled return arm A-prime did not reproduce corruption')
    if b['corrupt'] != 0:
        reasons.append('gate-on arm B still corrupted')
    if b['gate_timeouts']:
        reasons.append('gate-on arm B hit the bounded gate timeout')
    if b['real_waits'] == 0:
        reasons.append('gate-on arm B never actually waited')
    if b['real_waits'] != b['waits_matched']:
        reasons.append('at least one B wait has no matched band-0 publication')
    if b['gate_entries_per_call'] != [REGISTERED['gate_entries_per_call']]:
        reasons.append('B gate-entry coverage incomplete')
    for arm in REGISTERED['arms']:
        if arms[arm]['real_waits'] == 0 and arms[arm]['gate_entries_per_call'] == [0]:
            reasons.append(f'probe records missing in arm {arm}')
    return ('CROSS_HOST_CONFIRMATION_PASS' if not reasons else 'INCONCLUSIVE'), reasons


def source_checks(args):
    findings = {}
    for label, path, key in (
            ('gate_applier', args.gate_applier, 'gate_applier_sha256'),
            ('probe_header', args.probe_header, 'probe_header_sha256'),
            ('protocol', args.protocol, 'protocol_sha256')):
        if not path:
            continue
        digest = sha256_file(path)
        findings[label] = {
            'sha256': digest,
            'registered': REGISTERED[key],
            'match': digest == REGISTERED[key],
        }
    return findings


def newline_probe(args, result):
    """Show that the published patched-source hash depends on the writer's newline.

    The applier writes with Path.write_text(), whose newline handling follows
    os.linesep: CRLF on Windows, LF elsewhere. The published hash therefore does
    not reproduce byte-for-byte on a POSIX host unless the same translation is
    forced. This probe reports both variants instead of assuming either.
    """
    if not args.pristine or not args.gate_applier:
        return None
    pristine = Path(args.pristine).expanduser().resolve()
    decoder = pristine / 'upstream' / 'Codec' / 'decoder.c'
    applier = Path(args.gate_applier).expanduser().resolve()
    if not decoder.is_file():
        return {'error': f'missing {decoder}'}

    text = decoder.read_text(encoding='utf-8')
    probe = {}
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / 'copy'
        (root / 'upstream' / 'Codec').mkdir(parents=True)
        (root / 'upstream' / 'Codec' / 'decoder.c').write_text(text, encoding='utf-8')
        completed = subprocess.run(
            [sys.executable, str(applier), '--root', str(root)],
            capture_output=True, text=True)
        if completed.returncode != 0:
            return {'error': completed.stderr.strip()[:400]}
        patched = root / 'upstream' / 'Codec' / 'decoder.c'
        patched_text = patched.read_text(encoding='utf-8')
        variants = {
            'lf': patched_text.replace('\r\n', '\n'),
            'crlf': patched_text.replace('\r\n', '\n').replace('\n', '\r\n'),
        }
        for label, body in variants.items():
            variant = Path(str(patched) + '.' + label)
            variant.write_bytes(body.encode('utf-8'))
            digest = sha256_file(variant)
            probe[label] = {
                'patched_decoder_sha256': digest,
                'matches_published_patched_decoder': (
                    digest == args.patched_decoder_sha256),
            }
        probe['published_patched_decoder_sha256'] = args.patched_decoder_sha256
        probe['pristine_line_endings'] = (
            'crlf' if b'\r\n' in decoder.read_bytes() else 'lf')
        probe['note'] = ('apply_rca160_gate.py rewrites with Path.write_text(), which '
                         'uses os.linesep. On Windows that preserves the CRLF of the '
                         'frozen tree and reproduces the published hash; on POSIX the '
                         'same patch yields the LF variant. The decoded behaviour is '
                         'identical; only the file hash is newline-dependent, so a '
                         'portable claim should compare content, not this hash.')
    result['newline_reproducibility'] = probe
    return probe


def self_test():
    """Prove the verifier discriminates: four synthetic ledgers, four outcomes."""
    def record(arm, call, digest, premature=0, waits=(), entries=9, publishes=10,
               decode_ns=20_000_000, valid=True):
        coverage = [{'ch': ch, 'idx': idx, 'spins': 1, 'elapsed_ms': 0,
                     'entry_valid': '0x0000000e', 'entry_started': '0x0000000e',
                     'admit_valid': '0x0000000f', 'admit_started': '0x0000000f',
                     'producer_publish_matched': True} for ch, idx in waits]
        return {
            'arm': arm, 'call': call, 'returncode': 0, 'timed_out': False,
            'output_valid': valid, 'stderr': '',
            'worker': {'decoder_status': 0 if valid else 7, 'decode_ns': decode_ns,
                       'cpu_max': 4, 'resolution': 'HALF',
                       'actual_format_code': 1111970369},
            'gate': {'gate_entries': entries, 'premature_entries': premature,
                     'waits': len(coverage), 'timeouts': 0,
                     'publishes_band0': publishes, 'waits_matched': len(coverage),
                     'coverage': coverage},
            'output': {'bytes': 2073600, 'sha256': digest,
                       'domain': {'width': 960, 'height': 540, 'pitch': 3840,
                                  'padding_bytes': 0}},
        }

    def build(o_corrupt, b_corrupt, ap_corrupt, b_waits=True):
        ledger = []
        for arm, corrupt in (('O', o_corrupt), ('B', b_corrupt), ('AP', ap_corrupt)):
            for call in range(1, 121):
                bad = call <= corrupt
                digest = 'f' * 64 if bad else 'a' * 64
                waits = ((0, 1),) if (arm == 'B' and b_waits and call <= 60) else ()
                ledger.append(record(arm, call, digest,
                                     premature=1 if bad else 0,
                                     waits=waits))
        return ledger

    cases = {}
    arms = {arm: rederive_arm([r for r in build(4, 0, 1) if r['arm'] == arm])
            for arm in REGISTERED['arms']}
    cases['registered_pass'] = registered_verdict(arms)[0]
    arms = {arm: rederive_arm([r for r in build(4, 0, 0) if r['arm'] == arm])
            for arm in REGISTERED['arms']}
    cases['no_induced_positive'] = registered_verdict(arms)[0]
    arms = {arm: rederive_arm([r for r in build(4, 3, 1) if r['arm'] == arm])
            for arm in REGISTERED['arms']}
    cases['gate_arm_still_corrupt'] = registered_verdict(arms)[0]
    ledger = build(4, 0, 1, b_waits=False)
    arms = {arm: rederive_arm([r for r in ledger if r['arm'] == arm])
            for arm in REGISTERED['arms']}
    cases['gate_never_waited'] = registered_verdict(arms)[0]
    arms = {arm: rederive_arm([r for r in build(4, 0, 1) if r['arm'] == arm])
            for arm in REGISTERED['arms']}
    cases['modal_agrees'] = len({arms[a]['modal_sha256'] for a in arms}) == 1

    expected = {
        'registered_pass': 'CROSS_HOST_CONFIRMATION_PASS',
        'no_induced_positive': 'INCONCLUSIVE',
        'gate_arm_still_corrupt': 'INCONCLUSIVE',
        'gate_never_waited': 'INCONCLUSIVE',
        'modal_agrees': True,
    }
    failed = [key for key, value in expected.items() if cases.get(key) != value]
    print(json.dumps({'cases': cases, 'expected': expected, 'failed': failed}, indent=1))
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--ledger')
    parser.add_argument('--json')
    parser.add_argument('--protocol')
    parser.add_argument('--gate-applier')
    parser.add_argument('--probe-header')
    parser.add_argument('--pristine')
    parser.add_argument('--patched-decoder-sha256')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if not args.ledger:
        parser.error('--ledger is required unless --self-test is given')

    report = json.loads(Path(args.ledger).read_text(encoding='utf-8'))
    ledger = report['ledger']
    result = {
        'tool': 'verify_rca160_cross_host',
        'ledger_protocol': report.get('protocol'),
        'ledger_verdict_as_submitted': report.get('verdict'),
        'ledger_reasons_as_submitted': report.get('reasons'),
        'entries': len(ledger),
    }

    # 1. Arm recomputation from per-call records.
    arms = {}
    for arm in REGISTERED['arms']:
        records = [r for r in ledger if r['arm'] == arm]
        arms[arm] = rederive_arm(records)
    result['arms'] = arms

    # 2. Registered rule check, re-derived.
    verdict, reasons = registered_verdict(arms)
    result['verdict_rederived'] = verdict
    result['reasons_rederived'] = reasons
    result['verdict_matches_submission'] = (
        verdict == report.get('verdict') and reasons == report.get('reasons', []))

    # 3. Integrity of the shared input/condition across arms.
    conditions = {
        'cpu_max': {r['worker']['cpu_max'] for r in ledger},
        'resolution': {r['worker']['resolution'] for r in ledger},
        'format_code': {r['worker']['actual_format_code'] for r in ledger},
        'output_bytes': {r['output']['bytes'] for r in ledger if r.get('output')},
        'width': {r['output']['domain']['width'] for r in ledger if r.get('output')},
        'height': {r['output']['domain']['height'] for r in ledger if r.get('output')},
        'pitch': {r['output']['domain']['pitch'] for r in ledger if r.get('output')},
        'padding': {r['output']['domain']['padding_bytes'] for r in ledger if r.get('output')},
    }
    result['condition_uniform'] = {key: sorted(value, key=str)
                                   for key, value in conditions.items()}
    modal_hashes = {arm: arms[arm]['modal_sha256'] for arm in REGISTERED['arms']}
    result['modal_identical_across_arms'] = len(set(modal_hashes.values())) == 1
    result['modal_sha256'] = modal_hashes['O']

    # 4. Execution order, as recorded.
    order = []
    for record in ledger:
        if not order or order[-1] != record['arm']:
            order.append(record['arm'])
    result['execution_order'] = order
    result['execution_order_as_registered'] = order == list(REGISTERED['arms'])

    # 5. Registered coverage rules that the executed analysis does not enforce.
    publishes = {arm: arms[arm]['publishes_band0_per_call'] for arm in REGISTERED['arms']}
    result['coverage_gaps'] = {
        'publishes_band0_registered_per_call': REGISTERED['publishes_band0_per_call'],
        'publishes_band0_observed_per_call': publishes,
        'publish_rule_enforced_in_code': False,
        'publish_rule_would_have_passed': all(
            min(values) == REGISTERED['publishes_band0_per_call']
            for values in publishes.values()),
        'entry_identity_retained_for_non_wait_entries': False,
        'note': ('The ledger keeps per-entry identity only for entries that actually '
                 'waited. Non-wait gate entries are counted, not enumerated, so '
                 '"9/9 entries in every call" is verified as a count and the '
                 'ch0/ch1/ch2 x index visit claim is evidenced only by the 60 waits.'),
    }

    # 6. Premature entry vs corruption, per arm.
    result['necessary_not_sufficient'] = {
        arm: {
            'corrupt': arms[arm]['corrupt'],
            'corrupt_without_premature_entry': len(
                set(arms[arm]['corrupt_calls']) - arms[arm]['_premature_calls']),
            'premature_without_corruption': len(
                arms[arm]['_premature_calls'] - set(arms[arm]['corrupt_calls'])),
        } for arm in REGISTERED['arms']
    }

    # 7. Sensitivity of the observed contrast.
    pooled_corrupt = arms['O']['corrupt'] + arms['AP']['corrupt']
    pooled_calls = arms['O']['calls'] + arms['AP']['calls']
    result['sensitivity'] = {
        'observed_rate_gate_off': round(pooled_corrupt / pooled_calls, 5),
        'o_vs_b_fisher_p': round(fisher_exact(
            arms['O']['corrupt'], arms['O']['calls'] - arms['O']['corrupt'],
            arms['B']['corrupt'], arms['B']['calls'] - arms['B']['corrupt']), 4),
        'pooled_gate_off_vs_b_fisher_p': round(fisher_exact(
            pooled_corrupt, pooled_calls - pooled_corrupt,
            arms['B']['corrupt'], arms['B']['calls'] - arms['B']['corrupt']), 4),
        'ap_vs_b_fisher_p': round(fisher_exact(
            arms['AP']['corrupt'], arms['AP']['calls'] - arms['AP']['corrupt'],
            arms['B']['corrupt'], arms['B']['calls'] - arms['B']['corrupt']), 4),
        'probability_zero_corrupt_in_b_at_pooled_rate': round(
            zero_probability(arms['B']['calls'], pooled_corrupt / pooled_calls), 4),
        'note': ('0/120 is expected with probability ~0.08 at the pooled gate-off '
                 'rate (and ~0.02 at the O rate alone), so the pass rests on the '
                 'registered direction and coverage, not on a rate effect size. '
                 'Do not read 4 vs 0 as a significant rate reduction.'),
    }

    # 8. Published helper hashes.
    result['source_checks'] = source_checks(args)

    # 9. Newline reproducibility of the private patch.
    newline_probe(args, result)

    for arm in REGISTERED['arms']:
        arms[arm].pop('_premature_calls', None)

    text = json.dumps(result, indent=1, sort_keys=True)
    if args.json:
        Path(args.json).write_text(text + '\n', encoding='utf-8')

    summary = {
        'verdict_rederived': result['verdict_rederived'],
        'verdict_matches_submission': result['verdict_matches_submission'],
        'arms': {arm: {key: arms[arm][key] for key in
                       ('calls', 'distinct_outputs', 'corrupt', 'real_waits',
                        'waits_matched', 'gate_timeouts', 'premature_entries')}
                 for arm in REGISTERED['arms']},
        'modal_identical_across_arms': result['modal_identical_across_arms'],
        'publish_rule_would_have_passed': result['coverage_gaps']['publish_rule_would_have_passed'],
        'fisher_p_o_vs_b': result['sensitivity']['o_vs_b_fisher_p'],
        'source_checks': {key: value.get('match') for key, value in
                          result['source_checks'].items()},
    }
    if 'newline_reproducibility' in result:
        summary['newline_reproducibility'] = result['newline_reproducibility']
    print(json.dumps(summary, indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
