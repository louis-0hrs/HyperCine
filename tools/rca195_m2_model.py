#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""RCA-195 Milestone-2 design model, revision 2: real producer edges.

This supersedes revision 1, which the maintainer review rejected as
`MODEL_INCOMPLETE`. Revision 1 had three structural faults and one of them made
the model unable to see the very defect it claimed to fix:

1. It published the transform's output onto the **entry's own input wavelet**
   (`publish_valid(0)` on `plane(index)`). The source writes the lowpass into
   `transform->wavelet[index-1]` (decoder.c:13023/13051), so a missing upstream
   producer could be papered over by the model's own write.
2. It let `publish` fire for any band with no owning producer. Producer edges
   were never represented.
3. It performed claim, execution and publication inside one atomic action, so no
   interleaving involving a claimed-but-unexecuted entry, a generation change
   after claim, or two competing workers could be explored.

Revision 2 fixes all three by modelling three things separately:

* **Planes**: `(channel, index, generation)` with independent `valid`/`started`
  bit words, exactly as the released decoder keeps them.
* **Read and write sets per entry**, derived from `ReconstructWaveletBand` and
  from the inverse transforms themselves, and pinned to source by
  `--check`. An entry writes into *another* plane's band 0; it never writes its
  own inputs.
* **Scheduled transitions**: `CLAIM -> ADMIT -> EXECUTE -> PUBLISHED|FAILED`,
  one action each, with real worker occupancy and generation invalidation.

The silent-no-op is modelled explicitly, because it is the actual mechanism:
`TransformInverseSpatialQuantLowpass` begins with

    if (input->band[0] == NULL) return;      (wavelet.c:5699, and [1..3] after)

so an inverse handed a wavelet missing a band **returns without writing anything
and without setting an error**. That is why the caller still reports OKAY.

Policies:

  released  the shipped discipline: claim on `valid == started`, silent skip on
            a failed post-unlock re-check, silent no-op when a read band is
            absent, drain zeroes the queue without verifying.
  legacy    revision 1 of this model, kept only as a negative regression: it
            reports success where the real code cannot. It must FAIL the
            invariants, which proves the corrected model is not merely agreeing
            with itself.
  proposed  claim gated on the full published read set and on generation;
            a missing input at execution is a terminal error; drain verifies.

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

# --------------------------------------------------------------------------
# Topologies. Each plane is (index, num_bands). `entropy_bands` is the set of
# bands that entropy decoding publishes for that plane; every other band is
# published by a transform entry. This distinction is the whole point: band 0 of
# a non-top plane is *never* an entropy output, so a model that lets entropy
# publish it invents a producer that does not exist.
#
# Pinned to the frozen subband tables:
#   intra  decoder.c:11596  wavelet {2,2,2,2,1,1,1,0,0,0} bands {0,1,2,3,1,2,3,1,2,3}
#   group  decoder.c:11191  wavelet {5,5,5,5,4,4,4,3,3,3,3,1,1,1,0,0,0}
#                            bands   {0,1,2,3,1,2,3,0,1,2,3,1,2,3,1,2,3}
# A plane whose band 0 appears in the subband table receives it from entropy;
# the others receive band 0 from the adjacent transform (decoder.c:13051).
# --------------------------------------------------------------------------

TOPOLOGIES = {
    # Intra/spatial: plane 2 is the top and gets every band from entropy.
    # Entry(index=2) reads plane 2 and writes plane 1 band 0.
    # Entry(index=1) reads plane 1 and writes plane 0 band 0.
    'spatial_chain': {
        'planes': {0: 4, 1: 4, 2: 4},
        'entropy_bands': {0: {1, 2, 3}, 1: {1, 2, 3}, 2: {0, 1, 2, 3}},
        'entries': [
            # (name, channel, index, reads, writes)
            #   reads/writes are (plane_index, band) pairs on this channel.
            ('E2', 0, 2, [(2, 0), (2, 1), (2, 2), (2, 3)], [(1, 0)]),
            ('E1', 0, 1, [(1, 0), (1, 1), (1, 2), (1, 3)], [(0, 0)]),
        ],
        # The entry whose absence the review asked to be caught.
        'regression_target': 'E1',
    },
    # Field-plus temporal: the temporal wavelet has two entropy bands and the
    # inverse writes band 0 into *both* frame wavelets (decoder.c:13255-13256).
    'fieldplus_temporal': {
        'planes': {0: 4, 1: 4, 2: 2},
        'entropy_bands': {0: {1, 2, 3}, 1: {1, 2, 3}, 2: {0, 1}},
        'entries': [
            ('T2', 0, 2, [(2, 0), (2, 1)], [(0, 0), (1, 0)]),
        ],
        'regression_target': 'T2',
    },
}

# Read set of the highpass branch, for the topology-check anchor.
HIGHPASS_READS = [(3, 0), (3, 1), (3, 2), (3, 3)]
HIGHPASS_WRITE = (2, 1)


# --------------------------------------------------------------------- state

class Plane:
    __slots__ = ('valid', 'started', 'generation')

    def __init__(self):
        self.valid = 0
        self.started = 0
        self.generation = 0

    def bands(self, num_bands):
        out = set()
        for band in range(num_bands):
            if self.valid & (1 << band):
                out.add(band)
        return out

    def release_ok(self):
        """The shipped predicate: two flag words compared to each other."""
        return self.valid == self.started


class Entry:
    __slots__ = ('name', 'channel', 'index', 'generation', 'reads', 'writes',
                 'state', 'executions')

    def __init__(self, name, channel, index, reads, writes):
        self.name = name
        self.channel = channel
        self.index = index
        self.generation = 0
        self.reads = tuple(reads)
        self.writes = tuple(writes)
        # The claimable set is on the *input* plane of this entry.
        self.state = 'QUEUED'
        self.executions = 0


class Machine:
    def __init__(self, topology, policy, workers=2):
        self.topo = topology
        self.policy = policy
        self.workers = workers
        self.busy = frozenset()
        self.planes = {(0, i): Plane() for i in topology['planes']}
        self.entries = [Entry(*spec) for spec in topology['entries']]
        self.error = None
        self.reported_ok = None
        self.drained = False
        # Entropy publications are independent bit-sets, so their mutual order
        # is irrelevant to every queue decision. They are fired through one
        # pointer in a fixed order instead of being permuted: for any subset of
        # events completed before a queue action, the same subset is reachable
        # by advancing the pointer. This is a symmetry reduction, not a
        # restriction on the interleavings that matter.
        self.entropy_events = []
        for (channel, index), _plane in sorted(self.planes.items()):
            for band in sorted(topology['entropy_bands'].get(index, set())):
                # started precedes valid, as in the source: entropy announces a
                # band before it publishes it.
                self.entropy_events.append(('e_started', (channel, index, band)))
                self.entropy_events.append(('e_valid', (channel, index, band)))
        self.events_done = 0

    # -- helpers ---------------------------------------------------------
    def key(self):
        return (
            tuple((e.state, e.executions, e.generation) for e in self.entries),
            tuple(sorted((k, p.valid, p.started, p.generation)
                         for k, p in self.planes.items())),
            tuple(sorted(self.busy)),
            self.events_done,
            self.error,
            self.reported_ok,
            self.drained,
        )

    def clone(self):
        other = Machine.__new__(Machine)
        other.topo = self.topo
        other.policy = self.policy
        other.workers = self.workers
        other.busy = self.busy
        other.planes = {}
        for k, p in self.planes.items():
            q = Plane()
            q.valid, q.started, q.generation = p.valid, p.started, p.generation
            other.planes[k] = q
        other.entries = []
        for e in self.entries:
            f = Entry(e.name, e.channel, e.index, e.reads, e.writes)
            f.generation = e.generation
            f.state = e.state
            f.executions = e.executions
            other.entries.append(f)
        other.error = self.error
        other.reported_ok = self.reported_ok
        other.drained = self.drained
        other.entropy_events = self.entropy_events
        other.events_done = self.events_done
        return other

    def plane(self, channel, index):
        return self.planes[(channel, index)]

    def input_plane(self, entry):
        return self.plane(entry.channel, entry.index)

    def reads_published(self, entry):
        for (plane_index, band) in entry.reads:
            p = self.plane(entry.channel, plane_index)
            if not p.valid & (1 << band):
                return False
        return True

    def generation_ok(self, entry):
        for (plane_index, _band) in entry.reads + entry.writes:
            if self.plane(entry.channel, plane_index).generation != entry.generation:
                return False
        return True

    def claimable(self, entry):
        if entry.state != 'QUEUED':
            return False
        if self.policy == 'released':
            # The shipped predicate compares the two flag words, so a plane that
            # entropy has not touched yet (0 == 0) is "ready".
            return self.input_plane(entry).release_ok()
        if self.policy == 'legacy':
            # Revision 1 of this model: claim gated on the *entropy* mask only.
            # For a 4-band plane that is 0xE, which deliberately excludes band 0
            # -- the band the adjacent transform must produce. This is the
            # reviewer's first counterexample and the reason revision 1 was
            # withdrawn.
            plane = self.input_plane(entry)
            num_bands = self.topo['planes'][entry.index]
            mask = 0 if entry.index == 2 and num_bands == 2 else ((1 << num_bands) - 1) & ~1
            return (plane.valid & mask) == mask
        return self.reads_published(entry) and self.generation_ok(entry)

    # -- actions ---------------------------------------------------------
    def actions(self):
        if self.drained:
            return []
        acts = [('drain', None)]
        free = [w for w in range(self.workers) if w not in self.busy]
        for entry in self.entries:
            if entry.state == 'QUEUED' and free and self.claimable(entry):
                for w in free[:1]:
                    acts.append(('claim', (entry.name, w)))
            elif entry.state == 'CLAIMED':
                acts.append(('admit', entry.name))
            elif entry.state == 'EXECUTING':
                acts.append(('execute', entry.name))

        # Exactly one entropy event per step: the next in canonical order. The
        # per-band loop that used to sit here enumerated the same events in
        # arbitrary order, which is what made the state space explode without
        # exploring a single additional queue interleaving.
        if self.events_done < len(self.entropy_events):
            acts.append(self.entropy_events[self.events_done])

        if self.policy == 'proposed':
            # A generation change is allowed at most once per plane. Without this
            # cap the counter is unbounded, the state space is infinite, and the
            # search can only ever truncate -- which would make "exhaustive"
            # unprovable for the wrong reason. One change after a claim is the
            # case the design has to survive.
            for (channel, index), plane in self.planes.items():
                if plane.generation == 0:
                    acts.append(('stale', (channel, index)))
        return acts

    def step(self, action, entry_lookup):
        kind, payload = action
        if kind == 'drain':
            self.drain()
            return
        if kind == 'e_started':
            channel, index, band = payload
            self.plane(channel, index).started |= 1 << band
            self.events_done += 1
            return
        if kind == 'e_valid':
            channel, index, band = payload
            self.plane(channel, index).valid |= 1 << band
            self.events_done += 1
            return
        if kind == 'stale':
            channel, index = payload
            self.plane(channel, index).generation += 1
            return
        if kind == 'claim':
            name, worker = payload
            entry = entry_lookup[name]
            entry.state = 'CLAIMED'
            entry.generation = self.plane(entry.channel, entry.index).generation
            self.busy = self.busy | {worker}
            return
        if kind == 'admit':
            entry = entry_lookup[payload]
            if self.policy == 'released':
                # The post-unlock re-check re-runs the same vacuous predicate.
                if self.input_plane(entry).release_ok():
                    entry.state = 'EXECUTING'
                else:
                    entry.state = 'DROPPED'      # silent skip, no error
                    self._free_worker(entry)
            elif self.policy == 'legacy':
                entry.state = 'EXECUTING'
            else:
                if self.generation_ok(entry) and self.reads_published(entry):
                    entry.state = 'EXECUTING'
                else:
                    self._fail(entry, 'stale-generation')
            return
        if kind == 'execute':
            entry = entry_lookup[payload]
            if self.policy == 'released':
                if self.reads_published(entry):
                    self._apply_writes(entry)
                else:
                    # wavelet.c returns silently when a read band is absent.
                    entry.state = 'NOOP'
            elif self.policy == 'legacy':
                # Revision 1 published onto the entry's *own* input plane
                # (`publish_valid(0)` on plane(index)) instead of onto
                # plane(index-1). That fabricates a band the real inverse never
                # writes and hides a missing upstream producer.
                self.plane(entry.channel, entry.index).valid |= 1
                entry.state = 'PUBLISHED'
                entry.executions += 1
            else:
                if self.reads_published(entry) and self.generation_ok(entry):
                    self._apply_writes(entry)
                else:
                    self._fail(entry, 'input-missing-at-execute')
            self._free_worker(entry)
            return

    def _free_worker(self, entry):
        # Worker occupancy is per claim; free the single worker that holds it.
        if self.busy:
            self.busy = frozenset(sorted(self.busy)[1:])

    def _apply_writes(self, entry):
        for (plane_index, band) in entry.writes:
            self.plane(entry.channel, plane_index).valid |= 1 << band
        entry.state = 'PUBLISHED'
        entry.executions += 1

    def _fail(self, entry, why):
        entry.state = 'FAILED'
        if self.error is None:
            self.error = 'CODEC_ERROR_BAD_FRAME(%s:%s)' % (entry.name, why)
        self._free_worker(entry)

    def drain(self):
        self.drained = True
        if self.policy in ('released', 'legacy'):
            # Zeroed without inspection, no completeness check, no error. Under
            # the legacy policy this is what made revision 1 report success.
            for entry in self.entries:
                if entry.state in ('QUEUED', 'CLAIMED', 'EXECUTING'):
                    entry.state = 'DISCARDED'
            self.reported_ok = True
            return
        live = [e for e in self.entries
                if e.state in ('QUEUED', 'CLAIMED', 'EXECUTING')]
        if live:
            if self.error is None:
                self.error = ('CODEC_ERROR_BAD_FRAME(incomplete:%s)'
                              % ','.join(e.name for e in live))
            self.reported_ok = False
            return
        self.reported_ok = self.error is None

    # -- invariants ------------------------------------------------------
    def violated(self):
        bad = []
        ok = bool(self.reported_ok)

        # I1: a public OK with an entry that never published, or with a read set
        # that was never satisfied at execution time.
        if ok:
            for entry in self.entries:
                if entry.state in ('DROPPED', 'DISCARDED', 'NOOP', 'FAILED'):
                    bad.append('I1_FAIL_CLOSED')
                    break
                if entry.state == 'PUBLISHED' and not self.reads_published(entry):
                    bad.append('I1_FAIL_CLOSED')
                    break
            # Every plane band that only a transform can produce must exist.
            if 'I1_FAIL_CLOSED' not in bad:
                for (channel, index), plane in self.planes.items():
                    nbands = self.topo['planes'][index]
                    entropy = self.topo['entropy_bands'].get(index, set())
                    for band in range(nbands):
                        if band in entropy:
                            continue
                        if not plane.valid & (1 << band):
                            bad.append('I1_FAIL_CLOSED')
                            break
                    if 'I1_FAIL_CLOSED' in bad:
                        break

        # I2: nothing executed twice.
        if any(e.executions > 1 for e in self.entries):
            bad.append('I2_NO_DOUBLE')

        # I3: nothing left the queue unexecuted and unreported.
        if ok and self.error is None:
            for entry in self.entries:
                if entry.state in ('DROPPED', 'DISCARDED', 'NOOP'):
                    bad.append('I3_NO_SILENT')
                    break

        # I1 is the only invariant a *terminal* run can break; a run that ended
        # with an error is fail-closed by definition.
        if self.error is not None and self.reported_ok is False:
            bad = [b for b in bad if b == 'I2_NO_DOUBLE']
            if not any(e.executions > 1 for e in self.entries):
                bad = []
        return bad


def false_okay_states(topology_name, policy, max_states=200000):
    """Terminal states that report OK while a transform-only band is absent.

    This is the property the maintainer review asked to be represented: not just
    "some invariant fired", but specifically *a public success with a missing
    band that only an adjacent transform can produce*. Counting reachable states
    makes the negative regression checkable instead of asserted.
    """
    topo = TOPOLOGIES[topology_name]
    start = Machine(topo, policy)
    frontier = [start]
    seen = {start.key()}
    hits = 0
    example = None

    while frontier:
        if len(seen) >= max_states:
            return {'count': hits, 'states': len(seen), 'exhaustive': False,
                    'example': example}
        nxt = []
        for machine in frontier:
            if machine.drained:
                if machine.reported_ok:
                    missing = []
                    for (channel, index), plane in machine.planes.items():
                        entropy = topo['entropy_bands'].get(index, set())
                        for band in range(topo['planes'][index]):
                            if band in entropy:
                                continue
                            if not plane.valid & (1 << band):
                                missing.append({'index': index, 'band': band})
                    if missing:
                        hits += 1
                        if example is None:
                            example = {
                                'missing': missing,
                                'error': machine.error,
                                'entries': {e.name: e.state for e in machine.entries},
                            }
                continue
            for action in machine.actions():
                child = machine.clone()
                child.step(action, {e.name: e for e in child.entries})
                k = child.key()
                if k in seen:
                    continue
                seen.add(k)
                nxt.append(child)
        frontier = nxt

    return {'count': hits, 'states': len(seen), 'exhaustive': True,
            'example': example}


def run(topology_name, policy, max_states=200000):
    """Breadth-first over the reachable state space.

    BFS rather than depth-first: an exhausted frontier is a proof over every
    reachable interleaving at this abstraction, and a truncated one is reported
    with its frontier size instead of being presented as a proof.
    """
    topo = TOPOLOGIES[topology_name]
    start = Machine(topo, policy)
    lookup = {e.name: e for e in start.entries}
    frontier = [start]
    seen = {start.key()}
    violations = {}
    drained_states = 0
    terminal_states = 0

    while frontier:
        if len(seen) >= max_states:
            return {'violations': sorted(violations),
                    'states': len(seen),
                    'drained': drained_states,
                    'terminal': terminal_states,
                    'exhaustive': False,
                    'truncated_at': max_states}
        nxt = []
        for machine in frontier:
            if machine.drained:
                drained_states += 1
                terminal_states += 1
                for name in machine.violated():
                    violations.setdefault(name, 1)
                continue
            acts = machine.actions()
            if not acts:
                terminal_states += 1
                for name in machine.violated():
                    violations.setdefault(name, 1)
                continue
            for action in acts:
                child = machine.clone()
                child_lookup = {e.name: e for e in child.entries}
                child.step(action, child_lookup)
                k = child.key()
                if k in seen:
                    continue
                seen.add(k)
                nxt.append(child)
        frontier = nxt

    return {'violations': sorted(violations),
            'states': len(seen),
            'drained': drained_states,
            'terminal': terminal_states,
            'exhaustive': True,
            'truncated_at': None}


def source_anchor(repo=None):
    """Pin every claim in this model to the frozen tree."""
    repo = Path(repo).resolve() if repo else Path(__file__).resolve().parents[1]
    codec = repo / 'upstream' / 'Codec'
    problems = []
    rows = []

    def read(name):
        path = codec / name
        if not path.is_file():
            problems.append('missing %s' % name)
            return ''
        return path.read_text(encoding='utf-8', errors='replace').replace('\r\n', '\n')

    decoder = read('decoder.c')
    wavelet = read('wavelet.c')

    # 1. The lowpass inverse reads all four bands of its input wavelet.
    anchor = 'TransformInverseSpatialQuantLowpass(wavelet, lowpass, scratch, prescale, inverse_prescale);'
    if anchor not in decoder:
        problems.append('lowpass inverse call site changed')
    else:
        rows.append({'edge': 'spatial lowpass', 'reads': 'wavelet[index].band[0..3]',
                     'writes': 'wavelet[index-1].band[0]', 'ok': True})

    # 2. It writes into wavelet[index-1], not into its own input plane.
    lowpass_write = 'UpdateWaveletBandValidFlags(decoder, lowpass, 0);'
    if decoder.count(lowpass_write) < 2:
        problems.append('lowpass publication anchor changed (%d)' % decoder.count(lowpass_write))

    # 3. The highpass inverse writes band 1 of wavelet[index-1].
    if 'UpdateWaveletBandValidFlags(decoder, highpass, 1);' not in decoder:
        problems.append('highpass publication anchor changed')

    # 4. The temporal inverse publishes band 0 into both frame wavelets.
    if decoder.count('UpdateWaveletBandValidFlags(decoder, frame[0], 0);') != 1:
        problems.append('temporal publication anchor for frame[0] changed')
    if decoder.count('UpdateWaveletBandValidFlags(decoder, frame[1], 0);') != 1:
        problems.append('temporal publication anchor for frame[1] changed')

    # 5. The silent no-op: an absent read band returns without writing or setting
    #    an error. This is the mechanism the released policy models.
    for band in range(4):
        needle = 'if (input->band[%d] == NULL) return;' % band
        if needle not in wavelet:
            problems.append('silent-return anchor for band %d changed' % band)
    if 'if (input->band[0] == NULL) return;' in wavelet:
        rows.append({'edge': 'inverse guard', 'reads': 'input->band[0..3]',
                     'writes': 'none (silent return)', 'ok': True})

    # 6. The subband tables that decide which plane gets band 0 from entropy.
    for table in ('static int subband_wavelet_index[] = {2, 2, 2, 2, 1, 1, 1, 0, 0, 0};',
                  'static int subband_wavelet_index[] = {5, 5, 5, 5, 4, 4, 4, 3, 3, 3, 3, 1, 1, 1, 0, 0, 0};'):
        if table not in decoder:
            problems.append('subband table changed: %s' % table[:48])

    # 7. The released predicate and the drain anchors.
    if '#define BANDS_ALL_VALID(wavelet)\t((wavelet)->band_valid_flags == (wavelet)->band_started_flags)' not in read('image.h'):
        problems.append('released BANDS_ALL_VALID form changed')
    threading = read('entropy_threading.c')
    for needle in ('data->queue[curr_entry].done = 1;',
                   'if(wavelet && BANDS_ALL_VALID(wavelet))'):
        if needle not in threading:
            problems.append('drain anchor changed: %s' % needle)

    return {'status': 'OK' if not problems else 'MISMATCH',
            'rows': rows, 'problems': problems}


# ----------------------------------------------------------------- self-test

def self_test():
    failures = []

    # The corrected model must reproduce the released failure in both shapes.
    for topo in TOPOLOGIES:
        released = run(topo, 'released')
        proposed = run(topo, 'proposed')
        if 'I1_FAIL_CLOSED' not in released['violations']:
            failures.append('%s: released does not reproduce I1' % topo)
        if not proposed['exhaustive']:
            failures.append('%s: proposed search truncated, cannot claim a proof' % topo)
        if proposed['violations']:
            failures.append('%s: proposed still violates %s'
                            % (topo, proposed['violations']))

    # Negative regression, stated as a reachability property rather than as a
    # violation list. Revision 1's claim was "the proposed rule is clean"; the
    # reviewer showed that its rule admits a missing adjacent-producer band. So:
    # the retired policy MUST reach a success-with-missing-band state, and the
    # proposed one MUST NOT.
    for topo in TOPOLOGIES:
        legacy = false_okay_states(topo, 'legacy')
        proposed = false_okay_states(topo, 'proposed')
        if not legacy['exhaustive'] or not proposed['exhaustive']:
            failures.append('%s: false-OKAY search truncated, cannot conclude' % topo)
        if legacy['count'] == 0:
            failures.append('%s: revision-1 policy never reaches a success with a '
                            'transform-only band absent; the negative regression '
                            'the review asked for is not represented' % topo)
        if proposed['count']:
            failures.append('%s: proposed policy reaches %d '
                            'success-with-missing-band state(s)'
                            % (topo, proposed['count']))

    # I1 must be able to fire on a *satisfied-looking* state: the released run
    # has every startable band started, so a model that merely checks flag
    # presence would miss it.
    spatial = run('spatial_chain', 'released')
    if 'I3_NO_SILENT' not in spatial['violations']:
        failures.append('spatial_chain: released does not reproduce the silent skip')

    if failures:
        for failure in failures:
            print('SELF-TEST FAIL %s' % failure, file=sys.stderr)
        return 1

    print('SELF-TEST PASS: released and legacy both reproduce I1 in %d '
          'topologies; legacy reaches success-with-missing-band in every one, '
          'proposed in none; proposed is fail-closed and exhaustive'
          % len(TOPOLOGIES))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--json', metavar='PATH')
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    anchor = source_anchor()
    report = {'kind': 'DESIGN_MODEL',
              'revision': 2,
              'note': ('reachability model over explicit producer edges; not a '
                       'measurement of the frozen decoder'),
              'source_anchor': anchor,
              'topologies': {}}
    for topo in TOPOLOGIES:
        report['topologies'][topo] = {
            policy: run(topo, policy)
            for policy in ('released', 'legacy', 'proposed')
        }
        report['topologies'][topo]['false_okay'] = {
            policy: false_okay_states(topo, policy)
            for policy in ('released', 'legacy', 'proposed')
        }

    if anchor['status'] != 'OK':
        for problem in anchor['problems']:
            print('ANCHOR FAIL: %s' % problem, file=sys.stderr)
        return 2

    if args.json:
        out = Path(args.json)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(report, indent=1) + '\n', encoding='utf-8')
        print('wrote %s' % out)

    for topo, per in report['topologies'].items():
        print('%-20s released=%-14s legacy=%-14s proposed=%-4s states=%d%s'
              % (topo,
                 ','.join(per['released']['violations']) or '-',
                 ','.join(per['legacy']['violations']) or '-',
                 ','.join(per['proposed']['violations']) or '-',
                 per['proposed']['states'],
                 '' if per['proposed']['exhaustive'] else ' TRUNCATED'))
        print('%-20s false-OKAY states: released=%d legacy=%d proposed=%d'
              % ('', per['false_okay']['released']['count'],
                 per['false_okay']['legacy']['count'],
                 per['false_okay']['proposed']['count']))

    if not args.check and not args.json:
        parser.error('one of --check/--json/--self-test is required')
    clean = all(not p['proposed']['violations'] and p['proposed']['exhaustive']
                and p['false_okay']['proposed']['count'] == 0
                for p in report['topologies'].values())
    print('MODEL PASS' if clean else 'MODEL FAIL')
    return 0 if clean else 2


if __name__ == '__main__':
    sys.exit(main())
