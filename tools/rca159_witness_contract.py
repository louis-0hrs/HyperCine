#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Validate the recorded RCA-159 band-0 witness and its pre-registered A/B.

The published statistics are recomputed from the recorded contingency tables
rather than trusted, a withdrawn hypothesis must still be contradicted by its
own observations, and any host path in either record is refused. A contract
failure exits 1; it never reports a pass for an incomplete record.
"""
import argparse
import json
import math
from pathlib import Path
import re
import sys

EVIDENCE_SCHEMA = 'hypercine.rca159_band0_witness/1'
PROTOCOL_SCHEMA = 'hypercine.rca159_band0_guard_preregistration/1'
WITNESS_BANDS = '0x0E'
SOURCE_LOCI = (
    'upstream/Codec/image.h:227-231',
    'upstream/Codec/decoder.c:12106-12148',
    'upstream/Codec/entropy_threading.c:269-305',
)
CONTROL_IDS = ('C1', 'C2', 'C3', 'C4')
VERDICTS = ('CURE_CONFIRMED', 'NO_EFFECT', 'INCONCLUSIVE_TIMING', 'BLOCKED_UPSTREAM_PATCH')
PER_ARM_RUNNER_FIELDS = ('divergent', 'modal', 'processes', 'exit_code', 'output_sha256',
                         'early_reader_observed')
PER_CONTROL_RUNNER_FIELDS = ('executed', 'outcome', 'deadline_violated')
CONTROL_OUTCOMES = ('PASS', 'FAIL', 'SKIPPED', 'NOT_APPLICABLE')
HEX64 = re.compile(r'^[0-9a-f]{64}$')
UNSANITIZED = (
    re.compile(r'[A-Za-z]:[\\/]'),
    re.compile(r'\\\\[A-Za-z0-9_.$-]+[\\/]'),
    re.compile(r'(?:^|[\s"\'=(:,])/(?:home|Users|mnt|media|Volumes|root|opt|srv|tmp)/'),
)

REPO = Path(__file__).resolve().parents[1]
EVIDENCE = REPO / 'docs/validation/rca159_band0_witness_2026-10-25.json'
PROTOCOL = REPO / 'docs/validation/rca159_band0_guard_preregistration_2026-10-25.json'


class ContractError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise ContractError(message)


def integer(value, message):
    require(type(value) is int, message)
    return value


def fisher_two_sided(a, b, c, d):
    row1, row2 = a + b, c + d
    col1, total = a + c, a + b + c + d
    observed = math.comb(row1, a) * math.comb(row2, c) / math.comb(total, col1)
    low, high = max(0, col1 - row2), min(col1, row1)
    return sum(p for p in (math.comb(row1, x) * math.comb(row2, col1 - x) / math.comb(total, col1)
                           for x in range(low, high + 1)) if p <= observed * (1 + 1e-12))


def significant_digits(value):
    mantissa = repr(float(value)).lower().split('e')[0].lstrip('-')
    digits = mantissa.replace('.', '').lstrip('0').rstrip('0')
    return max(1, len(digits))


def round_significant(value, digits):
    return float('%.*e' % (digits - 1, value))


def matches_published(exact, published, label):
    digits = significant_digits(published)
    require(round_significant(exact, digits) == float(published),
            '%s: recomputed %r does not round to published %r at %d significant digits'
            % (label, exact, published, digits))


def strings(node):
    if isinstance(node, str):
        yield node
    elif isinstance(node, dict):
        for key, value in node.items():
            yield key
            for text in strings(value):
                yield text
    elif isinstance(node, list):
        for value in node:
            for text in strings(value):
                yield text


def check_sanitized(document, label):
    for text in strings(document):
        for pattern in UNSANITIZED:
            require(not pattern.search(text),
                    '%s: unsanitized host path in %r' % (label, text))


def check_all_false(mapping, label):
    require(isinstance(mapping, dict) and mapping, '%s: missing non-claim block' % label)
    for key, value in mapping.items():
        require(value is False, '%s: non-claim %r must be exactly false' % (label, key))


def check_table(table, label):
    require(isinstance(table, dict), '%s: missing contingency table' % label)
    counts = table.get('counts')
    require(isinstance(counts, list) and len(counts) == 2, '%s: counts must be two rows' % label)
    for row in counts:
        require(isinstance(row, list) and len(row) == 2, '%s: counts must be two columns' % label)
        for value in row:
            integer(value, '%s: counts must be integers' % label)
    require(len(table.get('rows', [])) == 2 and len(table.get('columns', [])) == 2,
            '%s: table headings must be two by two' % label)
    return counts


def validate_evidence(document):
    require(document.get('schema') == EVIDENCE_SCHEMA, 'unknown evidence schema')
    require(integer(document.get('issue'), 'issue must be an integer') == 234, 'evidence must serve #234')
    require(document.get('executed_by_this_record') is False,
            'this record must not claim to have executed the experiment')

    provenance = document.get('provenance', {})
    require(provenance.get('tracked_upstream_modified') is False, 'upstream must stay unmodified')
    require(provenance.get('private_bytes_included') is False, 'private bytes must stay local')
    require(provenance.get('host_identity_published') is False, 'host identity must stay local')
    require(provenance.get('source_issue') == 159 and provenance.get('source_pull_request') == 169,
            'evidence must cite #159 / #169')

    mechanism = document.get('mechanism', {})
    state = mechanism.get('observed_gate_state', {})
    require(state.get('band_valid_flags') == WITNESS_BANDS and state.get('band_started_flags') == WITNESS_BANDS,
            'recorded band state must be the measured 0x0E/0x0E witness')
    require(state.get('band0_valid') is False and state.get('band0_started') is False,
            'band 0 must be recorded as neither valid nor started')
    require(state.get('gate_all_valid_returned') is True and state.get('inverse_executed') is True,
            'the gate must be recorded as admitting the inverse')
    require(integer(mechanism.get('reader_calls_per_decode'), 'reader call count must be an integer') == 6,
            'the recorded witness has six reader calls per decode')
    require(tuple(mechanism.get('source_loci', ())) == SOURCE_LOCI, 'source loci must be the recorded relative paths')

    population = document.get('witness', {}).get('population', {})
    integer(population.get('fresh_processes'), 'witness population must be an integer process count')
    integer(population.get('reader_calls_per_process'), 'witness must record reader calls per process')
    require(population.get('reader_calls_per_process') == 6, 'witness must record six reader calls per process')

    counts = check_table(document.get('witness', {}).get('contingency', {}), 'witness')
    witness_p = document['witness']['contingency'].get('published_p_two_sided')
    exact = fisher_two_sided(*counts[0], *counts[1])
    matches_published(exact, witness_p, 'witness')
    witness_divergent, witness_modal = sum(counts[0]), sum(counts[1])
    require(population.get('fresh_processes') == witness_divergent + witness_modal,
            'witness population must equal the contingency total %d, found %r'
            % (witness_divergent + witness_modal, population.get('fresh_processes')))

    arms = document.get('rate_arms', {}).get('arms', [])
    require(len(arms) >= 2, 'rate arms missing')
    quotable = [arm.get('id') for arm in arms if arm.get('quotable_as_rate') is True]
    require(quotable == ['uninstrumented'],
            'only the uninstrumented arm may be quoted as a rate, found %r' % (quotable,))
    by_id = {}
    for arm in arms:
        arm_id = arm.get('id')
        require(arm_id and arm_id not in by_id, 'rate arms need unique ids, found %r' % (arm_id,))
        by_id[arm_id] = arm
        divergent = integer(arm.get('divergent'), 'arm %r divergent count' % arm_id)
        modal = integer(arm.get('modal'), 'arm %r modal count' % arm_id)
        processes = integer(arm.get('processes'), 'arm %r process count' % arm_id)
        require(divergent + modal == processes,
                'arm %r: divergent + modal must equal the process count' % arm_id)

    traced = [arm for arm in arms
              if (arm['divergent'], arm['modal'], arm['processes'])
              == (witness_divergent, witness_modal, witness_divergent + witness_modal)]
    require(len(traced) == 1,
            'exactly one rate arm must reproduce the witness table, found %r'
            % ([arm.get('id') for arm in traced],))
    traced = traced[0]
    require(traced.get('quotable_as_rate') is False,
            'the arm that reproduces the witness table is traced and may not be quoted as a rate')

    contrast_table = document.get('rate_arms', {}).get('traced_vs_uninstrumented', {})
    contrast = check_table(contrast_table, 'rate contrast')
    require(contrast_table.get('rows') == [traced['id'], 'uninstrumented'],
            'the rate contrast must name the traced arm first and the uninstrumented arm second, found %r'
            % (contrast_table.get('rows'),))
    require(contrast_table.get('columns') == ['divergent', 'modal'],
            'the rate contrast columns must be divergent then modal, found %r'
            % (contrast_table.get('columns'),))
    require(contrast == [[traced['divergent'], traced['modal']],
                         [by_id['uninstrumented']['divergent'], by_id['uninstrumented']['modal']]],
            'the rate contrast must be the arms it summarises, found %r' % (contrast,))
    matches_published(fisher_two_sided(*contrast[0], *contrast[1]),
                      contrast_table.get('published_p_two_sided'),
                      'rate contrast')

    withdrawn = document.get('withdrawn_hypotheses', [])
    require(withdrawn, 'a corrected witness must record its withdrawn hypotheses')
    for entry in withdrawn:
        require(entry.get('claim') and entry.get('withdrawn_because'),
                'withdrawn hypotheses need a claim and a reason')
        frames = entry.get('contradicting_observations', {}).get('frames', [])
        require(len(frames) >= 3, 'withdrawn hypotheses need the contradicting frame table')
        by_count = {}
        for frame in frames:
            count = integer(frame.get('early_accept_events'), 'event count must be an integer')
            moved = tuple(frame.get('planes_moved', ()))
            require(moved, 'each frame must record the planes that moved')
            changed = {channel for channel, percent in frame.get('channel_changed_percent', {}).items()
                       if percent > 0}
            require(changed == set(moved),
                    'channel percentages must mark exactly the moved planes')
            by_count.setdefault(count, set()).add(moved)
        same_count_different_planes = any(len(planes) > 1 for planes in by_count.values())
        different_count_same_planes = (len({next(iter(planes)) for planes in by_count.values()}) == 1
                                       and len(by_count) > 1)
        require(same_count_different_planes or different_count_same_planes,
                'the recorded frames do not actually contradict the withdrawn claim')

    domains = document.get('hash_domains', {})
    require(domains.get('recomputed_here') is False,
            'this record does not own the private output and must not claim a rehash')
    entries = domains.get('entries', [])
    require(len(entries) == 2, 'both recorded hash domains are required')
    image, extended = entries
    width, height = image.get('geometry', (0, 0))
    require(width * height * image.get('bytes_per_pixel', 0) == image.get('bytes'),
            'image domain length must equal geometry times bytes per pixel')
    require(extended.get('bytes') == image.get('bytes') + extended.get('sentinel_bytes', -1),
            'sentinel domain must extend the image domain by its sentinel bytes')
    require(image.get('pitch') == extended.get('pitch'), 'both domains must share the recorded pitch')
    sentinel = integer(extended.get('sentinel_byte_value'), 'sentinel byte value must be an integer')
    require(0 <= sentinel <= 255, 'sentinel byte value must fit in one byte, found %r' % (sentinel,))
    for entry in entries:
        require(HEX64.match(entry.get('sha256', '')), 'recorded hashes must be lowercase sha256')

    check_all_false(document.get('non_claims'), 'evidence')
    blocked = [item.get('step') for item in document.get('remaining_blocked_work', [])]
    require(blocked == [2, 4, 5], 'the unimplemented steps must stay listed as blocked')
    for item in document['remaining_blocked_work']:
        require(item.get('blocker'), 'every blocked step must name its blocker')

    check_sanitized(document, 'evidence')
    return dict(witness_p_two_sided=float(witness_p),
                rate_p_two_sided=float(document['rate_arms']['traced_vs_uninstrumented']['published_p_two_sided']),
                withdrawn=len(withdrawn))


def validate_protocol(document, evidence):
    require(document.get('schema') == PROTOCOL_SCHEMA, 'unknown protocol schema')
    require(integer(document.get('issue'), 'issue must be an integer') == 234, 'protocol must serve #234')
    require(document.get('executed') is False, 'the pre-registration must predate execution')
    require(document.get('status') == 'PREREGISTERED_NOT_EXECUTED', 'protocol status must stay unexecuted')

    prerequisite = document.get('prerequisite_evidence', {})
    require(prerequisite.get('schema') == evidence.get('schema'), 'protocol must cite the witness schema')
    require((REPO / prerequisite.get('path', '')).is_file(), 'protocol must cite an existing witness record')

    patch = document.get('patch_requirement', {})
    for flag in ('unbounded_busy_wait_forbidden', 'producer_write_publish_ordering_reviewed',
                 'consumer_matching_acquire_or_lock_reviewed', 'flag_bit_alone_is_not_publication'):
        require(patch.get(flag) is True, 'patch requirement %r must be registered' % flag)
    require(patch.get('tracked_upstream_change_permitted') is False,
            'the protocol must not permit a tracked upstream change')
    require(set(patch.get('checked_at', ())) == {'enqueue', 'worker_dequeue'},
            'the predicate must be checked at both enqueue and worker dequeue')

    arms = {arm.get('id'): arm for arm in document.get('arms', [])}
    require(set(arms) == {'A', 'B'}, 'exactly arms A and B are registered')
    require(arms['A'].get('guard_enabled') is False and arms['B'].get('guard_enabled') is True,
            'arm A is unmodified and arm B carries the guard')

    controls = [control.get('id') for control in document.get('controls', [])]
    require(controls == list(CONTROL_IDS), 'all four registered controls are required, found %r' % (controls,))

    invariants = document.get('registration_invariants', {})
    for flag in ('same_packet_sha256_in_both_arms', 'same_worker_count_in_both_arms',
                 'same_affinity_in_both_arms', 'same_cpu_count_in_both_arms',
                 'uninstrumented_counts_reported_separately_from_trace_counts',
                 'trace_used_for_correspondence_only'):
        require(invariants.get(flag) is True, 'registration invariant %r must hold' % flag)
    require(integer(invariants.get('child_deadline_seconds'), 'child deadline must be an integer') > 0,
            'a bounded child deadline is required')

    require(tuple(document.get('permitted_verdicts', ())) == VERDICTS, 'permitted verdicts changed')

    fields = document.get('runner_required_fields', [])
    require(fields, 'the runner must have required fields')
    for required in ('packet_sha256', 'worker_count', 'verdict'):
        require(any(field.endswith(required) for field in fields),
                'runner field %r is required' % required)
    for arm_id in ('A', 'B'):
        for name in PER_ARM_RUNNER_FIELDS:
            field = 'run.arms.%s.%s' % (arm_id, name)
            require(field in fields,
                    'runner field %r is required so arm %s can be reported' % (field, arm_id))
    for control_id in CONTROL_IDS:
        for name in PER_CONTROL_RUNNER_FIELDS:
            field = 'run.controls.%s.%s' % (control_id, name)
            require(field in fields,
                    'runner field %r is required so control %s can be reported' % (field, control_id))
    require('run.controls.executed' not in fields,
            'an aggregate run.controls.executed cannot show which control was skipped')
    require(tuple(document.get('control_outcomes', ())) == CONTROL_OUTCOMES,
            'the control outcome vocabulary must keep skipped controls visible')

    check_all_false(document.get('non_claims'), 'protocol')
    check_sanitized(document, 'protocol')
    return dict(controls=len(controls), runner_fields=len(fields))


def load(path):
    with open(path, encoding='utf-8') as handle:
        return json.load(handle)


def validate(evidence_path=EVIDENCE, protocol_path=PROTOCOL):
    evidence = load(evidence_path)
    protocol = load(protocol_path)
    result = validate_evidence(evidence)
    result.update(validate_protocol(protocol, evidence))
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path, default=EVIDENCE)
    parser.add_argument('--protocol', type=Path, default=PROTOCOL)
    args = parser.parse_args(argv)
    try:
        result = validate(args.evidence, args.protocol)
    except (OSError, ContractError, KeyError, TypeError) as error:
        print('RCA-159 witness contract invalid: %s' % error, file=sys.stderr)
        return 1
    print(json.dumps(dict(status='CONTRACT_VALID', **result), sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
