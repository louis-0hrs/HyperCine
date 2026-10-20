#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""RCA-195 Milestone-2 design model, revision 3.

Revision 2 answered the first review (real producer edges, correct output plane,
scheduled transitions).  The second review then named three remaining fidelity
faults, all of which are real:

1. ``Machine.key()`` omitted ``entry.worker`` although that field decides which
   slot ``_free_worker`` releases, so two distinguishable interleavings could
   collapse into one BFS node.
2. The "symmetry reduction" that fired entropy events in one global canonical
   order was *asserted*, not proven; different readiness subsets can lead to
   different queue admissions.
3. ``drain`` was always available, so a fail-closed early cancellation was
   indistinguishable from forward progress, and no invariant required a valid
   decode to actually *complete*.

Revision 3 addresses each:

* the state key carries the entry-to-worker assignment, and worker assignment is
  a real branch (no "lowest free slot" shortcut);
* entropy publications are modelled as a **partial order per band** in the
  default mode, and the canonical total order is only used when
  ``confluence`` proves, over the reachable state set, that swapping any two
  independently enabled publications yields the same successor state -- so any
  interleaving can be normalised to the canonical one;
* ``execute`` and ``publish`` are separate transitions, so a consumer can be
  observed claiming between a producer's computation and its publication -- the
  window the whole contract is about;
* drain has two modes: ``anytime`` (fail-closed cancellation; safety only) and
  ``quiescent`` (only when no producer can make progress).  Under ``quiescent``
  the model proves a positive completion property: from *every* reachable state
  the policy can still reach an OKAY terminal, and at least one OKAY terminal
  exists.  Eager drain is then reported for what it is -- a safe abort, not
  liveness;
* the topology set covers three channels, FULL and HALF geometry of a
  field-plus group, the spatial highpass branch (``index == 3``) and the
  ``index > 3`` lowpass mapping (``index - 1`` / ``index - 2``).

Policies:

  released  the shipped discipline: claim on ``valid == started``, silent skip on
            a failed post-unlock re-check, silent no-op when a read band is
            absent, drain zeroes the queue without verifying.
  legacy    revision 1 of this model, kept only as a negative regression: it
            claims on the entropy mask and publishes onto the entry's own input
            plane.  It must FAIL the invariants.
  proposed  claim gated on the full published read set, generation and
            write-target ownership; a missing input at execution is terminal;
            publication is its own transition; drain verifies.

Usage:
  python tools/rca195_m2_model.py --check
  python tools/rca195_m2_model.py --json docs/validation/<file>.json
  python tools/rca195_m2_model.py --self-test
  python tools/rca195_m2_model.py --confluence spatial_chain
  python tools/rca195_m2_model.py --negative-cases
"""
import argparse
import json
import sys
from pathlib import Path

# --------------------------------------------------------------------------
# Topologies.
#
# A plane is (channel, index) with `num_bands` and a set of bands that entropy
# decoding publishes for it.  Every other band must be produced by a transform
# entry, which is the whole point: band 0 of a non-top plane is never an entropy
# output, so a model that lets entropy publish it invents a producer.
#
# Pinned to the frozen subband tables:
#   intra   decoder.c:11596  wavelet {2,2,2,2,1,1,1,0,0,0}
#                            bands   {0,1,2,3,1,2,3,1,2,3}
#   group   decoder.c:11191  wavelet {5,5,5,5,4,4,4,3,3,3,3,1,1,1,0,0,0}
#                            bands   {0,1,2,3,1,2,3,0,1,2,3,1,2,3,1,2,3}
#
# HALF resolution decodes subbands 0..10 only (codec.h:648
# CODEC_NUM_SUBBANDS_HALF == 11, mask 0x7FF), so for a group the planes 0 and 1
# high bands are never entropy-decoded and the spatial entry index 1 is never
# queued (decoder.c:12115 BANDS_ALL_STARTED is false for plane 1).  FULL
# decodes all 17 subbands and does queue index 1.  This is the real
# FULL/HALF matrix difference, not a label.
# --------------------------------------------------------------------------


def _plane_table(num_bands):
    return {index: n for index, n in num_bands}


TOPOLOGIES = {
    # Intra / spatial, FULL or HALF: the intra subband table has ten entries, so
    # both resolutions decode the same planes and the same entries.  Kept as the
    # minimal topology the reviews reason about.
    'spatial_chain': {
        'planes': {(0, 0): 4, (0, 1): 4, (0, 2): 4},
        'entropy_bands': {0: {1, 2, 3}, 1: {1, 2, 3}, 2: {0, 1, 2, 3}},
        'entries': [
            # (name, channel, index, reads, writes)
            ('E2', 0, 2, [(2, 0), (2, 1), (2, 2), (2, 3)], [(1, 0)]),
            ('E1', 0, 1, [(1, 0), (1, 1), (1, 2), (1, 3)], [(0, 0)]),
        ],
        'required_output': [(0, 0)],
        'stale_capable': True,
        'regression_target': 'E1',
    },
    # Spatial highpass branch, index == 3: writes band 1 of plane index-1
    # (decoder.c:13173) after reading all four bands of plane 3.
    'spatial_highpass': {
        'planes': {(0, 3): 4, (0, 2): 2},
        'entropy_bands': {3: {0, 1, 2, 3}},
        'entries': [
            ('E3', 0, 3, [(3, 0), (3, 1), (3, 2), (3, 3)], [(2, 1)]),
        ],
        'required_output': [(2, 1)],
        'stale_capable': True,
        'regression_target': 'E3',
    },
    # Field-plus group at HALF: subbands 0..10 only.  Entropy publishes plane 5
    # (all four), plane 4 bands 1-3 and plane 3 (all four).  Plane 4 band 0 comes
    # from index 5 (lowpass_index == index-1, decoder.c:13064), plane 2 bands 0/1
    # from index 4 (index-2) and index 3 (highpass), then the temporal inverse
    # index 2 writes band 0 into frame wavelets 0 and 1.  Index 1 is not queued.
    'fieldplus_half': {
        'planes': {(0, 0): 4, (0, 1): 4, (0, 2): 2,
                   (0, 3): 4, (0, 4): 4, (0, 5): 4},
        'entropy_bands': {0: set(), 1: set(), 2: set(),
                          3: {0, 1, 2, 3}, 4: {1, 2, 3}, 5: {0, 1, 2, 3}},
        'entries': [
            ('E5', 0, 5, [(5, 0), (5, 1), (5, 2), (5, 3)], [(4, 0)]),
            ('E4', 0, 4, [(4, 0), (4, 1), (4, 2), (4, 3)], [(2, 0)]),
            ('E3', 0, 3, [(3, 0), (3, 1), (3, 2), (3, 3)], [(2, 1)]),
            ('T2', 0, 2, [(2, 0), (2, 1)], [(0, 0), (1, 0)]),
        ],
        'required_output': [(0, 0)],
        'stale_capable': True,
        'regression_target': 'E4',
    },
    # The same group at FULL: all 17 subbands, so planes 1 and 0 receive bands
    # 1-3 from entropy in addition to band 0 from the temporal inverse.  There is
    # no index==1 entry in a field-plus transform: ReconstructWaveletBand has
    # branches for `index > 3`, `index == 3` and `index == 2` only under
    # TRANSFORM_TYPE_FIELDPLUS (decoder.c:13060/13121/13189), so the frame
    # wavelets are filled by the temporal inverse and by entropy alone.
    'fieldplus_full': {
        'planes': {(0, 0): 4, (0, 1): 4, (0, 2): 2,
                   (0, 3): 4, (0, 4): 4, (0, 5): 4},
        'entropy_bands': {0: {1, 2, 3}, 1: {1, 2, 3}, 2: set(),
                          3: {0, 1, 2, 3}, 4: {1, 2, 3}, 5: {0, 1, 2, 3}},
        'entries': [
            ('E5', 0, 5, [(5, 0), (5, 1), (5, 2), (5, 3)], [(4, 0)]),
            ('E4', 0, 4, [(4, 0), (4, 1), (4, 2), (4, 3)], [(2, 0)]),
            ('E3', 0, 3, [(3, 0), (3, 1), (3, 2), (3, 3)], [(2, 1)]),
            ('T2', 0, 2, [(2, 0), (2, 1)], [(0, 0), (1, 0)]),
        ],
        'required_output': [(0, 0)],
        'stale_capable': True,
        'regression_target': 'E3',
    },
    # Three channels: the same intra chain per channel.  The channels share only
    # the worker pool, so this topology is where per-channel ownership and
    # worker starvation are exercised.
    'spatial_three_channel': {
        'planes': {(c, i): 4 for c in (0, 1, 2) for i in (0, 1, 2)},
        'entropy_bands': {0: {1, 2, 3}, 1: {1, 2, 3}, 2: {0, 1, 2, 3}},
        'entries': [
            (name, channel, index, [((index, b)) if False else (index, b) for b in range(4)], [])
            for name, channel, index in ()
        ],
        'required_output': [(0, 0)],
        'stale_capable': True,
        # Three independent chains multiply the interleaving space; the released
        # policy claims all six entries at once (0 == 0 on every plane), so the
        # frontier is an order of magnitude larger than the single-channel case.
        'state_cap': 1200000,
        'regression_target': 'C0E1',
    },
}

# Per-channel entries for the three-channel topology, built explicitly because
# the plane keys are (channel, index) rather than (index,).
TOPOLOGIES['spatial_three_channel']['entries'] = [
    (name, channel, index, [(index, b) for b in range(4)], [(index - 1, 0)])
    for channel in (0, 1, 2)
    for name, index in (('C%dE2' % channel, 2), ('C%dE1' % channel, 1))
]

# The intra chain for a single channel is exactly spatial_chain; the per-channel
# plane table is the channel-expanded form.
TOPOLOGIES['spatial_three_channel']['planes'] = {
    (c, i): 4 for c in (0, 1, 2) for i in (0, 1, 2)
}

# Read set of the highpass branch, for the topology-check anchor.
HIGHPASS_READS = [(3, 0), (3, 1), (3, 2), (3, 3)]
HIGHPASS_WRITE = (2, 1)

TERMINAL_STATES = frozenset(('PUBLISHED', 'FAILED', 'DROPPED', 'DISCARDED'))


# --------------------------------------------------------------------- state

class Plane:
    __slots__ = ('valid', 'started', 'generation')

    def __init__(self):
        self.valid = 0
        self.started = 0
        self.generation = 0

    def bands(self, num_bands):
        return {b for b in range(num_bands) if self.valid & (1 << b)}

    def release_ok(self):
        """The shipped predicate: two flag words compared to each other."""
        return self.valid == self.started


class Entry:
    __slots__ = ('name', 'channel', 'index', 'generation', 'reads', 'writes',
                 'state', 'executions', 'worker', 'pending')

    def __init__(self, name, channel, index, reads, writes):
        self.name = name
        self.channel = channel
        self.index = index
        self.generation = 0
        self.reads = tuple(reads)
        self.writes = tuple(writes)
        self.state = 'QUEUED'
        self.executions = 0
        # The slot this entry occupies while CLAIMED/EXECUTING/COMPUTED.  Held on
        # the entry, so releasing worker N cannot free worker 0.
        self.worker = None
        # Writes staged by EXECUTE and applied by PUBLISH.  Modelling them
        # separately is what makes the claim-before-publication window visible.
        self.pending = ()


class Machine:
    def __init__(self, topology, policy, workers=2, entropy='canonical',
                 drain='anytime', withhold=frozenset(), stale=False):
        self.topo = topology
        self.policy = policy
        self.workers = workers
        self.entropy_mode = entropy
        self.drain_mode = drain
        self.withhold = frozenset(withhold)
        self.stale_enabled = stale and topology.get('stale_capable', False)
        self.busy = frozenset()
        self.planes = {key: Plane() for key in topology['planes']}
        self.entries = [Entry(*spec) for spec in topology['entries']]
        self.error = None
        self.reported_ok = None
        self.drained = False
        # (channel, index, band) events.  `started` precedes `valid` per band, as
        # in the source, which announces a band before publishing it.
        self.entropy_events = []
        for (channel, index) in sorted(self.planes, key=lambda k: (k[0], k[1])):
            for band in sorted(topology['entropy_bands'].get(index, set())):
                if (channel, index, band) in self.withhold:
                    continue
                self.entropy_events.append(('e_started', (channel, index, band)))
                self.entropy_events.append(('e_valid', (channel, index, band)))
        self.events_done = 0

    # -- helpers ---------------------------------------------------------
    def key(self):
        return (
            tuple((e.state, e.executions, e.generation, e.worker)
                  for e in self.entries),
            tuple((k, p.valid, p.started, p.generation)
                  for k, p in sorted(self.planes.items())),
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
        other.entropy_mode = self.entropy_mode
        other.drain_mode = self.drain_mode
        other.withhold = self.withhold
        other.stale_enabled = self.stale_enabled
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
            f.worker = e.worker
            f.pending = e.pending
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

    def write_conflict(self, entry):
        """A second producer of an already published band.  The contract refuses
        it instead of letting two owners write the same allocation."""
        for (plane_index, band) in entry.writes:
            if self.plane(entry.channel, plane_index).valid & (1 << band):
                return True
        for other in self.entries:
            if other is entry:
                continue
            if other.channel != entry.channel:
                continue
            if set(other.writes) & set(entry.writes):
                if other.state in ('QUEUED', 'CLAIMED', 'EXECUTING', 'COMPUTED'):
                    return True
        return False

    def claimable(self, entry):
        if entry.state != 'QUEUED':
            return False
        if self.policy == 'released':
            # The shipped predicate compares the two flag words, so a plane that
            # entropy has not touched yet (0 == 0) is "ready".
            return self.input_plane(entry).release_ok()
        if self.policy == 'legacy':
            # Revision 1: claim gated on the entropy mask only.  For a four-band
            # plane that is 0xE, which deliberately excludes band 0 -- the band
            # the adjacent transform must produce.
            plane = self.input_plane(entry)
            num_bands = self.topo['planes'][(entry.channel, entry.index)]
            mask = 0 if num_bands == 2 else ((1 << num_bands) - 1) & ~1
            return (plane.valid & mask) == mask
        return (self.reads_published(entry)
                and self.generation_ok(entry)
                and not self.write_conflict(entry))

    # -- actions ---------------------------------------------------------
    def entropy_pending(self):
        return self.events_done < len(self.entropy_events)

    def entropy_action(self):
        """The next publication, in the schedule the mode allows.

        canonical  the next event in table order (a total order over bands).
        partial    any band whose `started` has not fired, or whose `started`
                   fired but `valid` has not; this is the per-band partial order.
        """
        if not self.entropy_pending():
            return None
        if self.entropy_mode == 'canonical':
            return self.entropy_events[self.events_done]
        pending = self.entropy_events[self.events_done:]
        for event in pending:
            kind, (channel, index, band) = event
            plane = self.plane(channel, index)
            bit = 1 << band
            if kind == 'e_started' and not plane.started & bit:
                return event
            if kind == 'e_valid' and plane.started & bit and not plane.valid & bit:
                return event
        return None

    def productive_actions(self):
        acts = []
        free = [w for w in range(self.workers) if w not in self.busy]
        for entry in self.entries:
            if entry.state == 'QUEUED' and free and self.claimable(entry):
                for w in free:
                    acts.append(('claim', (entry.name, w)))
            elif entry.state == 'CLAIMED':
                acts.append(('admit', entry.name))
            elif entry.state == 'EXECUTING':
                acts.append(('execute', entry.name))
            elif entry.state == 'COMPUTED' and self.policy == 'proposed':
                acts.append(('publish', entry.name))
        event = self.entropy_action()
        if event is not None:
            acts.append(event)
        if self.stale_enabled and self.policy == 'proposed':
            for (channel, index), plane in self.planes.items():
                if plane.generation == 0:
                    acts.append(('stale', (channel, index)))
        return acts

    def actions(self):
        if self.drained:
            return []
        acts = self.productive_actions()
        if self.drain_mode == 'anytime' or not acts:
            acts.append(('drain', None))
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
            entry.worker = worker
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
                    entry.state = 'PUBLISHED'
                    entry.executions += 1
                else:
                    # wavelet.c returns silently when a read band is absent.
                    entry.state = 'NOOP'
                self._free_worker(entry)
            elif self.policy == 'legacy':
                # Revision 1 published onto the entry's own input plane instead
                # of onto plane(index-1), fabricating the band it should miss.
                self.plane(entry.channel, entry.index).valid |= 1
                entry.state = 'PUBLISHED'
                entry.executions += 1
                self._free_worker(entry)
            else:
                if self.reads_published(entry) and self.generation_ok(entry):
                    entry.pending = entry.writes
                    entry.state = 'COMPUTED'     # computed, not yet published
                else:
                    self._fail(entry, 'input-missing-at-execute')
            return
        if kind == 'publish':
            entry = entry_lookup[payload]
            if self.generation_ok(entry) and self.reads_published(entry):
                self._apply_writes(entry)
                entry.state = 'PUBLISHED'
                entry.executions += 1
                self._free_worker(entry)
            else:
                self._fail(entry, 'stale-before-publication')
            return

    def _free_worker(self, entry):
        if entry.worker is not None:
            self.busy = frozenset(w for w in self.busy if w != entry.worker)
            entry.worker = None

    def _apply_writes(self, entry):
        for (plane_index, band) in entry.writes:
            self.plane(entry.channel, plane_index).valid |= 1 << band
        entry.pending = ()

    def _fail(self, entry, why):
        entry.state = 'FAILED'
        if self.error is None:
            self.error = 'CODEC_ERROR_BAD_FRAME(%s:%s)' % (entry.name, why)
        self._free_worker(entry)

    def drain(self):
        self.drained = True
        if self.policy in ('released', 'legacy'):
            # Zeroed without inspection, no completeness check, no error.
            for entry in self.entries:
                if entry.state not in TERMINAL_STATES:
                    entry.state = 'DISCARDED'
            self.reported_ok = True
            return
        live = [e for e in self.entries if e.state not in TERMINAL_STATES]
        if live:
            if self.error is None:
                self.error = ('CODEC_ERROR_BAD_FRAME(incomplete:%s)'
                              % ','.join(e.name for e in live))
            self.reported_ok = False
            return
        self.reported_ok = self.error is None

    # -- invariants ------------------------------------------------------
    def required_bands(self):
        """Closure over `required_output` through the entries' read sets.

        Every band needed for the output path must exist at a public OKAY,
        whether entropy or a transform produced it.
        """
        need = set()
        todo = [(0, index, band)
                for (index, band) in self.topo['required_output']]
        by_write = {}
        for entry in self.entries:
            for target in entry.writes:
                by_write.setdefault((entry.channel,) + target, []).append(entry)
        while todo:
            item = todo.pop()
            if item in need:
                continue
            need.add(item)
            for entry in by_write.get(item, []):
                todo.extend((entry.channel, i, b) for (i, b) in entry.reads)
        return need

    def missing_required(self):
        missing = []
        for (channel, plane_index, band) in sorted(self.required_bands()):
            if not self.plane(channel, plane_index).valid & (1 << band):
                missing.append({'channel': channel, 'index': plane_index,
                                'band': band})
        return missing

    def violated(self):
        bad = []
        ok = bool(self.reported_ok)

        if ok:
            # I1: a public OK with a band the output path requires missing.
            if self.missing_required():
                bad.append('I1_FAIL_CLOSED')
            # I1b: a public OK with an entry that never published.
            for entry in self.entries:
                if entry.state in ('DROPPED', 'NOOP', 'FAILED', 'DISCARDED'):
                    bad.append('I1_FAIL_CLOSED')
                    break
                if entry.state == 'PUBLISHED' and entry.executions != 1:
                    bad.append('I2_NO_DOUBLE')
                    break

        # I2: nothing executed twice.
        if any(e.executions > 1 for e in self.entries):
            if 'I2_NO_DOUBLE' not in bad:
                bad.append('I2_NO_DOUBLE')

        # I3: nothing left the queue unexecuted and unreported.
        if ok and self.error is None:
            for entry in self.entries:
                if entry.state in ('DROPPED', 'DISCARDED', 'NOOP'):
                    bad.append('I3_NO_SILENT')
                    break

        # I5: no entry may starve while a worker is free and the entry is
        # claimable -- evaluated at every drain, because that is when the queue is
        # discarded.  A claimable entry that could have run and did not is a
        # forward-progress failure, distinct from a missing input.
        if ok:
            free = self.workers - len(self.busy)
            for entry in self.entries:
                if entry.state in ('DROPPED', 'DISCARDED', 'QUEUED'):
                    if free > 0 and self.claimable(entry):
                        bad.append('I5_PROGRESS')
                        break

        # I6: an entry that was claimed and then lost its generation must be a
        # terminal error, never a silent success.
        if ok:
            for entry in self.entries:
                if entry.state == 'QUEUED' and entry.generation != \
                        self.plane(entry.channel, entry.index).generation:
                    bad.append('I6_OWNERSHIP')
                    break

        # I7: a band may not have two live producers.
        if ok:
            owners = {}
            for entry in self.entries:
                if entry.state in ('QUEUED', 'CLAIMED', 'EXECUTING', 'COMPUTED'):
                    for target in entry.writes:
                        owners.setdefault(target, []).append(entry.name)
            if any(len(v) > 1 for v in owners.values()):
                bad.append('I7_SINGLE_OWNER')

        # I4 is a property of the search, not of one terminal state: recorded by
        # run() as `bounded`.  A truncated search reports I4_UNPROVEN.
        if self.error is not None and self.reported_ok is False:
            keep = {'I2_NO_DOUBLE', 'I5_PROGRESS', 'I6_OWNERSHIP', 'I7_SINGLE_OWNER'}
            bad = [b for b in bad if b in keep]
        return bad


# ------------------------------------------------------------------- search

def explore(topology_name, policy, max_states=400000, workers=2,
            entropy='canonical', drain='anytime', withhold=frozenset(),
            stale=False, collect_edges=False):
    """Breadth-first over the reachable state space.

    An exhausted frontier is a proof over every reachable interleaving at this
    abstraction; a truncated one is reported with its frontier size and labelled
    `exhaustive: false` rather than presented as a proof.
    """
    topo = TOPOLOGIES[topology_name]
    max_states = max(max_states, topo.get('state_cap', 0))
    start = Machine(topo, policy, workers=workers, entropy=entropy, drain=drain,
                    withhold=withhold, stale=stale)
    frontier = [start]
    seen = {start.key()}
    edges = {}
    violations = {}
    drained_states = 0
    terminal_states = 0
    ok_states = 0
    deadlocks = 0

    while frontier:
        if len(seen) >= max_states:
            return {'violations': sorted(violations) + ['I4_UNPROVEN'],
                    'states': len(seen), 'drained': drained_states,
                    'terminal': terminal_states, 'ok': ok_states,
                    'deadlocks': deadlocks, 'exhaustive': False,
                    'bounded': False, 'truncated_at': max_states,
                    'edges': edges if collect_edges else None}
        nxt = []
        for machine in frontier:
            if machine.drained:
                drained_states += 1
                terminal_states += 1
                if machine.reported_ok:
                    ok_states += 1
                for name in machine.violated():
                    violations.setdefault(name, 1)
                if collect_edges:
                    edges[machine.key()] = []
                continue
            acts = machine.actions()
            if not acts:
                # A non-drained state with no action is a deadlock: a
                # forward-progress failure rather than a bound.
                deadlocks += 1
                terminal_states += 1
                violations.setdefault('I5_PROGRESS', 1)
                for name in machine.violated():
                    violations.setdefault(name, 1)
                if collect_edges:
                    edges[machine.key()] = []
                continue
            children = []
            for action in acts:
                child = machine.clone()
                child_lookup = {e.name: e for e in child.entries}
                child.step(action, child_lookup)
                k = child.key()
                children.append(k)
                if k in seen:
                    continue
                seen.add(k)
                nxt.append(child)
            if collect_edges:
                edges[machine.key()] = children
        frontier = nxt

    return {'violations': sorted(violations), 'states': len(seen),
            'drained': drained_states, 'terminal': terminal_states,
            'ok': ok_states, 'deadlocks': deadlocks, 'exhaustive': True,
            'bounded': True, 'truncated_at': None,
            'edges': edges if collect_edges else None}


def false_okay_states(topology_name, policy, max_states=400000, workers=2,
                      entropy='canonical', withhold=frozenset()):
    """Terminal states that report OK while a required band is absent.

    This is the property the review asked to be represented: not just "some
    invariant fired", but specifically a public success with a band missing from
    the output path's closure.
    """
    topo = TOPOLOGIES[topology_name]
    max_states = max(max_states, topo.get('state_cap', 0))
    start = Machine(topo, policy, workers=workers, entropy=entropy,
                    withhold=withhold)
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
                    missing = machine.missing_required()
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


def liveness(topology_name, policy='proposed', max_states=400000, workers=2,
             entropy='canonical'):
    """Positive completion under a quiescent-only drain.

    Every reachable state must still be able to reach an OKAY terminal: the
    policy may not walk a valid decode into a failure by aborting early.  This is
    the invariant the review's point (3) asked for, and it is only meaningful
    when `drain` is not available as an escape hatch.
    """
    result = explore(topology_name, policy, max_states=max_states,
                     workers=workers, entropy=entropy, drain='quiescent',
                     collect_edges=True)
    if not result['exhaustive']:
        return {'exhaustive': False, 'states': result['states'],
                'can_reach_ok': None, 'stranded': None,
                'violations': ['I4_UNPROVEN']}
    edges = result['edges']
    ok_keys = {k for k, succ in edges.items()
               if not succ and _reports_ok(topology_name, policy, k, workers,
                                           entropy)}
    # Reverse reachability to an OKAY terminal.
    reverse = {}
    for src, succ in edges.items():
        for dst in succ:
            reverse.setdefault(dst, []).append(src)
    good = set(ok_keys)
    stack = list(ok_keys)
    while stack:
        node = stack.pop()
        for parent in reverse.get(node, ()):
            if parent not in good:
                good.add(parent)
                stack.append(parent)
    stranded = [k for k in edges if k not in good]
    violations = []
    if not ok_keys:
        violations.append('I6_NO_COMPLETION')
    if stranded:
        violations.append('I6_STRANDED')
    return {'exhaustive': True, 'states': result['states'],
            'ok_terminals': len(ok_keys), 'can_reach_ok': len(good),
            'stranded': len(stranded), 'stranded_example': stranded[:3],
            'violations': violations}


def _reports_ok(topology_name, policy, key, workers, entropy):
    """Recover `reported_ok` from a state key (last three fields)."""
    return key[-2] is True


def confluence(topology_name, policy='proposed', max_states=400000, workers=2):
    """Check that independently enabled entropy publications commute.

    The canonical schedule is only a valid reduction if swapping any two
    publications that are both enabled leaves the same successor state.  This is
    checked on every reachable state; a counterexample is reported instead of
    assumed away.
    """
    topo = TOPOLOGIES[topology_name]
    start = Machine(topo, policy, workers=workers, entropy='partial')
    frontier = [start]
    seen = {start.key()}
    checked = 0
    counterexample = None
    truncated = False

    while frontier:
        if len(seen) >= max_states:
            truncated = True
            break
        nxt = []
        for machine in frontier:
            if machine.drained:
                continue
            enabled = []
            for event in machine.entropy_events[machine.events_done:]:
                kind, (channel, index, band) = event
                plane = machine.plane(channel, index)
                bit = 1 << band
                if kind == 'e_started' and not plane.started & bit:
                    enabled.append(event)
                elif kind == 'e_valid' and plane.started & bit \
                        and not plane.valid & bit:
                    enabled.append(event)
            for i in range(len(enabled)):
                for j in range(i + 1, len(enabled)):
                    first, second = enabled[i], enabled[j]
                    a = machine.clone()
                    a.step(first, {e.name: e for e in a.entries})
                    a.step(second, {e.name: e for e in a.entries})
                    b = machine.clone()
                    b.step(second, {e.name: e for e in b.entries})
                    b.step(first, {e.name: e for e in b.entries})
                    checked += 1
                    if a.key() != b.key():
                        if counterexample is None:
                            counterexample = {
                                'state': list(machine.key()[:1]),
                                'first': first[0] + ':' + repr(first[1]),
                                'second': second[0] + ':' + repr(second[1]),
                            }
            for action in machine.actions():
                child = machine.clone()
                child.step(action, {e.name: e for e in child.entries})
                k = child.key()
                if k in seen:
                    continue
                seen.add(k)
                nxt.append(child)
        frontier = nxt

    return {'pairs_checked': checked, 'states': len(seen),
            'exhaustive': not truncated, 'counterexample': counterexample}


# ------------------------------------------------------------ negative cases

NEGATIVE_CASES = [
    # (a) spatial index 1 with the entropy mask satisfied but the adjacent
    #     producer's band 0 absent must not be claimable.
    {'name': 'spatial_index1_entropy_mask_without_producer',
     'topology': 'spatial_chain', 'policy': 'proposed',
     'withhold': [(0, 2, 0)], 'entry': 'E1', 'expect_claims': 0},
    # The retired revision-1 rule must claim in exactly that state.
    {'name': 'legacy_claims_without_producer',
     'topology': 'spatial_chain', 'policy': 'legacy',
     'withhold': [(0, 2, 0)], 'entry': 'E1', 'expect_claims_min': 1},
    # (b) temporal index 2 with neither input band published must not be
    #     claimable (its required mask is 0 in the old rule).
    {'name': 'temporal_without_inputs',
     'topology': 'fieldplus_half', 'policy': 'proposed',
     'withhold': [(0, 3, 0), (0, 4, 0), (0, 5, 0)], 'entry': 'T2',
     'expect_claims': 0},
    {'name': 'legacy_temporal_without_inputs',
     'topology': 'fieldplus_half', 'policy': 'legacy',
     'withhold': [(0, 3, 0), (0, 4, 0), (0, 5, 0)], 'entry': 'T2',
     'expect_claims_min': 1},
]


def claims_seen(topology_name, policy, withhold, entry_name, max_states=200000):
    """Number of reachable states in which `entry_name` holds a claim."""
    topo = TOPOLOGIES[topology_name]
    start = Machine(topo, policy, withhold=frozenset(withhold))
    frontier = [start]
    seen = {start.key()}
    hits = 0
    while frontier:
        if len(seen) >= max_states:
            return hits, len(seen), False
        nxt = []
        for machine in frontier:
            for e in machine.entries:
                if e.name == entry_name and e.state in \
                        ('CLAIMED', 'EXECUTING', 'COMPUTED', 'PUBLISHED'):
                    hits += 1
                    break
            if machine.drained:
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
    return hits, len(seen), True


def negative_cases():
    failures = []
    rows = []
    for case in NEGATIVE_CASES:
        hits, states, exhaustive = claims_seen(
            case['topology'], case['policy'], case['withhold'], case['entry'])
        low = case.get('expect_claims_min')
        if not exhaustive:
            failures.append('%s: search truncated' % case['name'])
            continue
        if low is None:
            good = hits == case['expect_claims']
        else:
            good = hits >= low
        rows.append({'case': case['name'], 'claims': hits, 'states': states,
                     'ok': good})
        if not good:
            failures.append('%s: %d claims (expected %s)'
                            % (case['name'], hits,
                               case.get('expect_claims', '>=%s' % low)))
    # (c) a claimed-then-stale entry must fail closed.
    stale = explore('spatial_chain', 'proposed', stale=True, drain='quiescent')
    if not stale['exhaustive']:
        failures.append('stale-generation: search truncated')
    elif 'I1_FAIL_CLOSED' in stale['violations']:
        failures.append('stale-generation: a stale entry still reached OKAY')
    elif stale['ok'] == 0:
        failures.append('stale-generation: no scenario completed at all')
    # (d) duplicate producer: a second owner of an already published band is
    #     refused, so no state reports two live producers.
    dup = explore('spatial_chain', 'proposed')
    if 'I7_SINGLE_OWNER' in dup['violations']:
        failures.append('single-owner: two live producers of one band')
    # (e) four workers on unsatisfiable entries: drain fails closed.
    starve = explore('fieldplus_half', 'proposed', workers=4,
                     withhold=[(0, 5, 0), (0, 4, 0), (0, 3, 0)])
    if starve['ok']:
        failures.append('starvation: an unsatisfiable decode reported OKAY')
    rows.append({'case': 'stale_generation_fail_closed',
                 'claims': stale.get('ok'), 'states': stale.get('states'),
                 'ok': 'I1_FAIL_CLOSED' not in stale['violations']})
    rows.append({'case': 'four_worker_starvation',
                 'claims': starve.get('ok'), 'states': starve.get('states'),
                 'ok': starve.get('ok') == 0})
    return rows, failures


# --------------------------------------------------------------- anchoring

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
    image_h = read('image.h')
    threading = read('entropy_threading.c')
    codec_h = read('codec.h')

    # 1. The lowpass inverse reads all four bands of its input wavelet and writes
    #    band 0 of the *next lower* plane.
    anchor = ('TransformInverseSpatialQuantLowpass(wavelet, lowpass, scratch, '
              'prescale, inverse_prescale);')
    if anchor not in decoder:
        problems.append('lowpass inverse call site changed')
    else:
        rows.append({'edge': 'spatial lowpass', 'reads': 'wavelet[index].band[0..3]',
                     'writes': 'wavelet[index-1].band[0]', 'ok': True})
    if decoder.count('UpdateWaveletBandValidFlags(decoder, lowpass, 0);') < 2:
        problems.append('lowpass publication anchor changed')
    if 'int lowpass_index = (index > 4) ? index - 1 : index - 2;' not in decoder:
        problems.append('index>3 lowpass mapping changed')

    # 2. The highpass branch writes band 1 of plane index-1.
    if 'UpdateWaveletBandValidFlags(decoder, highpass, 1);' not in decoder:
        problems.append('highpass publication anchor changed')
    else:
        rows.append({'edge': 'spatial highpass', 'reads': 'wavelet[3].band[0..3]',
                     'writes': 'wavelet[2].band[1]', 'ok': True})

    # 3. The temporal inverse reads both bands and writes band 0 into both frame
    #    wavelets; its guard is assert-only in Release.
    if decoder.count('UpdateWaveletBandValidFlags(decoder, frame[0], 0);') != 1:
        problems.append('temporal publication anchor for frame[0] changed')
    if decoder.count('UpdateWaveletBandValidFlags(decoder, frame[1], 0);') != 1:
        problems.append('temporal publication anchor for frame[1] changed')
    if 'assert(BANDS_ALL_VALID(temporal));' not in decoder:
        problems.append('temporal assert-only guard changed')
    else:
        rows.append({'edge': 'temporal', 'reads': 'temporal->band[0..1]',
                     'writes': 'frame[0..1].band[0]', 'ok': True})

    # 4. The silent no-op of the spatial inverses.
    for band in range(4):
        needle = 'if (input->band[%d] == NULL) return;' % band
        if needle not in wavelet:
            problems.append('silent-return anchor for band %d changed' % band)
    if 'if (input->band[0] == NULL) return;' in wavelet:
        rows.append({'edge': 'inverse guard', 'reads': 'input->band[0..3]',
                     'writes': 'none (silent return)', 'ok': True})

    # 5. The subband tables decide which plane receives band 0 from entropy.
    intra = 'static int subband_wavelet_index[] = {2, 2, 2, 2, 1, 1, 1, 0, 0, 0};'
    group = ('static int subband_wavelet_index[] = '
             '{5, 5, 5, 5, 4, 4, 4, 3, 3, 3, 3, 1, 1, 1, 0, 0, 0};')
    for table in (intra, group):
        if table not in decoder:
            problems.append('subband table changed: %s' % table[:48])

    # 6. HALF decodes subbands 0..10 only; that is the FULL/HALF difference the
    #    topology matrix models.
    if '#define CODEC_NUM_SUBBANDS_HALF\t\t11' not in codec_h:
        problems.append('CODEC_NUM_SUBBANDS_HALF changed')
    if 'DECODED_SUBBAND_MASK_HALF' not in codec_h:
        problems.append('DECODED_SUBBAND_MASK_HALF missing')
    if 'return ((codec->decoded_subband_flags & DECODED_SUBBAND_MASK_HALF) == DECODED_SUBBAND_MASK_HALF);' not in decoder:
        problems.append('CanSkipChannel HALF branch changed')

    # 7. The released predicate, the queue ownership token and the drain anchors.
    if '#define BANDS_ALL_VALID(wavelet)\t((wavelet)->band_valid_flags == (wavelet)->band_started_flags)' not in image_h:
        problems.append('released BANDS_ALL_VALID form changed')
    if '#define BANDS_ALL_STARTED(wavelet)' not in image_h:
        problems.append('BANDS_ALL_STARTED missing')
    for needle in ('data->queue[curr_entry].done = 1;',
                   'if(wavelet && BANDS_ALL_VALID(wavelet))',
                   'decoder->transform_queue.num_entries = 0;'):
        if needle not in threading and needle not in decoder:
            problems.append('drain anchor changed: %s' % needle)

    # 8. The output boundary is hollow in Release: it only tests non-NULL.
    if 'if(wavelet == NULL)' not in decoder:
        problems.append('AllTransformBandsValid form changed')
    else:
        rows.append({'edge': 'output boundary', 'reads': 'wavelet != NULL only',
                     'writes': 'none', 'ok': True})

    return {'status': 'OK' if not problems else 'MISMATCH',
            'rows': rows, 'problems': problems}


# ----------------------------------------------------------------- self-test

def self_test():
    failures = []

    # The corrected model must reproduce the released failure in both shapes and
    # the proposed one must clear every topology it claims to cover.
    for topo in TOPOLOGIES:
        released = explore(topo, 'released')
        proposed = explore(topo, 'proposed')
        if 'I1_FAIL_CLOSED' not in released['violations']:
            failures.append('%s: released does not reproduce I1' % topo)
        if not proposed['exhaustive']:
            failures.append('%s: proposed search truncated, cannot claim a proof'
                            % topo)
        if proposed['violations']:
            failures.append('%s: proposed still violates %s'
                            % (topo, proposed['violations']))

    # Negative regression as a reachability property: the retired policy MUST
    # reach a success with a required band absent, the proposed one MUST NOT.
    for topo in TOPOLOGIES:
        legacy = false_okay_states(topo, 'legacy')
        proposed = false_okay_states(topo, 'proposed')
        if not legacy['exhaustive'] or not proposed['exhaustive']:
            failures.append('%s: false-OKAY search truncated' % topo)
        if legacy['count'] == 0:
            failures.append('%s: revision-1 policy never reaches a success with '
                            'a required band absent' % topo)
        if proposed['count']:
            failures.append('%s: proposed reaches %d success-with-missing-band '
                            'state(s)' % (topo, proposed['count']))

    # Positive completion: under a quiescent drain no valid decode may be
    # stranded, and at least one OKAY terminal must exist.
    for topo in TOPOLOGIES:
        live = liveness(topo, 'proposed')
        if not live['exhaustive']:
            failures.append('%s: liveness search truncated' % topo)
        elif live['violations']:
            failures.append('%s: liveness violated %s'
                            % (topo, live['violations']))

    # Confluence of the canonical entropy schedule.
    for topo in TOPOLOGIES:
        conf = confluence(topo, 'proposed', max_states=60000)
        if not conf['exhaustive']:
            failures.append('%s: confluence search truncated' % topo)
        if conf['counterexample']:
            failures.append('%s: entropy publications do not commute (%s)'
                            % (topo, conf['counterexample']))

    # The named negative cases.
    _rows, case_failures = negative_cases()
    failures.extend(case_failures)

    # I1 must be able to fire on a satisfied-looking state: the released run has
    # every startable band started, so a flag-presence check alone would miss it.
    spatial = explore('spatial_chain', 'released')
    if 'I3_NO_SILENT' not in spatial['violations']:
        failures.append('spatial_chain: released does not reproduce the silent skip')

    if failures:
        for failure in failures:
            print('SELF-TEST FAIL %s' % failure, file=sys.stderr)
        return 1

    print('SELF-TEST PASS: %d topologies; released and legacy both reproduce I1, '
          'legacy reaches success-with-missing-band in every one and proposed in '
          'none; proposed completes under a quiescent drain; entropy publications '
          'commute' % len(TOPOLOGIES))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--json', metavar='PATH')
    parser.add_argument('--confluence', metavar='TOPOLOGY')
    parser.add_argument('--negative-cases', action='store_true')
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    if args.confluence:
        report = confluence(args.confluence)
        print(json.dumps(report, indent=1))
        return 0 if report['exhaustive'] and not report['counterexample'] else 2

    if args.negative_cases:
        rows, failures = negative_cases()
        for row in rows:
            print('%-42s claims=%-4s states=%-6s %s'
                  % (row['case'], row['claims'], row['states'],
                     'OK' if row['ok'] else 'FAIL'))
        for failure in failures:
            print('NEGATIVE FAIL %s' % failure, file=sys.stderr)
        return 0 if not failures else 2

    anchor = source_anchor()
    report = {'kind': 'DESIGN_MODEL',
              'revision': 3,
              'note': ('reachability model over explicit producer edges and '
                       'per-band publication; not a measurement of the frozen '
                       'decoder'),
              'source_anchor': anchor,
              'topologies': {}}
    negatives, _failures = negative_cases()
    report['negative_cases'] = negatives
    for topo in TOPOLOGIES:
        report['topologies'][topo] = {
            policy: explore(topo, policy)
            for policy in ('released', 'legacy', 'proposed')
        }
        report['topologies'][topo]['false_okay'] = {
            policy: false_okay_states(topo, policy)
            for policy in ('released', 'legacy', 'proposed')
        }
        report['topologies'][topo]['liveness'] = liveness(topo, 'proposed')
        report['topologies'][topo]['confluence'] = confluence(
            topo, 'proposed', max_states=60000)

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
        print('%-24s released=%-14s legacy=%-14s proposed=%-4s states=%d%s'
              % (topo,
                 ','.join(per['released']['violations']) or '-',
                 ','.join(per['legacy']['violations']) or '-',
                 ','.join(per['proposed']['violations']) or '-',
                 per['proposed']['states'],
                 '' if per['proposed']['exhaustive'] else ' TRUNCATED'))
        print('%-24s false-OKAY: released=%d legacy=%d proposed=%d | '
              'liveness ok=%s stranded=%s | confluence pairs=%d conflict=%s'
              % ('', per['false_okay']['released']['count'],
                 per['false_okay']['legacy']['count'],
                 per['false_okay']['proposed']['count'],
                 per['liveness'].get('ok_terminals'),
                 per['liveness'].get('stranded'),
                 per['confluence']['pairs_checked'],
                 bool(per['confluence']['counterexample'])))

    if not args.check and not args.json:
        parser.error('one of --check/--json/--self-test/--confluence/'
                     '--negative-cases is required')
    clean = all(not p['proposed']['violations'] and p['proposed']['exhaustive']
                and p['false_okay']['proposed']['count'] == 0
                and not p['liveness']['violations']
                and not p['confluence']['counterexample']
                for p in report['topologies'].values())
    print('MODEL PASS' if clean else 'MODEL FAIL')
    return 0 if clean else 2


if __name__ == '__main__':
    sys.exit(main())
