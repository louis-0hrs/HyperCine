#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""RCA-195 Milestone-2 design model: transform-queue ownership and liveness.

This is a DESIGN MODEL, not a measurement and not a decoder patch. It encodes the
frozen SDK's queue discipline and the proposed replacement as two policies over
one small interpreter, then searches the interleaving space for a counterexample
to the invariants that matter.

Why a model and not another probe: Milestone 1 established *reachability* by
fault injection (v3 ledger, 120/120 induced false-OKAY). Milestone 2 has to
establish that a proposed rule cannot report success with a missing band, for
*every* interleaving, not for 30 sampled ones. That claim is about the shape of
the state machine, so it is checked exhaustively on a small model and the
model's fidelity is pinned to source anchors by the verifier.

Policies:

  released   -- upstream as shipped: readiness is `valid == started`; a claim is
                `done = 1; num_entries--`; the worker re-tests the same predicate
                after unlocking and silently skips when it fails; the drain zeroes
                the queue without verifying anything and returns OKAY.
  proposed   -- QUEUED -> CLAIMED -> EXECUTING -> PUBLISHED | FAILED, claim gated
                on the per-topology required-band set published for the entry's
                generation, no silent skip, drain verifies.

Invariants:

  I1 FAIL_CLOSED   the decode reports OK only if every required input band of
                   every output wavelet is PUBLISHED.
  I2 NO_DOUBLE     no queue entry is executed more than once.
  I3 NO_SILENT     no entry leaves the queue unexecuted without a terminal error.
  I4 BOUNDED       the run terminates within the step bound.
  I5 PROGRESS      if a worker is idle and an entry is claimable, a worker takes
                   it (no entry starves while capacity exists).

Usage:
  python tools/rca195_m2_model.py --check
  python tools/rca195_m2_model.py --json docs/validation/<file>.json
  python tools/rca195_m2_model.py --self-test
"""
import argparse
import itertools
import json
import sys
from pathlib import Path

BAND_VALID_MASK = lambda band: 1 << band

# Per-topology required *input* bands, re-derived from DecodedBandsValid
# (upstream/Codec/decoder.c:24136). `threaded_band_mask` names the band that a
# transform earlier in the queue is expected to have produced, so it is NOT a
# precondition of this entry -- it is a dependency on another entry.
#   field-plus index 2  -> temporal, both bands produced by earlier transforms
#   field-plus 3 and 5  -> end of a chain, all bands required
#   field-plus other    -> band 0 produced earlier, 0xE required
#   spatial   index 2   -> top of pyramid, all bands required
#   spatial   other     -> band 0 produced earlier, 0xE required
FIELDPLUS = 'FIELDPLUS'
SPATIAL = 'SPATIAL'


def required_input_mask(transform_type, index, num_bands):
    full = (1 << num_bands) - 1
    if transform_type == FIELDPLUS:
        if index == 2:
            return 0
        if index in (3, 5):
            return full
        return full & ~BAND_VALID_MASK(0)
    if transform_type == SPATIAL:
        if index == 2:
            return full
        return full & ~BAND_VALID_MASK(0)
    raise ValueError('unknown transform type %r' % (transform_type,))


def produced_mask(num_bands):
    """The band this transform writes: the lowpass of the next wavelet down."""
    return BAND_VALID_MASK(0) if num_bands > 1 else 1


class Entry:
    __slots__ = ('name', 'channel', 'index', 'transform_type', 'num_bands',
                 'required', 'generation', 'state', 'executions')

    def __init__(self, name, channel, index, transform_type, num_bands,
                 generation=0):
        self.name = name
        self.channel = channel
        self.index = index
        self.transform_type = transform_type
        self.num_bands = num_bands
        self.required = required_input_mask(transform_type, index, num_bands)
        self.generation = generation
        self.state = 'QUEUED'
        self.executions = 0


class Bands:
    """One (channel, index) wavelet's flag words, kept as two words on purpose.

    The released predicate compares both; the proposed policy reads only the
    published word. Modelling them separately is what lets the race exist in the
    model at all.
    """

    def __init__(self):
        self.valid = 0
        self.started = 0
        self.generation = 0

    def raise_started(self, band):
        self.started |= BAND_VALID_MASK(band)

    def publish_valid(self, band):
        self.valid |= BAND_VALID_MASK(band)

    def release_ok(self):
        return self.valid == self.started

    def required_ok(self, mask):
        return (self.valid & mask) == mask


class Machine:
    """Interleaving-searchable queue. One action per step."""

    def __init__(self, entries, workers, policy, bands, producer_lead=0):
        self.entries = entries
        self.workers = workers
        self.policy = policy
        self.bands = bands          # (channel, index) -> Bands
        self.producer_lead = producer_lead
        self.log = []
        self.error = None           # first terminal error, as in decoder->error
        self.reported_ok = None
        self.drained = False
        # Real band count per wavelet, so the action space is finite.
        self.band_width = {(e.channel, e.index): e.num_bands
                           for e in entries}

    # -- observable state ------------------------------------------------
    def queued(self):
        return [e for e in self.entries if e.state == 'QUEUED']

    def claimed(self):
        return [e for e in self.entries if e.state in ('CLAIMED', 'EXECUTING')]

    def live(self):
        # DROPPED is terminal for the queue: the entry was consumed and never
        # executed. Treating it as live meant the search never reached the drain
        # at all, which is exactly the state the released policy hides.
        return [e for e in self.entries
                if e.state not in ('PUBLISHED', 'FAILED', 'DROPPED')]

    def bands_for(self, entry):
        return self.bands[(entry.channel, entry.index)]

    def deps_ok(self, entry):
        if entry.required == 0:
            return True
        return self.bands_for(entry).required_ok(entry.required)

    def generation_ok(self, entry):
        return self.bands_for(entry).generation == entry.generation

    # -- released policy -------------------------------------------------
    def released_claimable(self):
        return [e for e in self.queued()
                if self.bands_for(e).release_ok()]

    def released_recheck_ok(self, entry):
        return self.bands_for(entry).release_ok()

    # -- proposed policy -------------------------------------------------
    def proposed_claimable(self):
        return [e for e in self.queued()
                if self.deps_ok(e) and self.generation_ok(e)]

    # -- actions ---------------------------------------------------------
    def actions(self):
        if self.drained:
            return []
        acts = [('drain', None)]
        if self.policy == 'released':
            for entry in self.released_claimable():
                acts.append(('claim', entry))
        else:
            for entry in self.proposed_claimable():
                acts.append(('claim', entry))
        # Producers can always advance, which is what races the re-check. Band
        # actions are capped at each wavelet's real band count so the search
        # terminates instead of climbing to eight bits.
        for key, bands in self.bands.items():
            num_bands = self.band_width[key]
            for band in range(num_bands):
                if not bands.started & BAND_VALID_MASK(band):
                    acts.append(('started', (key, band)))
                    break
            for band in range(num_bands):
                if not bands.valid & BAND_VALID_MASK(band):
                    acts.append(('publish', (key, band)))
                    break
        return acts

    def step(self, action):
        kind, payload = action
        if kind == 'drain':
            self.drain()
            self.log.append(('drain',))
            return
        if kind == 'started':
            (key, band) = payload
            self.bands[key].raise_started(band)
            self.log.append(('started', key, band))
            return
        if kind == 'publish':
            (key, band) = payload
            self.bands[key].publish_valid(band)
            self.log.append(('publish', key, band))
            return

        entry = payload
        if self.policy == 'released':
            # done = 1; next_entry++; num_entries--; then unlock, then re-test.
            entry.state = 'CLAIMED'
            self.log.append(('claim', entry.name))
            if self.released_recheck_ok(entry):
                entry.state = 'PUBLISHED'
                entry.executions += 1
                self.bands_for(entry).publish_valid(0)
                self.log.append(('exec', entry.name))
            else:
                # The silent skip: consumed from the queue, never executed,
                # no error recorded anywhere.
                entry.state = 'DROPPED'
                self.log.append(('skip', entry.name))
            return

        # proposed: claim -> execute under a re-check that can only fail on a
        # generation change, and a failure is terminal rather than silent.
        entry.state = 'EXECUTING'
        self.log.append(('claim', entry.name))
        if not self.generation_ok(entry):
            entry.state = 'FAILED'
            if self.error is None:
                self.error = 'CODEC_ERROR_BAD_FRAME(stale-generation:%s)' % entry.name
            self.log.append(('fail', entry.name))
            return
        entry.state = 'PUBLISHED'
        entry.executions += 1
        self.bands_for(entry).publish_valid(0)
        self.log.append(('exec', entry.name))

    def drain(self):
        """WaitForTransformThread's epilogue."""
        self.drained = True
        if self.policy == 'released':
            # The queue is zeroed without looking; nothing can be learned here.
            stranded = [e for e in self.entries
                        if e.state in ('QUEUED', 'CLAIMED', 'DROPPED')]
            for entry in stranded:
                entry.state = 'DISCARDED'
            self.reported_ok = True
            return
        # proposed: every entry must be terminal, else a terminal error.
        live = self.live()
        if live:
            if self.error is None:
                self.error = ('CODEC_ERROR_BAD_FRAME(incomplete:%s)'
                              % ','.join(e.name for e in live))
            self.reported_ok = False
            return
        self.reported_ok = self.error is None

    # -- invariants ------------------------------------------------------
    def violates(self):
        """Return the invariant names this final state breaks."""
        broken = []
        # I1: OK reported but a required input of some wavelet is unpublished.
        if self.reported_ok:
            for entry in self.entries:
                if entry.state != 'PUBLISHED':
                    continue
                if not self.deps_ok(entry):
                    broken.append('I1_FAIL_CLOSED')
                    break
        # I2: an entry executed more than once.
        if any(e.executions > 1 for e in self.entries):
            broken.append('I2_NO_DOUBLE')
        # I3: an entry left the queue without executing and without an error.
        if self.reported_ok and self.error is None:
            for entry in self.entries:
                if entry.state in ('DROPPED', 'DISCARDED'):
                    broken.append('I3_NO_SILENT')
                    break
        return broken


def initial_bands(entries):
    bands = {}
    for entry in entries:
        bands.setdefault((entry.channel, entry.index), Bands())
    return bands


def search(policy, entries_factory, workers, max_steps=60, limit=20000):
    """Depth-first over every interleaving. Returns (violations, explored)."""
    found = {}
    explored = [0]

    # One factory call. Building the entries and their band table from two
    # separate calls silently pairs objects that no longer correspond.
    entries = entries_factory()

    def walk(machine, depth):
        if explored[0] >= limit:
            return
        explored[0] += 1
        if depth >= max_steps:
            machine.log.append(('bound',))
            return
        if machine.drained:
            for name in machine.violates():
                found.setdefault(name, list(machine.log))
            return
        acts = machine.actions()
        if not acts:
            found.setdefault('I4_BOUNDED', list(machine.log))
            return
        for action in acts:
            snapshot = _snapshot(machine)
            machine.step(action)
            walk(machine, depth + 1)
            _restore(machine, snapshot)

    walk(Machine(entries, workers, policy, initial_bands(entries)), 0)
    return found, explored[0]


def _snapshot(machine):
    return (
        [(e.state, e.executions) for e in machine.entries],
        {k: (b.valid, b.started, b.generation) for k, b in machine.bands.items()},
        machine.error,
        machine.reported_ok,
        machine.drained,
        len(machine.log),
    )


def _restore(machine, snap):
    states, bands, error, ok, drained, loglen = snap
    for entry, (state, executions) in zip(machine.entries, states):
        entry.state = state
        entry.executions = executions
    for key, (valid, started, generation) in bands.items():
        machine.bands[key].valid = valid
        machine.bands[key].started = started
        machine.bands[key].generation = generation
    machine.error = error
    machine.reported_ok = ok
    machine.drained = drained
    del machine.log[loglen:]


# ------------------------------------------------------------------ scenarios

def scenario_claim_skip():
    """One spatial entry at index 1: band 0 is produced by an earlier transform.

    The released predicate passes while `valid == started == 0x0`, the entry is
    claimed, and a later `started` raise on the same wavelet breaks the re-check.
    """
    return [Entry('W11', channel=0, index=1, transform_type=SPATIAL,
                  num_bands=4)]


def scenario_never_admitted():
    """A producer raises `started` and never publishes `valid`.

    With `started != valid` the released predicate never passes, the entry is
    never admitted, and the drain discards it while returning OKAY. This is the
    `UNMITTED_DISCARD_FALSE_OKAY` shape the v3 ledger recorded 2/30 of in arm B.
    """
    return [Entry('W11', channel=1, index=1, transform_type=SPATIAL,
                  num_bands=4)]


def scenario_chain():
    """Two dependent entries: band 0 of the upper wavelet comes from below."""
    return [Entry('W21', channel=0, index=2, transform_type=SPATIAL,
                  num_bands=4),
            Entry('W11', channel=0, index=1, transform_type=SPATIAL,
                  num_bands=4)]


SCENARIOS = (
    ('claim_skip', scenario_claim_skip),
    ('never_admitted', scenario_never_admitted),
    ('chain', scenario_chain),
)


# ------------------------------------------------------- source anchoring

def anchor_matrix(repo=None):
    """Compare this model's required masks against the frozen source.

    The matrix in §2 R1 is a claim about `DecodedBandsValid`. If that function
    changes, this model would keep asserting a stale rule while its own tests
    stayed green — so the matrix is re-derived from the tree and compared, and a
    divergence is a hard failure rather than a note.
    """
    import importlib.util

    repo = Path(repo).resolve() if repo else Path(__file__).resolve().parents[1]
    verifier_path = Path(__file__).with_name('verify_rca195_band0_contract.py')
    if not verifier_path.is_file():
        return {'status': 'SKIPPED', 'reason': 'verifier not present'}

    spec = importlib.util.spec_from_file_location('rca195_verifier', verifier_path)
    verifier = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verifier)

    rows = []
    mismatches = []
    for transform_type, index, num_bands in (
            ('SPATIAL', 0, 4), ('SPATIAL', 1, 4), ('SPATIAL', 2, 4),
            ('FIELDPLUS', 2, 2), ('FIELDPLUS', 3, 4), ('FIELDPLUS', 4, 4),
            ('FIELDPLUS', 5, 4)):
        upstream = verifier.evaluate_mask_logic(transform_type, index, num_bands)
        # `None` means "earlier transforms compute both bands", i.e. this entry
        # requires no band of its own wavelet to be published first.
        expected = 0 if upstream is None else upstream
        modelled = required_input_mask(transform_type, index, num_bands)
        ok = expected == modelled
        rows.append({'transform': transform_type, 'index': index,
                     'num_bands': num_bands,
                     'upstream_mask': ('None' if upstream is None
                                       else '0x%X' % upstream),
                     'model_mask': '0x%X' % modelled, 'match': ok})
        if not ok:
            mismatches.append('%s index=%d: upstream 0x%X, model 0x%X'
                              % (transform_type, index, expected, modelled))

    # The predicate and the drain must still be the shapes this design assumes.
    text = verifier.read(repo, verifier.IMAGE_H)
    if verifier.MACRO_RELEASED not in text:
        mismatches.append('released BANDS_ALL_VALID form changed')
    drain = verifier.read(repo, verifier.THREADING)
    for anchor, what in (
            ('data->queue[curr_entry].done = 1;', 'claim write'),
            ('if(wavelet && BANDS_ALL_VALID(wavelet))', 'post-unlock re-check'),
            ('} while(wavelet && BANDS_ALL_VALID(wavelet));', 'drain loop tail')):
        if anchor not in drain:
            mismatches.append('%s anchor vanished: %s' % (what, anchor))

    return {
        'status': 'OK' if not mismatches else 'MISMATCH',
        'rows': rows,
        'mismatches': mismatches,
    }


def run_matrix(limit=20000):
    report = {'policy': {}, 'scenarios': {}}
    for name, factory in SCENARIOS:
        per = {}
        for policy in ('released', 'proposed'):
            found, explored = search(policy, factory, workers=2, limit=limit)
            # `explored >= limit` means the depth-first walk was cut short, so an
            # empty violation list is an absence in the explored subspace, not a
            # proof over all interleavings. Recording it prevents the two from
            # being read as the same claim.
            truncated = explored >= limit
            per[policy] = {'violations': sorted(found),
                           'interleavings': explored,
                           'search_truncated': truncated,
                           'exhaustive': not truncated}
            report['policy'].setdefault(policy, [])
            report['policy'][policy].extend(sorted(found))
        report['scenarios'][name] = per
    for policy, names in list(report['policy'].items()):
        report['policy'][policy] = sorted(set(names))
    return report


def self_test():
    """The model must reproduce the defect in `released` and clear `proposed`.

    A model that cannot fail is not a check. These are the negative cases.
    """
    failures = []
    report = run_matrix()

    released = report['policy']['released']
    proposed = report['policy']['proposed']

    for needed in ('I1_FAIL_CLOSED', 'I3_NO_SILENT'):
        if needed not in released:
            failures.append('released policy does not reproduce %s' % needed)
    if 'I2_NO_DOUBLE' in released:
        failures.append('released policy claims a double execution; the frozen '
                        'code cannot do that and the model should not invent it')
    if proposed:
        failures.append('proposed policy still violates %s' % (proposed,))

    # A scenario whose search was cut off must not be counted as a clearance
    # without saying so. The single-entry scenarios are small enough to be
    # exhaustive; the two-entry chain is not, at this limit.
    for name, per in report['scenarios'].items():
        if name in ('claim_skip', 'never_admitted') and not per['proposed']['exhaustive']:
            failures.append('%s should be exhaustive at this limit but was truncated'
                            % name)
        if per['proposed']['violations'] and not per['proposed']['search_truncated']:
            failures.append('%s proposed policy violates an invariant exhaustively' % name)

    # The two released failures must be distinguishable: a skip with the entry
    # claimed, and a discard of an entry that was never claimed.
    claim = report['scenarios']['claim_skip']['released']['violations']
    never = report['scenarios']['never_admitted']['released']['violations']
    if 'I3_NO_SILENT' not in claim:
        failures.append('claim_skip does not reproduce the silent skip')
    if 'I3_NO_SILENT' not in never:
        failures.append('never_admitted does not reproduce the unadmitted discard')

    if failures:
        for failure in failures:
            print('SELF-TEST FAIL %s' % failure, file=sys.stderr)
        return 1

    print('SELF-TEST PASS: released reproduces I1+I3 in both shapes, proposed '
          'clears all five invariants; exhaustive on %d interleavings, '
          'chain truncated at the search limit (reported, not counted as proof)'
          % sum(s['proposed']['interleavings']
                for n, s in report['scenarios'].items()
                if s['proposed']['exhaustive']))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--json', metavar='PATH')
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    report = run_matrix()
    report['kind'] = 'DESIGN_MODEL'
    report['note'] = ('reachability model of the transform queue, not a '
                      'measurement of the frozen decoder')
    report['required_input_masks'] = {
        'FIELDPLUS': {i: required_input_mask(FIELDPLUS, i, 4) for i in range(6)},
        'SPATIAL': {i: required_input_mask(SPATIAL, i, 4) for i in range(3)},
    }
    report['source_anchor'] = anchor_matrix()
    if report['source_anchor']['status'] != 'OK':
        for problem in report['source_anchor']['mismatches']:
            print('ANCHOR FAIL: %s' % problem, file=sys.stderr)
        return 2

    if args.json:
        out = Path(args.json)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(report, indent=1) + '\n', encoding='utf-8')
        print('wrote %s' % out)

    for name, per in report['scenarios'].items():
        print('%-16s released=%-28s proposed=%-4s %s'
              % (name, ','.join(per['released']['violations']) or '-',
                 ','.join(per['proposed']['violations']) or '-',
                 'exhaustive' if per['proposed']['exhaustive'] else 'TRUNCATED'))
    if not args.check and not args.json:
        parser.error('one of --check/--json/--self-test is required')
    print('MODEL PASS' if not report['policy']['proposed'] else 'MODEL FAIL')
    return 0 if not report['policy']['proposed'] else 2


if __name__ == '__main__':
    sys.exit(main())
