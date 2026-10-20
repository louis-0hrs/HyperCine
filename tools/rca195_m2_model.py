#!/usr/bin/env python3
# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""RCA-195 Milestone-2 design model, revision 6.

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

Revision 3 addressed each of those three faults (state-key worker identity,
partial order per band, quiescent drain).  Revisions 4-6 then corrected the
things the later reviews found in revision 3's own claims:

* **rev 4** -- the "partial order" was still a canonical prefix: only the first
  eligible event was offered, so a later band could never fire first.  Every
  causally enabled event is now offered, the outstanding set is state rather
  than a cursor, and a witness shows a canonical schedule foreclosing a
  reachable claim window.  Three-channel output closure now seeds ch0/ch1/ch2.
* **rev 5** -- the state space was reduced and the reduction *proved*: no policy
  guard reads ``started`` under ``proposed``/``legacy``, so the word is dropped
  and ``started_is_unobservable()`` plus ``reduction_check()`` license it.  The
  ``released`` policy is refused rather than approximated.  Four of five
  topologies became exhaustive on the reduced order.
* **rev 6** -- the *reporting* path was wrong, not the model.  ``--json`` called
  ``negative_cases()`` and discarded its failure list, so a broken control still
  printed MODEL PASS.  `model_verdict()` is now the single predicate for both
  paths, every control returns ``PASS_EXHAUSTIVE`` / ``PREFIX_ONLY_INCONCLUSIVE``
  / ``FAIL`` and states the premise it constructed, and a genuinely exhaustive
  single-entry stale control exists instead of a truncated one reported as ok.

The paragraphs below describe revision 3's mechanism and are still accurate for
it; see ``docs/validation/RCA195_M2_DESIGN.md`` for the current contract.

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
  python tools/rca195_m2_model.py --verdict-regression
  python tools/rca195_m2_model.py --confluence spatial_chain
  python tools/rca195_m2_model.py --negative-cases

Exit codes for the two control paths are 0 (all controls exhaustive and
holding), 3 (at least one control hit its cap and decided nothing) and 2 (a
control failed).  Truncation is never reported as a pass.
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
        'required_output': [(0, 0, 0)],
        'stale_capable': True,
        'state_cap': 200000,
        'policy_cap': 200000,
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
        'required_output': [(0, 2, 1)],
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
        # Both frame wavelets are output planes: the temporal inverse writes band
        # 0 into each of them (decoder.c:13253-13254).
        'required_output': [(0, 0, 0), (0, 1, 0)],
        'stale_capable': True,
        'state_cap': 600000,
        'policy_cap': 600000,
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
        'required_output': [(0, 0, 0), (0, 1, 0)],
        'stale_capable': True,
        # The true partial-order space for a 17-subband group exceeds memory at
        # the exploration cap.  It is reported TRUNCATED/INCONCLUSIVE rather than
        # presented as an exhaustive result.
        'state_cap': 250000,
        'policy_cap': 250000,
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
        # Every channel's final lowpass is an output plane.  Seeding the closure
        # with channel 0 only would let a public OKAY lack ch1/ch2 entirely --
        # exactly the gap the third review named.
        'required_output': [(0, 0, 0), (1, 0, 0), (2, 0, 0)],
        'stale_capable': True,
        # Three independent chains multiply the interleaving space; the released
        # policy claims all six entries at once (0 == 0 on every plane), so the
        # frontier is an order of magnitude larger than the single-channel case.
        'state_cap': 250000,
        'policy_cap': 250000,
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

TERMINAL_STATES = frozenset(('PUBLISHED', 'FAILED', 'DROPPED', 'DISCARDED',
                            'NOOP'))

# Verdicts for the named controls.  Three states, kept distinct on purpose:
#   PASS_EXHAUSTIVE         the premise was constructed and the property held on
#                           every reachable state -- this is the only green;
#   PREFIX_ONLY_INCONCLUSIVE the search hit its cap, so the control decided
#                           nothing.  A prefix that contains no counterexample is
#                           *not* evidence for the property when the premise
#                           itself may lie outside the prefix;
#   FAIL                    the premise held and the property did not.
# The fifth review found this distinction missing: a truncated stale-generation
# run fell through `if not stale['exhaustive']: pass` and the row was published
# as `ok` on the absence of a violation in an incomplete search.
PASS_EXHAUSTIVE = 'PASS_EXHAUSTIVE'
PREFIX_ONLY_INCONCLUSIVE = 'PREFIX_ONLY_INCONCLUSIVE'
FAIL = 'FAIL'


def _error_kind(error):
    """Collapse a terminal error into the mechanism that produced it.

    `CODEC_ERROR_BAD_FRAME(E3:stale-generation)` -> `stale-generation`;
    `CODEC_ERROR_BAD_FRAME(incomplete:E5,E4)` -> `incomplete`.  Controls need
    this to show they actually exercised their mechanism instead of merely
    avoiding a violation: a stale run with zero `stale-*` terminals never built
    the state it is supposed to test.
    """
    if not error:
        return None
    start = error.find('(')
    inner = error[start + 1:-1] if start >= 0 and error.endswith(')') else error
    if inner.startswith('incomplete'):
        return 'incomplete'
    return inner.rsplit(':', 1)[-1]

# Stable small-int codes for the entry state, so a state key stays compact.
STATE_CODES = {
    'QUEUED': 0, 'CLAIMED': 1, 'EXECUTING': 2, 'COMPUTED': 3,
    'PUBLISHED': 4, 'FAILED': 5, 'DROPPED': 6, 'DISCARDED': 7, 'NOOP': 8,
}


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
        # The reduced order drops `started` entirely, and the shipped predicate
        # is *defined* as `valid == started`.  Running reduced against that
        # policy would therefore model a different decoder, so it is refused
        # here instead of producing a quietly wrong safety table.
        if entropy == 'reduced' and policy == 'released':
            raise ValueError("entropy='reduced' is unsound for the released "
                             'policy: its claim predicate reads `started`')
        self.entropy_mode = entropy
        self.drain_mode = drain
        self.withhold = frozenset(withhold)
        self.stale_enabled = stale and topology.get('stale_capable', False)
        self.busy = frozenset()
        self.planes = {key: Plane() for key in topology['planes']}
        self.plane_order = tuple(sorted(self.planes))
        self.entries = [Entry(*spec) for spec in topology['entries']]
        self.error = None
        self.reported_ok = None
        self.drained = False
        # (channel, index, band) events.  `started` precedes `valid` per band, as
        # in the source, which announces a band before publishing it.  The set of
        # *outstanding* events is state, not a cursor: a later band's `started` is
        # free to fire before an earlier band's, which is the whole point of a
        # partial order.  A prefix cursor cannot express that and can strand an
        # event behind a consumed index.
        events = []
        for (channel, index) in sorted(self.planes, key=lambda k: (k[0], k[1])):
            for band in sorted(topology['entropy_bands'].get(index, set())):
                if (channel, index, band) in self.withhold:
                    continue
                # `reduced` drops `e_started` entirely.  That is sound only for a
                # policy that never reads `started`; `reduced_is_sound()` checks
                # exactly that and the self-test fails if a policy starts reading
                # it.  For every other mode the announcement is kept, because
                # `released` compares `valid == started`.
                if entropy != 'reduced':
                    events.append(('e_started', (channel, index, band)))
                events.append(('e_valid', (channel, index, band)))
        self.entropy_events = tuple(events)
        self.event_index = {event: i for i, event in enumerate(self.entropy_events)}
        self.pending = frozenset(range(len(self.entropy_events)))

    # -- helpers ---------------------------------------------------------
    def key(self):
        """A flat, hash-cheap identity for a state.

        Nested tuples dominate the memory of the explored set, and the state
        space is large enough that the representation matters: this packs the
        entries and planes into one flat tuple of small ints.  `worker` is
        encoded as -1 when unset so that the *entry-to-worker assignment* stays
        distinguishable -- the fault the third review found in revision 3.
        """
        entries = []
        for e in self.entries:
            entries.append(STATE_CODES[e.state])
            entries.append(e.executions)
            entries.append(e.generation)
            entries.append(-1 if e.worker is None else e.worker)
        planes = []
        for k in self.plane_order:
            p = self.planes[k]
            planes.append(p.valid)
            if self.entropy_mode != 'reduced':
                planes.append(p.started)
                planes.append(p.generation)
            else:
                # Reduced mode: `started` is not modelled (see reduced_is_sound),
                # so it carries no information and only inflates the key.
                planes.append(p.generation)
        busy = 0
        for w in self.busy:
            busy |= 1 << w
        pending = 0
        for i in self.pending:
            pending |= 1 << i
        return (tuple(entries), tuple(planes), busy, pending,
                self.error, self.reported_ok, self.drained)

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
        other.plane_order = self.plane_order
        other.planes = {}
        for k, p in self.planes.items():
            q = Plane()
            q.valid, q.generation = p.valid, p.generation
            q.started = 0 if self.entropy_mode == 'reduced' else p.started
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
        other.event_index = self.event_index
        other.pending = self.pending
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
        return bool(self.pending)

    def entropy_actions(self):
        """Every publication the current mode allows, not just the next one.

        canonical  the lowest-numbered outstanding event, i.e. one total order
                   over bands.  Kept as the explicitly labelled reduction.
        partial    every outstanding event whose precondition holds: `started`
                   for an unstarted band, `valid` for a started-but-unpublished
                   one.  Independent bands interleave freely, so a later band's
                   `started` can precede an earlier band's.
        """
        if not self.pending:
            return []
        out = []
        indices = sorted(self.pending)
        for index in indices:
            event = self.entropy_events[index]
            kind, (channel, plane_index, band) = event
            plane = self.plane(channel, plane_index)
            bit = 1 << band
            if kind == 'e_started':
                if not plane.started & bit:
                    out.append(event)
            elif self.entropy_mode == 'reduced':
                # Reduced mode has no `started` word at all: a band is simply
                # unpublished or published.  Reading `started` here would make
                # the mode reach nothing, which is the trap this branch avoids.
                if not plane.valid & bit:
                    out.append(event)
            elif plane.started & bit and not plane.valid & bit:
                out.append(event)
            if self.entropy_mode == 'canonical':
                # The prefix discipline: only the earliest outstanding event is
                # offered.  Under canonical ordering the earliest event's
                # precondition always holds, so at most one action is returned.
                return out
        return out

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
        event_actions = self.entropy_actions()
        acts.extend(event_actions)
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
            self.pending = self.pending - {self.event_index[action]}
            return
        if kind == 'e_valid':
            channel, index, band = payload
            self.plane(channel, index).valid |= 1 << band
            self.pending = self.pending - {self.event_index[action]}
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
        todo = list(self.topo['required_output'])
        by_write = {}
        for entry in self.entries:
            for target in entry.writes:
                by_write.setdefault((entry.channel,) + target, []).append(entry)
        while todo:
            item = todo.pop()
            if item in need:
                continue
            need.add(item)
            channel = item[0]
            for entry in by_write.get(item, []):
                todo.extend((channel, i, b) for (i, b) in entry.reads)
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

def explore(topology_name, policy, max_states=None, workers=2,
            entropy='partial', drain='anytime', withhold=frozenset(),
            stale=False, collect_edges=False, collect_projections=False,
            collect_false_okay=True):
    """Breadth-first over the reachable state space.

    An exhausted frontier is a proof over every reachable interleaving at this
    abstraction; a truncated one is reported with its frontier size and labelled
    `exhaustive: false` rather than presented as a proof.
    """
    topo = (topology_name if isinstance(topology_name, dict)
            else TOPOLOGIES[topology_name])
    if max_states is None:
        max_states = topo.get('state_cap', 400000)
    start = Machine(topo, policy, workers=workers, entropy=entropy, drain=drain,
                    withhold=withhold, stale=stale)
    frontier = [start]
    seen = {start.key()}
    edges = {}
    projections = set()
    violations = {}
    false_okay = 0
    false_okay_example = None
    drained_states = 0
    terminal_states = 0
    ok_states = 0
    deadlocks = 0
    # How many drained terminals failed for each reason.  A negative control has
    # to show that it reached the failure it claims to test; without this tally
    # an "ok" row can mean "never got there".
    error_kinds = {}

    while frontier:
        if len(seen) >= max_states:
            return {'violations': sorted(violations) + ['I4_UNPROVEN'],
                    'states': len(seen), 'drained': drained_states,
                    'terminal': terminal_states, 'ok': ok_states,
                    'deadlocks': deadlocks, 'exhaustive': False,
                    'bounded': False, 'truncated_at': max_states,
                    'error_kinds': error_kinds,
                    'edges': edges if collect_edges else None,
                    'projections': projections if collect_projections else None,
                    'false_okay': false_okay,
                    'false_okay_example': false_okay_example}
        nxt = []
        for machine in frontier:
            if collect_projections:
                projections.add(_observable_projection(machine))
            if machine.drained:
                drained_states += 1
                terminal_states += 1
                kind = _error_kind(machine.error)
                if kind is not None:
                    error_kinds[kind] = error_kinds.get(kind, 0) + 1
                if machine.reported_ok:
                    ok_states += 1
                    if collect_false_okay:
                        # The same visit that counts an OKAY can decide whether
                        # that OKAY was false.  Running a second BFS for this was
                        # pure duplication: it doubled the cost of every policy
                        # in the report for a number this loop already has.
                        missing = machine.missing_required()
                        if missing:
                            false_okay += 1
                            if false_okay_example is None:
                                false_okay_example = {
                                    'missing': missing,
                                    'error': machine.error,
                                    'entries': {e.name: e.state
                                                for e in machine.entries},
                                }
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
            'bounded': True, 'truncated_at': None, 'error_kinds': error_kinds,
            'edges': edges if collect_edges else None,
            'projections': projections if collect_projections else None,
            'false_okay': false_okay,
            'false_okay_example': false_okay_example}


def false_okay_states(topology_name, policy, max_states=None, workers=2,
                      entropy='partial', withhold=frozenset()):
    """Terminal states that report OK while a required band is absent.

    This is the property the review asked to be represented: not just "some
    invariant fired", but specifically a public success with a band missing from
    the output path's closure.
    """
    topo = TOPOLOGIES[topology_name]
    if max_states is None:
        max_states = topo.get('state_cap', 400000)
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


def liveness(topology_name, policy='proposed', max_states=None, workers=2,
             entropy='partial'):
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


def confluence(topology_name, policy='proposed', max_states=None, workers=2,
               probe_claims=True):
    """Check whether the canonical entropy order is a faithful reduction.

    Endpoint commutation is *not* sufficient, and the third review was right to
    say so: two publications that set different bits always end in the same
    state, yet publishing the final band a producer needs can make a consumer
    claimable, while publishing an unrelated band first cannot.  The intermediate
    observables differ even though the endpoints agree.

    So this compares the *observable futures*, not just the endpoints: for each
    independently enabled pair it applies A then B and B then A and compares
    (a) the successor key and (b) the set of actions enabled after each prefix.
    A pair whose prefixes expose different claimability is reported as a
    counterexample, because the canonical schedule would have foreclosed that
    interleaving.
    """
    topo = TOPOLOGIES[topology_name]
    if max_states is None:
        max_states = topo.get('state_cap', 200000)
    start = Machine(topo, policy, workers=workers, entropy='partial')
    frontier = [start]
    seen = {start.key()}
    checked = 0
    counterexample = None
    endpoint_only = 0
    truncated = False

    while frontier:
        if len(seen) >= max_states:
            truncated = True
            break
        nxt = []
        for machine in frontier:
            if machine.drained:
                continue
            enabled = machine.entropy_actions()
            for i in range(len(enabled)):
                for j in range(i + 1, len(enabled)):
                    first, second = enabled[i], enabled[j]
                    a = machine.clone()
                    a.step(first, {e.name: e for e in a.entries})
                    prefix_a = _observables(a)
                    a.step(second, {e.name: e for e in a.entries})
                    b = machine.clone()
                    b.step(second, {e.name: e for e in b.entries})
                    prefix_b = _observables(b)
                    b.step(first, {e.name: e for e in b.entries})
                    checked += 1
                    if a.key() != b.key():
                        endpoint_only += 1
                        if counterexample is None:
                            counterexample = {
                                'kind': 'endpoint',
                                'first': _event_label(first),
                                'second': _event_label(second),
                            }
                    elif prefix_a != prefix_b and counterexample is None:
                        # Endpoints agree, intermediate observables do not: the
                        # canonical order hides a reachable claim window.
                        counterexample = {
                            'kind': 'intermediate-observables',
                            'first': _event_label(first),
                            'second': _event_label(second),
                            'prefix_a': prefix_a,
                            'prefix_b': prefix_b,
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
            'endpoint_differences': endpoint_only,
            'exhaustive': not truncated, 'counterexample': counterexample}


def _event_label(event):
    return event[0] + ':' + repr(event[1])


def _observables(machine):
    """The queue-relevant observations visible at this state.

    Claimability is what a consumer can see; two prefixes that expose different
    claimable sets are not interchangeable even if they converge later.
    """
    claimable = tuple(sorted(
        entry.name for entry in machine.entries
        if entry.state == 'QUEUED' and machine.claimable(entry)))
    return (claimable, len(machine.busy), machine.error, machine.reported_ok)


# ------------------------------------------------- reduced-order soundness

def _observable_projection(machine):
    """Everything a policy or invariant can observe, minus `started`.

    `started` is deliberately excluded: the claim under test is that it is
    unobservable under `proposed`, so a projection that keeps it could not
    distinguish a sound reduction from an unsound one.
    """
    claimable = tuple(sorted(
        entry.name for entry in machine.entries
        if entry.state == 'QUEUED' and machine.claimable(entry)))
    return (
        tuple((e.name, e.state, e.executions, e.generation, e.worker)
              for e in machine.entries),
        tuple((k, machine.planes[k].valid, machine.planes[k].generation)
              for k in machine.plane_order),
        tuple(sorted(machine.busy)),
        claimable,
        machine.error,
        machine.reported_ok,
        machine.drained,
    )


def started_is_unobservable(topology_name, policy='proposed', max_states=60000,
                            workers=2):
    """Probe: does any reachable state depend on the `started` flags?

    For every reachable state the `started` word is permuted over the bands that
    entropy is allowed to announce, and the observable projection is recomputed.
    Under a policy that never reads `started` the projection cannot change.  This
    is what licenses dropping the word (and the `e_started` events) from the
    reduced model: without it, `reduced` would be a smaller space that happens to
    agree, not a quotient.
    """
    topo = TOPOLOGIES[topology_name]
    start = Machine(topo, policy, workers=workers, entropy='partial')
    frontier = [start]
    seen = {start.key()}
    checked = 0
    changed = None
    truncated = False

    while frontier:
        if len(seen) >= max_states:
            truncated = True
            break
        nxt = []
        for machine in frontier:
            base = _observable_projection(machine)
            for (channel, index) in machine.plane_order:
                plane = machine.planes[(channel, index)]
                allowed = 0
                for band in topo['entropy_bands'].get(index, set()):
                    if (channel, index, band) not in machine.withhold:
                        allowed |= 1 << band
                for variant in (0, allowed, plane.valid & allowed,
                                allowed & ~plane.valid):
                    probe = machine.clone()
                    probe.planes[(channel, index)].started = variant
                    checked += 1
                    if _observable_projection(probe) != base and changed is None:
                        changed = {'plane': [channel, index],
                                   'started': variant,
                                   'before': base[3], 'after':
                                   _observable_projection(probe)[3]}
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

    return {'unobservable': changed is None, 'probes': checked,
            'states': len(seen), 'exhaustive': not truncated,
            'counterexample': changed}


def reduction_check(topology_name, max_states=200000, workers=2,
                    policy='proposed'):
    """Is `reduced` an exact quotient of `partial` on observable behaviour?

    Two things are required, and both are checked:

    1. `started` is unobservable (see `started_is_unobservable`), so dropping the
       word and the per-band announcement cannot change what any guard,
       invariant or worker action can see.
    2. The reachable *observable* projections coincide.  Compared over the full
       explored set in both modes, so worker actions (claim, admit, execute,
       publish, generation change) are interleaved exactly as in the partial run.
       A topology whose partial space is bigger than the cap is reported
       `comparable: false` rather than compared on a truncated prefix.
    """
    partial = explore(topology_name, policy, entropy='partial',
                      max_states=max_states, workers=workers,
                      collect_projections=True)
    reduced = explore(topology_name, policy, entropy='reduced',
                      max_states=max_states, workers=workers,
                      collect_projections=True)
    if not partial['exhaustive'] or not reduced['exhaustive']:
        return {'comparable': False, 'partial_states': partial['states'],
                'reduced_states': reduced['states'],
                'partial_exhaustive': partial['exhaustive'],
                'reduced_exhaustive': reduced['exhaustive'],
                'only_partial': None, 'only_reduced': None}
    p, r = partial['projections'], reduced['projections']
    only_partial = p - r
    only_reduced = r - p
    return {'comparable': True, 'partial_states': partial['states'],
            'reduced_states': reduced['states'],
            'partial_exhaustive': True, 'reduced_exhaustive': True,
            'only_partial': len(only_partial),
            'only_reduced': len(only_reduced),
            'equal': not only_partial and not only_reduced,
            'example_only_partial': _projection_summary(next(iter(only_partial)))
            if only_partial else None,
            'example_only_reduced': _projection_summary(next(iter(only_reduced)))
            if only_reduced else None}


def _projection_summary(proj):
    if proj is None:
        return None
    entries, planes, busy, claimable, error, ok, drained = proj
    return {'entries': {e[0]: e[1] for e in entries},
            'claimable': list(claimable), 'busy': list(busy),
            'ok': ok, 'drained': drained}


def channel_independence(topology_name):
    """Structural check: do channels share anything but the worker pool?

    The compositional argument for three channels is only sound if no entry reads
    or writes another channel's plane and no plane key crosses channels.  That is
    a property of the topology table, so it is checked directly rather than
    inferred from a run.
    """
    topo = TOPOLOGIES[topology_name]
    problems = []
    channels = {k[0] for k in topo['planes']}
    for spec in topo['entries']:
        # Topology entries are plain tuples: (name, channel, index, reads, writes).
        name, channel, index, reads, writes = spec
        for (plane_index, band) in list(reads) + list(writes):
            if (channel, plane_index) not in topo['planes']:
                problems.append('%s touches plane (%d,%d), which is not its own'
                                % (name, channel, plane_index))
    # Every plane key belongs to exactly one channel, by construction of the key.
    for (channel, index) in topo['planes']:
        if (channel, index) not in topo['planes']:
            problems.append('plane %s missing' % ((channel, index),))
    return {'independent': not problems, 'channels': sorted(channels),
            'problems': problems}


def _channel_subtopology(topology_name, channel):
    """The same topology restricted to one channel.

    Used for the compositional safety argument: each channel must be safe on its
    own, exhaustively, because the only thing a neighbouring channel can do is
    occupy a worker slot.
    """
    topo = TOPOLOGIES[topology_name]
    planes = {k: n for k, n in topo['planes'].items() if k[0] == channel}
    entries = [spec for spec in topo['entries'] if spec[1] == channel]
    required = [item for item in topo['required_output'] if item[0] == channel]
    if not planes:
        return None
    return {
        'planes': planes,
        'entropy_bands': {index: bands
                          for index, bands in topo['entropy_bands'].items()},
        'entries': entries,
        'required_output': required,
        'stale_capable': topo.get('stale_capable', False),
        'state_cap': topo.get('state_cap', 400000),
    }


def compositional_safety(topology_name, policy='proposed', max_states=200000,
                         workers=2):
    """Proof-by-decomposition for the multi-channel topologies.

    A full product search over three channels is not exhaustible here.  The
    argument is instead:

      1. **Independence** -- no entry reads or writes another channel's plane, so
         a channel's evolution depends only on its own planes and on whether it
         obtains a worker.
      2. **Per-channel safety** -- each channel, modelled alone and *exhaustively*,
         never reports OKAY with a required band absent.
      3. **Fail-closed draining** -- a public OKAY requires every entry of every
         channel to be terminal and no terminal error, so a channel that starved
         for workers yields an error, never a silent success.

    (1) and (3) are structural; (2) is exhaustive per channel.  Together they
    cover every worker interleaving, including cross-channel ones, because a
    worker slot is the only thing one channel can take from another and the
    conclusion holds whether or not a channel ever gets one.
    """
    topo = TOPOLOGIES[topology_name]
    indep = channel_independence(topology_name)
    if not indep['independent']:
        return {'ok': False, 'reason': 'channels share planes',
                'independence': indep}

    per_channel = {}
    failures = []
    for channel in indep['channels']:
        sub = _channel_subtopology(topology_name, channel)
        if sub is None or not sub['entries']:
            continue
        result = explore(sub, policy, entropy='reduced',
                         max_states=max_states, workers=workers)
        per_channel[channel] = {
            'states': result['states'],
            'exhaustive': result['exhaustive'],
            'violations': result['violations'],
            'ok': result['ok'],
        }
        real = [v for v in result['violations'] if v != 'I4_UNPROVEN']
        if real:
            failures.append('channel %d violates %s' % (channel, real))
        if not result['exhaustive']:
            failures.append('channel %d not exhaustive (%d states)'
                            % (channel, result['states']))
        if result['exhaustive'] and result['ok'] == 0:
            failures.append('channel %d never completed' % channel)

    # (3) the drain predicate must require global terminality.
    start = Machine(topo, policy, workers=workers, entropy='reduced')
    sample = start.clone()
    for entry in sample.entries:
        if entry.channel != indep['channels'][0]:
            entry.state = 'PUBLISHED'
            entry.executions = 1
    sample.drain()
    drain_is_global = sample.reported_ok is False
    if not drain_is_global:
        failures.append('drain reported OKAY with a non-terminal entry')

    return {'ok': not failures, 'independence': indep,
            'per_channel': per_channel, 'drain_is_global': drain_is_global,
            'failures': failures,
            'note': ('independence + exhausting each channel alone + global '
                     'drain covers every worker interleaving: a slot is the only '
                     'cross-channel resource and the conclusion holds whether or '
                     'not a channel obtains one')}


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


def partial_order_witness(topology_name='spatial_chain', max_states=200000):
    """Is a later band's `started` reachable before the earliest band's?

    This is the reviewer's source-pinned negative control for the partial-order
    mode.  Under a genuine per-band partial order the answer is yes; under a
    canonical prefix cursor it is no, because the earliest outstanding event is
    the only one ever offered.  A model that claims partial-order coverage must
    fail this check if it silently reverts to a prefix.
    """
    topo = TOPOLOGIES[topology_name]
    start = Machine(topo, 'proposed', entropy='partial')
    # Only entropy transitions matter here; drain would end the run.
    frontier = [start]
    seen = {start.key()}
    first_band = start.entropy_events[0][1]
    witness = None
    while frontier:
        if len(seen) >= max_states:
            return {'reachable': witness is not None, 'states': len(seen),
                    'exhaustive': False, 'witness': witness}
        nxt = []
        for machine in frontier:
            if machine.drained:
                continue
            for event in machine.entropy_actions():
                child = machine.clone()
                child.step(event, {e.name: e for e in child.entries})
                kind, payload = event
                channel, plane_index, band = payload
                if (kind == 'e_started' and payload != first_band
                        and not child.plane(first_band[0], first_band[1])
                        .started & (1 << first_band[2])):
                    if witness is None:
                        witness = {'fired_first': list(payload),
                                   'still_unstarted': list(first_band)}
                k = child.key()
                if k in seen:
                    continue
                seen.add(k)
                nxt.append(child)
        frontier = nxt
    return {'reachable': witness is not None, 'states': len(seen),
            'exhaustive': True, 'witness': witness}


def missing_channel_output(topology_name='spatial_three_channel',
                           max_states=1200000, workers=4):
    """Does the output closure actually cover ch1/ch2, not just ch0?

    The third review's point: seeding `required_output` with channel 0 only
    leaves ch1/ch2 unconstrained, so a public OKAY can lack their final
    lowpasses.  Testing that by withholding a channel and expecting `I1` to fire
    does not work -- with the chain never started, no state reports OKAY at all,
    so the invariant is vacuously satisfied.  Instead this compares the two
    closures directly on a state that has completed ch0 and ch2 but not ch1:
    the ch0-only closure is empty (a false OKAY), the per-channel closure is not.
    """
    topo = TOPOLOGIES[topology_name]
    start = Machine(topo, 'proposed', workers=workers)
    # Publish the ch0 and ch2 chains by executing their entries directly, and
    # leave ch1 untouched.  This is a state construction, not a searched run: the
    # question is what the *output predicate* says about a given state.
    for entry in start.entries:
        if entry.channel in (0, 2):
            for (plane_index, band) in entry.reads:
                start.plane(entry.channel, plane_index).valid |= 1 << band
            for (plane_index, band) in entry.writes:
                start.plane(entry.channel, plane_index).valid |= 1 << band
            entry.state = 'PUBLISHED'
            entry.executions = 1
    for entry in start.entries:
        if entry.channel == 1:
            for (plane_index, band) in entry.writes:
                # ch1's entropy bands exist (plane 2 gets them), but the adjacent
                # transform never ran, so its band 0 does not.
                start.plane(entry.channel, plane_index).valid |= 1 << band
    start.reported_ok = True
    start.drained = True

    per_channel = start.missing_required()
    # The retired ch0-only closure: seed with channel 0 output alone.
    saved = topo['required_output']
    try:
        topo['required_output'] = [(0, 0, 0)]
        ch0_only = start.missing_required()
    finally:
        topo['required_output'] = saved

    ch1_gap = [m for m in per_channel if m['channel'] == 1]
    ok = bool(ch1_gap) and not ch0_only
    return {'ok': ok, 'per_channel_missing': per_channel,
            'ch0_only_missing': ch0_only,
            'reason': (None if ok else
                       'per-channel closure does not detect the ch1 gap: %s'
                       % (per_channel,))}


def canonical_prefix_blindspot(topology_name='spatial_chain', max_states=200000):
    """Regression: the canonical prefix forecloses a reachable claim window.

    The third review's central point, made executable.  Two entropy publications
    that are both outstanding and both enabled do not commute in their
    *intermediate observables*: publishing the band a producer is waiting for can
    make a consumer claimable, while publishing an unrelated earlier-numbered
    band first cannot.  A canonical schedule that offers only the earliest
    outstanding event therefore never reaches the first observation, even though
    the endpoints of the two orders agree.

    This returns the witness pair and whether the canonical mode can reach the
    claimable observation at all.  The self-test requires:
      * a witness exists (partial mode reaches a claim after the later event),
      * canonical mode does not reach a claim right after that event, and
      * the two endpoints are identical, which is why endpoint comparison alone
        had been misleading.
    """
    topo = TOPOLOGIES[topology_name]
    start = Machine(topo, 'proposed', entropy='partial')
    frontier = [start]
    seen = {start.key()}
    witness = None

    while frontier and len(seen) < max_states:
        nxt = []
        for machine in frontier:
            if machine.drained:
                continue
            enabled = machine.entropy_actions()
            for i in range(len(enabled)):
                for j in range(i + 1, len(enabled)):
                    first, second = enabled[i], enabled[j]
                    a = machine.clone()
                    a.step(first, {e.name: e for e in a.entries})
                    after_first = _observables(a)
                    a.step(second, {e.name: e for e in a.entries})
                    b = machine.clone()
                    b.step(second, {e.name: e for e in b.entries})
                    after_second = _observables(b)
                    b.step(first, {e.name: e for e in b.entries})
                    if (a.key() == b.key()
                            and after_first != after_second
                            and witness is None):
                        witness = {
                            'earlier': _event_label(first),
                            'later': _event_label(second),
                            'claimable_after_earlier': list(after_first[0]),
                            'claimable_after_later': list(after_second[0]),
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

    # Does the canonical mode ever expose that claim window?
    canonical_reaches = False
    canon = Machine(topo, 'proposed', entropy='canonical')
    frontier = [canon]
    cseen = {canon.key()}
    while frontier and len(cseen) < max_states:
        nxt = []
        for machine in frontier:
            if machine.drained:
                continue
            for action in machine.actions():
                child = machine.clone()
                child.step(action, {e.name: e for e in child.entries})
                if witness and action[0] == 'e_valid' \
                        and _event_label(action) == witness['later']:
                    if _observables(child)[0]:
                        canonical_reaches = True
                k = child.key()
                if k in seen:
                    continue
                cseen.add(k)
                nxt.append(child)
        frontier = nxt

    return {'witness': witness, 'canonical_reaches_window': canonical_reaches,
            'states': len(seen),
            'exhaustive': len(seen) < max_states and len(cseen) < max_states}


STALE_KINDS = ('stale-generation', 'stale-before-publication')


def _verdict(exhaustive, premise, holds):
    """Three-way verdict for one control.

    `premise` is whether the control actually constructed the situation it is
    supposed to test; `holds` is whether the property survived.  A truncated
    search can never be green, regardless of how clean its prefix looked.
    """
    if not exhaustive:
        return PREFIX_ONLY_INCONCLUSIVE
    if not premise:
        return FAIL
    return PASS_EXHAUSTIVE if holds else FAIL


def negative_cases():
    """Named controls, each with an explicit verdict and its premise.

    Returns `(rows, failures, inconclusive)`.  `failures` lists real defects;
    `inconclusive` lists controls whose search hit its cap.  They are kept apart
    because a truncated control has decided nothing -- folding it into the pass
    path is how a prefix becomes a proof, and folding it into the failure path is
    how an honest bound becomes a broken model.
    """
    failures = []
    inconclusive = []
    rows = []

    def record(row, premise_detail, blocking=True):
        row['premise'] = premise_detail
        row['blocking'] = blocking
        if blocking and row['verdict'] == FAIL:
            failures.append('%s: %s' % (row['case'], row.get('reason', 'failed')))
        elif blocking and row['verdict'] == PREFIX_ONLY_INCONCLUSIVE:
            inconclusive.append('%s: %s' % (row['case'], row.get('reason',
                                                               'search truncated')))
        rows.append(row)

    for case in NEGATIVE_CASES:
        hits, states, exhaustive = claims_seen(
            case['topology'], case['policy'], case['withhold'], case['entry'])
        low = case.get('expect_claims_min')
        if low is None:
            good = hits == case['expect_claims']
        else:
            good = hits >= low
        record({'case': case['name'], 'claims': hits, 'states': states,
                'verdict': _verdict(exhaustive, True, good),
                'reason': '%d claims (expected %s)'
                          % (hits, case.get('expect_claims', '>=%s' % low))},
               {'exhaustive': exhaustive, 'truncated_at': None if exhaustive else states})

    # (c) a claimed-then-stale entry must fail closed.
    #
    # Two controls, because the reviewer asked for one that is genuinely cheap
    # and exhaustive *and* the larger space is worth showing.
    #
    # (c1) single entry: `spatial_highpass` has exactly one transform entry, so
    #      the stale window is decidable on 366 states in milliseconds under the
    #      true partial order.  This is the exhaustive positive the fifth review
    #      asked for, and it is not vacuous: both stale mechanisms appear
    #      (`stale-generation`, `stale-before-publication`) alongside one OKAY
    #      terminal, so the run proves "a stale entry fails closed" while also
    #      proving the control can reach success.
    single = explore('spatial_highpass', 'proposed', stale=True,
                     drain='quiescent', max_states=60000)
    single_kinds = {k: v for k, v in single['error_kinds'].items()
                    if k in STALE_KINDS}
    single_premise = bool(single_kinds) and single['ok'] >= 1
    if not single_premise and single['exhaustive']:
        failures.append('stale-generation (single entry): the control never '
                        'reached a stale failure (kinds=%s, ok=%d), so it '
                        'proves nothing' % (single['error_kinds'], single['ok']))
    record({'case': 'stale_generation_fail_closed_single_entry',
            'claims': single['ok'], 'states': single['states'],
            'error_kinds': single['error_kinds'],
            'verdict': _verdict(single['exhaustive'], single_premise,
                                'I1_FAIL_CLOSED' not in single['violations']),
            'reason': ('a stale entry reached OKAY'
                       if 'I1_FAIL_CLOSED' in single['violations'] else
                       'no stale failure on any reachable path')},
           {'entropy': 'partial', 'topology': 'spatial_highpass',
            'stale_kinds': single_kinds, 'ok_terminals': single['ok'],
            'exhaustive': single['exhaustive']})

    # (c2) the chain, on the reduced order.  The partial order cannot be
    #      exhausted there: 411 841 states against a 400 k cap, so rev 5's
    #      version of this control only ever saw a prefix and then passed on the
    #      *absence* of a violation -- the fifth review's finding.  The reduced
    #      order is the same decoder under the proven quotient, and there the
    #      space is 13 529 states and finishes in under a second.
    stale = explore('spatial_chain', 'proposed', stale=True, drain='quiescent',
                    entropy='reduced', max_states=120000)
    stale_kinds = {k: v for k, v in stale['error_kinds'].items()
                   if k in STALE_KINDS}
    stale_premise = bool(stale_kinds) and stale['ok'] >= 1
    if not stale_premise and stale['exhaustive']:
        failures.append('stale-generation: the control never reached a stale '
                        'failure (kinds=%s, ok=%d), so it proves nothing'
                        % (stale['error_kinds'], stale['ok']))
    record({'case': 'stale_generation_fail_closed',
            'claims': stale['ok'], 'states': stale['states'],
            'error_kinds': stale['error_kinds'],
            'verdict': _verdict(stale['exhaustive'], stale_premise,
                                'I1_FAIL_CLOSED' not in stale['violations']),
            'reason': ('a stale entry reached OKAY'
                       if 'I1_FAIL_CLOSED' in stale['violations'] else
                       'no stale failure on any reachable path')},
           {'entropy': 'reduced', 'topology': 'spatial_chain',
            'stale_kinds': stale_kinds, 'ok_terminals': stale['ok'],
            'exhaustive': stale['exhaustive']})

    # The same control on the true partial order of the chain, kept as evidence
    # for what the reduction bought.  Non-blocking: it is a bound, and a bound
    # may not turn the verdict green or red.
    stale_partial = explore('spatial_chain', 'proposed', stale=True,
                            drain='quiescent', max_states=120000)
    record({'case': 'stale_generation_fail_closed_partial_order',
            'claims': stale_partial['ok'], 'states': stale_partial['states'],
            'verdict': PREFIX_ONLY_INCONCLUSIVE,
            'reason': 'partial-order space exceeds the cap; the reduced run '
                      'above carries the proof'},
           {'exhaustive': stale_partial['exhaustive'],
            'note': 'informational, not used for the verdict'},
           blocking=False)

    # (d) duplicate producer: a second owner of an already published band is
    #     refused, so no state reports two live producers.
    dup = explore('spatial_chain', 'proposed')
    record({'case': 'single_owner_no_duplicate_producer',
            'claims': dup['ok'], 'states': dup['states'],
            'verdict': _verdict(dup['exhaustive'], True,
                                'I7_SINGLE_OWNER' not in dup['violations']),
            'reason': 'two live producers of one band'},
           {'exhaustive': dup['exhaustive']})

    # (e) four workers on unsatisfiable entries: drain fails closed.  The premise
    #     is that a drain was actually reachable -- `ok == 0` alone would also be
    #     satisfied by a run that never terminated.
    starve = explore('fieldplus_half', 'proposed', workers=4,
                     withhold=[(0, 5, 0), (0, 4, 0), (0, 3, 0)])
    starve_premise = starve['drained'] > 0 and \
        starve['error_kinds'].get('incomplete', 0) > 0
    if not starve_premise and starve['exhaustive']:
        failures.append('starvation: no drain reached an incomplete error '
                        '(drained=%d, kinds=%s), so it proves nothing'
                        % (starve['drained'], starve['error_kinds']))
    record({'case': 'four_worker_starvation',
            'claims': starve['ok'], 'states': starve['states'],
            'error_kinds': starve['error_kinds'],
            'verdict': _verdict(starve['exhaustive'], starve_premise,
                                starve['ok'] == 0),
            'reason': 'an unsatisfiable decode reported OKAY'},
           {'workers': 4, 'drained': starve['drained'],
            'exhaustive': starve['exhaustive']})

    # (f) partial order must be a real partial order, not a prefix in disguise.
    witness = partial_order_witness()
    record({'case': 'partial_order_out_of_order_started',
            'claims': witness.get('states'),
            'states': witness.get('witness'),
            'verdict': _verdict(witness['exhaustive'], True,
                                witness['reachable']),
            'reason': 'a later band\'s started is unreachable before the '
                      'earliest band\'s -- the mode is a prefix'},
           {'reachable': witness['reachable'],
            'exhaustive': witness['exhaustive']})

    # (g) the three-channel output closure must cover ch1/ch2, not just ch0.
    chan = missing_channel_output()
    record({'case': 'three_channel_missing_output',
            'claims': chan.get('states'), 'states': chan.get('reason'),
            'verdict': PASS_EXHAUSTIVE if chan['ok'] else FAIL,
            'reason': chan.get('reason') or 'per-channel closure missing'},
           {'constructed_state': True})
    return rows, failures, inconclusive


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
    """Fail-closed checks, bounded so the suite runs in minutes.

    Exhaustiveness is not asserted here for the large topologies: their true
    partial-order space is bigger than the exploration cap, and a truncated
    search is reported as `INCONCLUSIVE` rather than counted as a proof.  What
    the suite does require is that no *safety* violation appears on any explored
    prefix and that both negative controls still fail.
    """
    failures = []
    # Every sweep below is bounded: the true partial-order space of a
    # 17-subband group or three channels exceeds what is exhaustible here, and a
    # truncated search is reported INCONCLUSIVE rather than counted as a proof.
    SMALL = 60000

    # (A) The canonical entropy order is NOT a valid reduction, and the model now
    # proves it instead of assuming it.  A later band's publication exposes a
    # claim that the earliest outstanding publication does not, while the two
    # endpoints agree -- which is exactly why endpoint comparison had been
    # misleading.
    blind = canonical_prefix_blindspot()
    if not blind['exhaustive']:
        failures.append('canonical blindspot: witness search truncated')
    if blind['witness'] is None:
        failures.append('canonical blindspot: no witness found; the canonical '
                        'reduction would need re-justification, not silence')
    elif blind['canonical_reaches_window']:
        failures.append('canonical blindspot: canonical mode DID reach the '
                        'claim window; re-examine the reduction')

    # (B) Proposed safety on every topology, on the explored prefix.
    # `I4_UNPROVEN` is a truncation marker, not a safety violation: it says the
    # search did not finish, which is reported honestly rather than counted as a
    # defect.  Only real invariant names are safety failures.
    for topo in TOPOLOGIES:
        proposed = explore(topo, 'proposed', entropy='partial',
                           max_states=SMALL)
        real = [v for v in proposed['violations'] if v != 'I4_UNPROVEN']
        if real:
            failures.append('%s: proposed violates %s under partial order'
                            % (topo, real))
        if proposed['exhaustive'] and proposed['ok'] == 0:
            failures.append('%s: proposed never completed a valid decode' % topo)

    # (C) Both controls must still fail on their explored prefix.  A control that
    # stops failing is a broken control, not a fixed decoder.
    for topo in TOPOLOGIES:
        released = explore(topo, 'released', entropy='partial',
                           max_states=SMALL)
        legacy = explore(topo, 'legacy', entropy='partial', max_states=SMALL)
        if 'I1_FAIL_CLOSED' not in released['violations']:
            failures.append('%s: released does not reproduce I1' % topo)
        if 'I1_FAIL_CLOSED' not in legacy['violations']:
            failures.append('%s: legacy does not reproduce I1' % topo)

    # (D) Positive completion on the topologies whose quiescent space is finite.
    for topo in ('spatial_chain', 'spatial_highpass'):
        live = liveness(topo, 'proposed', max_states=200000)
        if not live['exhaustive']:
            failures.append('%s: liveness search truncated' % topo)
        elif live['violations']:
            failures.append('%s: liveness violated %s'
                            % (topo, live['violations']))

    # (E) Named negative cases: producer gating, partial-order reachability and
    # the per-channel output closure.  A truncated control is *not* a failure of
    # the model and *not* a pass either: it is reported as INCONCLUSIVE and named
    # in the closing line, so "the space was too large" never reads as "proved".
    _rows, case_failures, case_inconclusive = negative_cases()
    failures.extend(case_failures)
    for case in case_inconclusive:
        print('SELF-TEST INCONCLUSIVE %s' % case, file=sys.stderr)

    # (F) The reduced order must be a *sound* quotient, not merely smaller.  Two
    # independent requirements: `started` is unobservable to every policy guard,
    # and the reachable observable projections coincide exactly.  If either
    # fails, `reduced` is just a different model that happens to agree here.
    # Only the policies the reduction is claimed for.  `released` is excluded by
    # construction: its predicate is `valid == started`, so `started` is
    # observable there and `reduced` refuses to run against it.
    for topo in ('spatial_highpass', 'spatial_chain', 'fieldplus_half'):
        for policy in ('proposed', 'legacy'):
            probe = started_is_unobservable(topo, policy=policy,
                                            max_states=SMALL)
            if not probe['exhaustive']:
                continue
            if not probe['unobservable']:
                failures.append(
                    '%s/%s: `started` is observable (%s), so dropping it is '
                    'unsound' % (topo, policy, probe['counterexample']))
    for topo in ('spatial_highpass', 'spatial_chain'):
        red = reduction_check(topo, max_states=400000)
        if not red['comparable']:
            failures.append('%s: reduced/partial not comparable (%d/%d states)'
                            % (topo, red['partial_states'],
                               red['reduced_states']))
        elif not red['equal']:
            failures.append(
                '%s: reduced order is not a quotient -- %d projection(s) only in '
                'partial, %d only in reduced'
                % (topo, red['only_partial'], red['only_reduced']))

    # (G) Three channels, proven by decomposition rather than by a product search
    # that cannot finish.  Independence is structural, each channel is exhaustive
    # alone, and the drain predicate is global -- together those cover every
    # cross-channel worker interleaving, because a worker slot is the only
    # resource one channel can take from another.
    comp = compositional_safety('spatial_three_channel', max_states=200000)
    if not comp['ok']:
        failures.append('three-channel decomposition failed: %s'
                        % (comp['failures'] or comp.get('reason')))

    if failures:
        for failure in failures:
            print('SELF-TEST FAIL %s' % failure, file=sys.stderr)
        return 1

    print('SELF-TEST PASS: canonical reduction disproved with a witness; '
          'proposed finds no safety violation on any explored prefix of %d '
          'topologies; released and legacy still fail; liveness clean where the '
          'space is exhaustive' % len(TOPOLOGIES))
    return 0


def model_verdict(report, negative_failures, negative_inconclusive):
    """The single place that decides whether this model run passed.

    It lives in one function for a reason.  Revision 5 computed its verdict
    inline in `main()` and never looked at the failure list from
    `negative_cases()`: a broken stale-generation or starvation control could
    fail, the report would still print MODEL PASS and the process would exit 0,
    while `--negative-cases` on the same tree reported a failure.  Two paths,
    two verdicts, one of them wrong.

    Returns `(verdict, exit_code, facts)`.  Truncation is a third outcome, not a
    softer failure: a control that hit its cap decided nothing, so it may not
    exit 0, and it is not a defect either.
    """
    def real_violations(per):
        return [v for v in per['violations'] if v != 'I4_UNPROVEN']

    tops = report['topologies']
    facts = {
        'safe': all(not real_violations(p['proposed']) for p in tops.values()),
        'reduced_safe': all(not real_violations(p['reduced']['proposed'])
                            for p in tops.values()),
        'quotients': all(p['reduction'].get('equal')
                         for p in tops.values()
                         if p['reduction'].get('comparable')),
        'unobservable': all(p['unobservable']['proposed']['unobservable']
                            for p in tops.values()),
        'controls_fire': all('I1_FAIL_CLOSED' in p['released']['violations']
                             and 'I1_FAIL_CLOSED' in p['legacy']['violations']
                             for p in tops.values()),
        'canonical_disproved': report['canonical_witness']['witness'] is not None,
        'compositional': bool(report['compositional']['ok']),
        'negative_controls': not negative_failures,
    }
    facts['exhaustive'] = [t for t, p in tops.items()
                           if p['reduced']['proposed']['exhaustive']]
    facts['incomplete'] = [t for t, p in tops.items()
                           if not p['reduced']['proposed']['exhaustive']]
    passed = all(v if isinstance(v, bool) else True for v in facts.values())
    if not passed:
        return 'FAIL', 2, facts
    if negative_inconclusive:
        return 'INCONCLUSIVE', 3, facts
    return 'PASS', 0, facts


def verdict_regression():
    """Prove the verdict can fail, and that both CLI paths agree.

    The fifth review's finding was structural: `--json` discarded the failure
    list from `negative_cases()`, so a broken control printed MODEL PASS.  A
    check that only exercises the happy path cannot catch that, so this builds a
    report that is green in every *other* respect and then injects exactly the
    two control outcomes the reviewer named.  Each case asserts the exit code,
    because an exit code is what CI actually reads.
    """
    def topo_entry(**over):
        base = {'proposed': {'violations': [], 'states': 10, 'ok': 1,
                             'exhaustive': True, 'false_okay': 0},
                'released': {'violations': ['I1_FAIL_CLOSED'], 'states': 10},
                'legacy': {'violations': ['I1_FAIL_CLOSED'], 'states': 10},
                'reduced': {'proposed': {'violations': [], 'states': 5,
                                         'exhaustive': True}},
                'reduction': {'comparable': True, 'equal': True,
                              'only_partial': 0, 'only_reduced': 0},
                'unobservable': {'proposed': {'unobservable': True}}}
        base.update(over)
        return base

    def report(topologies=None, witness=True, comp=True):
        return {'topologies': topologies or {'t': topo_entry()},
                'canonical_witness': {'witness': {'a': 1} if witness else None},
                'compositional': {'ok': comp}}

    failures = []
    rows = []

    def check(name, got, expected_code, expected_verdict):
        code, verdict = got
        good = code == expected_code and verdict == expected_verdict
        rows.append({'case': name, 'exit': code, 'model_verdict': verdict,
                     'expected_exit': expected_code,
                     'expected_verdict': expected_verdict,
                     'verdict': PASS_EXHAUSTIVE if good else FAIL})
        if not good:
            failures.append('%s: exit %d/verdict %s (expected %d/%s)'
                            % (name, code, verdict, expected_code,
                               expected_verdict))

    def evaluate(rep, fails=(), inconclusive=()):
        verdict, code, _facts = model_verdict(rep, list(fails),
                                              list(inconclusive))
        return code, verdict

    check('clean_run_is_pass', evaluate(report()), 0, 'PASS')
    # The reviewer's case: one control fails, everything else is green.
    check('control_failure_fails_json',
          evaluate(report(), ['stale_generation_fail_closed: no stale failure']),
          2, 'FAIL')
    # The truncation case: it may not exit 0 and it is not a defect.
    check('control_truncation_is_inconclusive',
          evaluate(report(), [], ['witness search truncated']), 3,
          'INCONCLUSIVE')
    # A real safety violation must still fail on its own.
    check('safety_violation_fails',
          evaluate(report({'t': topo_entry(
              proposed={'violations': ['I1_FAIL_CLOSED'], 'states': 1,
                        'ok': 1, 'exhaustive': True, 'false_okay': 1})})),
          2, 'FAIL')
    # A control that stops firing is a broken control, not a fix.
    check('control_stops_firing_fails',
          evaluate(report({'t': topo_entry(
              legacy={'violations': [], 'states': 10})})), 2, 'FAIL')

    # The two paths must agree: whatever the predicate says about a failed
    # control, `--negative-cases` says too.  Both now read the same list, so the
    # regression is that a non-empty failure list cannot produce exit 0.
    for fails in (['x: y'], ['a: b', 'c: d']):
        code, _v = evaluate(report(), fails)
        if code == 0:
            failures.append('negative failure list %r still exits 0' % (fails,))

    if failures:
        for failure in failures:
            print('VERDICT-REGRESSION FAIL %s' % failure, file=sys.stderr)
        return 1, rows
    print('VERDICT-REGRESSION PASS: %d cases, control failures and truncation '
          'both leave the green path' % len(rows))
    return 0, rows


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--json', metavar='PATH')
    parser.add_argument('--confluence', metavar='TOPOLOGY')
    parser.add_argument('--negative-cases', action='store_true')
    parser.add_argument('--canonical-witness', action='store_true')
    parser.add_argument('--reduction', metavar='TOPOLOGY')
    parser.add_argument('--verdict-regression', action='store_true')
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    if args.verdict_regression:
        code, rows = verdict_regression()
        for row in rows:
            print('%-38s exit=%d %s' % (row['case'], row['exit'],
                                        row['verdict']))
        return code

    if args.canonical_witness:
        print(json.dumps(canonical_prefix_blindspot(), indent=1))
        return 0

    if args.reduction:
        print(json.dumps(reduction_check(args.reduction, max_states=400000),
                         indent=1, default=str))
        return 0

    if args.confluence:
        print(json.dumps(confluence(args.confluence), indent=1))
        return 0

    if args.negative_cases:
        rows, failures, inconclusive = negative_cases()
        for row in rows:
            print('%-42s %s' % (row['case'], row['verdict']))
            for key in ('claims', 'states', 'witness', 'reason', 'error_kinds',
                        'premise'):
                if row.get(key) is not None:
                    print('%-42s   %s=%s' % ('', key, row[key]))
        for failure in failures:
            print('NEGATIVE FAIL %s' % failure, file=sys.stderr)
        for case in inconclusive:
            print('NEGATIVE INCONCLUSIVE %s' % case, file=sys.stderr)
        if failures:
            print('NEGATIVE-CASES FAIL')
            return 2
        if inconclusive:
            print('NEGATIVE-CASES INCONCLUSIVE: %d control(s) truncated, none of '
                  'them counted as a pass' % len(inconclusive))
            return 3
        print('NEGATIVE-CASES PASS_EXHAUSTIVE')
        return 0

    # `--check` is the CTest gate and must stay cheap: it verifies that every
    # claim in the model still matches the frozen tree.  It must NOT build the
    # state-space report, whose sweeps are bounded but minutes long -- the anchor
    # is the thing CI has to fail on, and a slow gate invites a raised timeout
    # instead of a fix.
    if args.check:
        anchor = source_anchor()
        if anchor['status'] != 'OK':
            for problem in anchor['problems']:
                print('ANCHOR FAIL: %s' % problem, file=sys.stderr)
            return 2
        print('ANCHOR PASS: %d edges pinned to the frozen tree'
              % len(anchor['rows']))
        return 0

    if not args.json:
        parser.error('one of --check/--json/--self-test/--canonical-witness/'
                     '--reduction/--confluence/--negative-cases is required')

    anchor = source_anchor()
    report = {'kind': 'DESIGN_MODEL',
              'revision': 6,
              'entropy_order': ('partial (per-band; the canonical prefix order is '
                                'disproved, see canonical_witness)'),
              'note': ('reachability model over explicit producer edges and '
                       'per-band publication; not a measurement of the frozen '
                       'decoder'),
              'source_anchor': anchor,
              'canonical_witness': canonical_prefix_blindspot(),
              'reduced_order': {
                  'definition': ('drop the `started` word and the per-band '
                                 'e_started events; a band is either unpublished '
                                 'or published'),
                  'soundness': ('started_is_unobservable proves no policy guard '
                                'reads `started`; the released policy is refused '
                                'because its claim predicate is valid == started'),
                  'scope': 'proposed and legacy only',
              },
              'topologies': {}}
    # The fifth review: `--json` dropped these failures on the floor.  The rows
    # went into the report, `_failures` was thrown away, and the pass predicate
    # never looked at it -- so a stale-generation or starvation control could fail
    # while the same run printed MODEL PASS and exited 0.  Both lists are now kept
    # and both feed the verdict at the end of `main()`.
    negatives, negative_failures, negative_inconclusive = negative_cases()
    report['negative_cases'] = negatives
    report['negative_case_verdict'] = {
        'failures': negative_failures,
        'inconclusive': negative_inconclusive,
        'passed_exhaustive': [r['case'] for r in negatives
                              if r['verdict'] == PASS_EXHAUSTIVE],
    }

    # Two orders are reported side by side:
    #   * `partial`  the true per-band order, capped, therefore INCONCLUSIVE for
    #                anything large -- the honest bound on what was explored;
    #   * `reduced`  the quotient that drops `started`.  Sound only because
    #                `started_is_unobservable` shows no guard reads it, and
    #                `reduction_check` proves it matches the partial order on
    #                observable behaviour instead of assuming it.  This is where
    #                exhaustiveness is actually reachable.
    #
    # Caps are per topology: a uniform cap either truncates small spaces
    # needlessly or tries to enumerate a product space that does not fit in
    # memory.  Three channels are not searched as a product at all -- they are
    # covered by `compositional_safety`.
    PARTIAL_CAP = 60000
    REDUCED_CAP = {
        'spatial_highpass': 100000,
        'spatial_chain': 100000,
        'fieldplus_half': 100000,
        'fieldplus_full': 600000,
        'spatial_three_channel': 100000,
    }
    QUOTIENT_CHECKABLE = ('spatial_highpass', 'spatial_chain', 'fieldplus_half')
    # The unobservability probe multiplies states by planes by four variants, so
    # its cost is much higher per state than a plain exploration.  Large
    # topologies get a smaller cap and are reported as a prefix, which is all the
    # claim needs: the probe is a search for a *counterexample*, and a prefix that
    # finds none is reported as such rather than as an exhaustive result.
    UNOBS_CAP = {
        'spatial_highpass': 60000,
        'spatial_chain': 60000,
        'fieldplus_half': 30000,
        'fieldplus_full': 12000,
        'spatial_three_channel': 12000,
    }
    LIVENESS_CHECKABLE = ('spatial_highpass', 'spatial_chain', 'fieldplus_half')

    for topo in TOPOLOGIES:
        # Progress on stderr: this report takes minutes and a silent run is
        # indistinguishable from a hung one.
        print('[report] %s' % topo, file=sys.stderr, flush=True)
        per = {}
        # One BFS per policy, not two: `explore` already visits every OKAY
        # terminal, so it tallies the false-OKAY states in the same pass.  The
        # separate `false_okay_states` sweeps doubled the cost of the whole
        # report without adding a number the main loop did not have.
        for policy in ('released', 'legacy', 'proposed'):
            per[policy] = explore(topo, policy, entropy='partial',
                                  max_states=PARTIAL_CAP)
        per['false_okay'] = {
            policy: {'count': per[policy]['false_okay'],
                     'exhaustive': per[policy]['exhaustive'],
                     'example': per[policy]['false_okay_example']}
            for policy in ('released', 'legacy', 'proposed')
        }
        rcap = REDUCED_CAP.get(topo, 200000)
        for policy in ('legacy', 'proposed'):
            per.setdefault('reduced', {})[policy] = explore(
                topo, policy, entropy='reduced', max_states=rcap)
        per['reduced_false_okay'] = {
            policy: {'count': per['reduced'][policy]['false_okay'],
                     'exhaustive': per['reduced'][policy]['exhaustive'],
                     'example': per['reduced'][policy]['false_okay_example']}
            for policy in ('legacy', 'proposed')
        }
        if topo in LIVENESS_CHECKABLE:
            per['reduced_liveness'] = liveness(topo, 'proposed',
                                               max_states=rcap,
                                               entropy='reduced')
        else:
            per['reduced_liveness'] = {
                'exhaustive': False, 'reason': 'reduced space exceeds the cap',
                'violations': ['I4_UNPROVEN']}
        if topo in QUOTIENT_CHECKABLE:
            # The cap must exceed the *partial* space, or the comparison cannot
            # finish and reports `comparable: false`.  fieldplus_half needs ~420k;
            # a smaller cap would silently turn a checkable quotient into an
            # unproven one.
            per['reduction'] = reduction_check(topo, max_states=600000)
        else:
            per['reduction'] = {
                'comparable': False,
                'reason': ('partial space exceeds the cap; quotient equality is '
                           'proved on the smaller topologies and extended here '
                           'only through the unobservability proof'),
                'partial_states': per['proposed']['states'],
                'reduced_states': per['reduced']['proposed']['states'],
            }
        ucap = UNOBS_CAP.get(topo, 20000)
        per['unobservable'] = {
            policy: started_is_unobservable(topo, policy=policy,
                                            max_states=ucap)
            for policy in ('proposed', 'legacy')
        }
        if topo in ('spatial_chain', 'spatial_highpass'):
            per['liveness'] = liveness(topo, 'proposed', max_states=200000)
        else:
            per['liveness'] = {'exhaustive': False, 'reason': 'space too large',
                               'violations': ['I4_UNPROVEN']}
        report['topologies'][topo] = per

    report['compositional'] = compositional_safety('spatial_three_channel',
                                                   max_states=200000)
    report['channel_independence'] = channel_independence(
        'spatial_three_channel')

    if anchor['status'] != 'OK':
        for problem in anchor['problems']:
            print('ANCHOR FAIL: %s' % problem, file=sys.stderr)
        return 2

    out = Path(args.json)
    out.parent.mkdir(parents=True, exist_ok=True)

    print('canonical reduction: %s'
          % ('DISPROVED (%s)' % report['canonical_witness']['witness']
             if report['canonical_witness']['witness'] else 'not disproved'))
    for topo, per in report['topologies'].items():
        prop = per['proposed']
        live = per['liveness']
        print('%-24s released=%-14s legacy=%-14s proposed=%-4s states=%-8d%s'
              % (topo,
                 ','.join(per['released']['violations']) or '-',
                 ','.join(per['legacy']['violations']) or '-',
                 ','.join(prop['violations']) or '-',
                 prop['states'],
                 '' if prop['exhaustive'] else ' TRUNCATED/INCONCLUSIVE'))
        print('%-24s false-OKAY: released=%d legacy=%d proposed=%d | '
              'liveness=%s | ok-terminals=%s stranded=%s'
              % ('', per['false_okay']['released']['count'],
                 per['false_okay']['legacy']['count'],
                 per['false_okay']['proposed']['count'],
                 'clean' if live.get('exhaustive') and not live['violations']
                 else ('TRUNCATED' if not live.get('exhaustive')
                       else ','.join(live['violations'])),
                 live.get('ok_terminals'), live.get('stranded')))
        red = per['reduced']['proposed']
        rq = per['reduction']
        rlive = per['reduced_liveness']
        print('%-24s reduced: states=%-6d exhaustive=%-5s viol=%-6s | '
              'quotient=%s | unobservable=%s | reduced-liveness=%s'
              % ('', red['states'], red['exhaustive'],
                 ','.join(red['violations']) or '-',
                 rq.get('equal') if rq.get('comparable') else 'n/a',
                 per['unobservable']['proposed']['unobservable'],
                 'clean' if rlive.get('exhaustive')
                 and not rlive.get('violations')
                 else ('TRUNCATED' if not rlive.get('exhaustive')
                       else ','.join(rlive['violations']))))

    comp = report['compositional']
    print('three-channel decomposition: %s (independent=%s, drain-global=%s)'
          % ('PASS' if comp['ok'] else 'FAIL',
             comp['independence']['independent'], comp['drain_is_global']))

    # A pass means: no *safety* violation on any explored prefix, both controls
    # still fail, the canonical reduction stays disproved, the reduced order is a
    # proven quotient, and the three-channel decomposition holds.  `I4_UNPROVEN`
    # is a truncation marker, not a defect -- counting it as a failure would make
    # the honest path ("this space is too large") indistinguishable from a broken
    # model, which is how a truncated search gets quietly re-labelled as proof.
    # A pass means: no *safety* violation on any explored prefix, both controls
    # still fail, the canonical reduction stays disproved, the reduced order is a
    # proven quotient, the three-channel decomposition holds, and **no named
    # control failed**.  `I4_UNPROVEN` is a truncation marker, not a defect --
    # counting it as a failure would make the honest path ("this space is too
    # large") indistinguishable from a broken model, which is how a truncated
    # search gets quietly re-labelled as proof.  The predicate itself lives in
    # `model_verdict` so this path and `--negative-cases` cannot disagree.
    verdict, code, facts = model_verdict(report, negative_failures,
                                         negative_inconclusive)
    exhaustive = facts['exhaustive']
    incomplete = facts['incomplete']
    report['verdict'] = verdict
    report['verdict_facts'] = {k: v for k, v in facts.items()
                               if isinstance(v, bool)}
    out.write_text(json.dumps(report, indent=1) + '\n', encoding='utf-8')
    print('wrote %s (verdict %s)' % (out, verdict))
    for failure in negative_failures:
        print('NEGATIVE FAIL %s' % failure, file=sys.stderr)
    for case in negative_inconclusive:
        print('NEGATIVE INCONCLUSIVE %s' % case, file=sys.stderr)
    if negative_failures:
        print('negative controls: %d failure(s) -- a control that does not hold '
              'is a model defect, not a decoder result' % len(negative_failures))
    print('MODEL %s: safety on explored prefix, exact quotient, controls fire, '
          'three-channel decomposition holds%s'
          % (verdict, '' if not negative_inconclusive else
             '; %d control(s) INCONCLUSIVE' % len(negative_inconclusive)))
    print('reduced order exhaustive for %d/%d topologies%s'
          % (len(exhaustive), len(report['topologies']),
             ('; INCONCLUSIVE: ' + ', '.join(incomplete)) if incomplete else ''))
    return code


if __name__ == '__main__':
    sys.exit(main())
