# RCA-195 Milestone 2 — liveness-safe band-0 publication contract (DESIGN, rev 3)

**Status: `DESIGN_ONLY` / model `MODEL_COMPLETE` at the stated abstraction.** No decoder patch, no
`upstream/` edit, no merge, no release. Revision 3 answers the second independent source review of
PR #197; it keeps revision 1 only as a negative regression policy and does not retract the
Milestone-1 v3 injection evidence.

## 0. What each revision changed

| Revision | Disposition | What it fixed |
| --- | --- | --- |
| rev 1 | `MODEL_INCOMPLETE` | — (published onto its own input plane, invented free publications, atomic claim/execute) |
| rev 2 | `SOURCE_MODEL_REV2_IMPROVED` | real producer edges, correct output plane, scheduled transitions |
| rev 3 | this document | worker ownership in the state key, per-band partial order, separated execute/publish, quiescent-drain liveness, five topologies |

The second review named three precise faults, and all three were real:

1. **State dedup dropped worker ownership.** `Machine.key()` carried `tuple(sorted(self.busy))` but
   not `entry.worker`, although that field decides which slot `_free_worker` releases. Two opposite
   entry-to-worker assignments with the same `busy={0,1}` collapsed into one BFS node.
2. **The entropy "symmetry reduction" was asserted, not proven.** Only the next canonical event was
   ever offered, so intermediate readiness subsets that differ in a queue-relevant way could never
   be reached.
3. **Forward progress was not modelled as claimed.** `drain` was always available, so an eager
   fail-closed abort was indistinguishable from liveness, and no invariant required a valid decode
   to actually complete.

## 1. The mechanism, in source terms

The spatial inverses begin with a guard that returns **silently**:

```c
// wavelet.c:5699-5706 and the same form in TransformInverseSpatialQuantHighpass
if (input->band[0] == NULL) return;
if (input->band[1] == NULL) return;
if (input->band[2] == NULL) return;
if (input->band[3] == NULL) return;
```

An inverse handed a wavelet missing any band returns without writing and without setting an error,
so the caller still reports OKAY. The temporal inverse has no such guard at all — its only
protection is `assert(BANDS_ALL_VALID(temporal))` (`decoder.c:13247`), which is compiled out in
Release.

The queue contributes the second half: `done = 1` (`entropy_threading.c:271`) records **admission**,
not execution; the entry is re-tested after unlocking against the same `valid == started` predicate;
and `WaitForTransformThread` (`decoder.c:24283`) zeroes `started/num_entries/next_entry/free_entry`
with no completeness check and no `decoder->error`.

## 2. Producer edges, pinned to source

Each entry has an explicit **read set** and **write set**, and the write target is a *different*
plane:

| Entry | Reads | Writes | Source |
| --- | --- | --- | --- |
| spatial `index=2` | plane 2, bands 0–3 | plane 1, band 0 | `decoder.c:13045`, `:13051` |
| spatial `index=1` | plane 1, bands 0–3 | plane 0, band 0 | same |
| spatial `index=3` | plane 3, bands 0–3 | plane 2, band 1 | `decoder.c:13173` |
| field-plus `index>3` | plane `index`, bands 0–3 | plane `index-1` (or `index-2`) band 0 | `decoder.c:13064`, `:13112` |
| field-plus `index=3` | plane 3, bands 0–3 | plane 2, band 1 | `decoder.c:13173` |
| field-plus `index=2` | plane 2, bands 0–1 | planes 0 **and** 1, band 0 | `decoder.c:13255-13256` |

`TransformInverseSpatialQuantLowpass` reads **all four** bands of the input wavelet
(`wavelet.c:5743-5746`, `:5763-5766`), which is why a `0xE` mask is insufficient: `0xE` names only
the entropy bands, and band 0 of a non-top plane is produced by the adjacent transform.

`index > 3` deliberately maps to `index-1` for `index > 4` and to `index-2` for `index == 4`
(`decoder.c:13064`), which is how plane 4 band 0 is written by entry 5 and never by entry 4. The
model encodes that mapping explicitly rather than assuming `index-1` everywhere.

## 3. FULL versus HALF is a structural difference, not a label

`CODEC_NUM_SUBBANDS_HALF == 11` (`codec.h:648`) and `DECODED_SUBBAND_MASK_HALF` is `0x7FF`; FULL
decodes all 17 subbands of the group table. For a field-plus group that means:

- **HALF** decodes subbands 0–10 → planes 5, 4 and 3 plus the temporal plane 2. Plane 4 band 0 comes
  from entry 5, plane 2 bands 0/1 from entries 4 and 3, then the temporal inverse fills the frame
  wavelets. `BANDS_ALL_STARTED` is false for the frame planes, so no index-1 entry exists.
- **FULL** decodes all 17 subbands → frame planes 0 and 1 also receive bands 1–3 from entropy. The
  frame wavelets are then filled by the temporal inverse plus entropy, and still no index-1 entry
  exists: `ReconstructWaveletBand` has branches for `index > 3`, `index == 3` and `index == 2` only
  under `TRANSFORM_TYPE_FIELDPLUS`.

Both are modelled separately (`fieldplus_half`, `fieldplus_full`) so that a required-band claim
cannot be silently transferred from one resolution to the other.

## 4. Proposed contract

Per entry: `QUEUED → CLAIMED → ADMIT → EXECUTING → COMPUTED → PUBLISHED | FAILED`, plus `DROPPED`.
`COMPUTED` is new in revision 3: the inverse has run and its writes are staged but not yet visible.
It exists so that "claimed but not published" is a reachable state rather than an atomic blur.

- **R1 — claim gated on the published read set.** Not on `valid == started`, not on an entropy mask.
  Every band in the read set must be `valid` for the plane's current generation.
- **R2 — ownership.** `done` becomes a three-valued token (`FREE`/`OWNED`/`CONSUMED`), so
  `num_entries` stops being the only record of what left the queue. A second live producer of the
  same `(channel, index, band, generation)` is refused (`I7_SINGLE_OWNER`).
- **R3 — admission re-checks generation, not the predicate.** A failed generation check is
  **terminal**: `decoder->error = CODEC_ERROR_BAD_FRAME`.
- **R4 — execution fails closed on a missing read band.** The silent-return path does not exist in
  the proposed policy.
- **R5 — publication is its own transition, and re-checks generation.** Writes are applied only at
  `PUBLISH`, after a second generation check, so an allocation swap between compute and publish is a
  terminal error rather than a write into a stale buffer.
- **R6 — drain verifies.** The queue may only be discarded when every entry is terminal; otherwise
  the first terminal error is set and the public call fails.
- **R7 — forward progress.** No wait/spin inside `ReconstructWaveletBand`, since it shares
  `EntropyWorkerThreadProc` with the drain loop and the pool is capped at 4 (`decoder.c:1596`). An
  entry with missing inputs stays `QUEUED`; an idle worker returns.
- **R8 — output boundary.** The hollow `AllTransformBandsValid` (non-NULL test only,
  `decoder.c:2050-2076`) is replaced at its live sites by the required-set closure.

## 5. Model results (revision 3)

Breadth-first over the reachable state space; an exhausted frontier is a proof at this abstraction,
a truncated one reports `I4_UNPROVEN`. All five topologies, two workers, canonical entropy schedule,
`drain=anytime` unless stated.

| Topology | released | legacy (rev 1) | proposed | proposed states |
| --- | --- | --- | --- | --- |
| `spatial_chain` | `I1`,`I3` | `I1`,`I3` | — | 70 |
| `spatial_highpass` | `I1`,`I3` | `I1`,`I3` | — | 32 |
| `fieldplus_half` | `I1`,`I3` | `I1`,`I3` | — | 422 |
| `fieldplus_full` | `I1`,`I3` | `I1`,`I3` | — | 446 |
| `spatial_three_channel` | `I1`,`I3` | `I1`,`I3` | — | 8 758 |

All searches exhaustive (`bounded: true`). The three-channel released run needs ~769 k states because
the shipped predicate claims all six entries at once; the topology carries its own state cap so the
bound is real rather than a truncation read as a proof.

**Success-with-missing-band reachability** (the review's negative regression, stated as a property):

| Topology | released | legacy | proposed |
| --- | --- | --- | --- |
| `spatial_chain` | 225 | 58 | **0** |
| `spatial_highpass` | 35 | 12 | **0** |
| `fieldplus_half` | 1 901 | 596 | **0** |
| `fieldplus_full` | 2 153 | 644 | **0** |
| `spatial_three_channel` | 37 037 | 5 844 | **0** |

An example `legacy` terminal state:

```json
{"missing": [{"channel": 0, "index": 0, "band": 0},
             {"channel": 0, "index": 1, "band": 0}],
 "error": null,
 "entries": {"E2": "DISCARDED", "E1": "DISCARDED"}}
```

Both transforms discarded at the drain, two required bands absent, `error` null, public result OK.

### Liveness is now a positive property

`drain=quiescent` removes `drain` from the action set while any transition remains. Under it, every
reachable state of every topology can still reach an OKAY terminal:

| Topology | OKAY terminals | states that can reach OKAY | stranded |
| --- | --- | --- | --- |
| all five | 1 | all | **0** |

This is the invariant the review asked for: the proposed policy does not merely avoid false OKAY, it
*completes* a valid decode. `drain=anytime` is retained and reported for what it is — a safe abort.
The model does not claim a wall-clock bound; `I4_BOUNDED` is finite-graph enumeration, and the
document says so.

### Confluence is checked, not assumed

Under `entropy=partial` the model offers every band whose `started` (or, after `started`, its
`valid`) is still outstanding. For every reachable state it swaps each pair of independently enabled
publications and compares the resulting state keys:

| Topology | pairs checked | counterexamples |
| --- | --- | --- |
| `spatial_chain` | 330 | 0 |
| `spatial_highpass` | 20 | 0 |
| `fieldplus_half` | 1 224 | 0 |
| `fieldplus_full` | 2 416 | 0 |
| `spatial_three_channel` | 91 770 | 0 |

The canonical total order is therefore a valid reduction *for these topologies and this state
machine*, and the model now fails if a swap ever changes the successor. It is still a reduction:
confluence is checked over the reachable set, not proven symbolically for arbitrary topologies.

### Named negative cases

| Case | Expected | Measured |
| --- | --- | --- |
| spatial index 1, `0xE` satisfied, index-2 band 0 withheld, `proposed` | 0 claims | 0 |
| same state, `legacy` | ≥1 claim | 62 |
| temporal index 2, all producers withheld, `proposed` | 0 claims | 0 |
| same state, `legacy` | ≥1 claim | 1 008 |
| stale generation after claim | terminal error, never OKAY | fails closed |
| duplicate writer of a published band | refused | `I7` never fires |
| four workers, unsatisfiable entries | never OKAY | 0 OKAY |

## 6. What the model still does not show

One channel except for `spatial_three_channel`, which models three independent intra chains and
therefore exercises the shared worker pool but not chroma geometry. No real arithmetic, no timing,
no full 17-band field-plus `index > 5` chain beyond the mapping anchors, no memory-ordering model
(the happens-before argument stays a design statement, not a checked property). `I2_NO_DOUBLE` is
enforced but unreachable in every policy, because no policy permits a second claim of the same entry.

## 7. Source anchoring

`--check` fails closed if any claim above stops matching the frozen tree: the lowpass call site and
both publication sites, the `index > 4` mapping, the highpass publication, both temporal
publications, the assert-only temporal guard, all four silent returns, both subband tables,
`CODEC_NUM_SUBBANDS_HALF`/`DECODED_SUBBAND_MASK_HALF`/the `CanSkipChannel` HALF branch, the released
`BANDS_ALL_VALID` and `BANDS_ALL_STARTED` forms, the queue/drain anchors and the hollow
`AllTransformBandsValid`. Currently **`anchor: OK`, zero problems.**

## 8. Deterministic test cases Milestone 2 still requires at the SDK level

These remain *design* requirements; none of them has been run, because no SDK patch exists.

| Case | Construction | Expected |
| --- | --- | --- |
| missing adjacent producer | withhold plane `i-1` band 0 | terminal error, non-OKAY |
| missing temporal input | withhold one of plane 2's two bands | terminal error, non-OKAY |
| claimed-but-not-executed | drop the entry after `done = 1` | terminal error, not OKAY |
| publish-after-generation-change | reallocate between compute and publish | `FAILED`, not OKAY |
| duplicate execution | present the same entry twice | second claim refused |
| pool saturation | 4 workers on unsatisfiable entries | drain fails closed |
| positive control | unmodified decode | OKAY, digest equals the modal control |
| FULL/HALF matrix | both resolutions, three channels | required sets as in §2–§3 |

## 9. Next action

The model-fidelity gates the review named are now represented, checked and reported. The next step
is review of *this* revision: the topology set covers three channels and FULL/HALF, the scheduler
distinguishes claim/admit/execute/publish, and liveness is a positive requirement rather than an
absence of false OKAY.

Only after that review should a **bounded private scratch-SDK** implementation of R1+R3+R4+R5+R6 be
considered — claim gating on the read set, generation re-check at admission *and* publication,
fail-closed execution, verifying drain — as the minimum that converts both observed false-OKAY
shapes into terminal errors. R7/R8 follow only after that conversion is demonstrated. No production
`upstream/` edit, no spin/poll fix, no root/main merge, `#120` stays
`REFERENCE_DECODER_NONDETERMINISTIC / UNRESOLVED`.

## 10. Files

- `tools/rca195_m2_model.py` — revision-3 model; `--check`, `--json`, `--self-test`,
  `--confluence`, `--negative-cases`.
- `docs/validation/rca195_m2_model_2026-10-20.json` — per-policy, per-topology results including
  false-OKAY counts, liveness and confluence.
- `docs/validation/RCA195_BAND0_CONTRACT.md` — Milestone-1 source map.
