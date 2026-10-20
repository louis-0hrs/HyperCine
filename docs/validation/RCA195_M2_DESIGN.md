# RCA-195 Milestone 2 — liveness-safe band-0 publication contract (DESIGN, rev 2)

**Status: `DESIGN_ONLY` / model `MODEL_COMPLETE`.** No decoder patch, no `upstream/` edit, no merge,
no release. Revision 1 of this design and of `tools/rca195_m2_model.py` was rejected by the
maintainer review as `MODEL_INCOMPLETE`; this revision answers that review.

## 0. Why revision 1 was withdrawn

The review named four faults, and all four were real. The first is the one that matters, because
it made the model structurally blind to the defect it claimed to fix:

1. **Wrong output plane.** Revision 1 published a transform's result onto the entry's **own input
   wavelet** (`publish_valid(0)` on `plane(index)`). The source writes the lowpass into
   `transform->wavelet[index-1]` (`decoder.c:13023`, `:13051`). A model that writes onto its own
   input can fabricate the very band it was supposed to notice was missing.
2. **Free publications.** Any band could be published with no owning producer. Producer edges
   were not represented at all.
3. **Atomic claim/execute/publish.** One `step()` did all three, so no interleaving involving a
   claimed-but-unexecuted entry, a late generation change, or two competing workers could exist.
   `I5_PROGRESS` was never evaluable, `I4_BOUNDED` was uninformative because `drain` was always
   available, and `I2_NO_DOUBLE` could not fire.
4. **A truncation read as a proof.** The `chain` scenario hit the DFS limit and its empty
   violation list was presented alongside exhaustive results.

Revision 2 removes all four and keeps revision 1 only as a **negative regression policy** that
must fail.

## 1. The mechanism, in source terms

The inverse transforms begin with a guard that returns **silently**:

```c
// wavelet.c:5699-5706 (TransformInverseSpatialQuantLowpass)
if (input->band[0] == NULL) return;
if (input->band[1] == NULL) return;
if (input->band[2] == NULL) return;
if (input->band[3] == NULL) return;
```

An inverse handed a wavelet missing any of its four bands returns without writing anything and
without setting an error. The caller therefore still reports OKAY. That is the actual mechanism,
and it is modelled explicitly as a `NOOP` terminal state.

The queue's contribution is separate: `done = 1` (`entropy_threading.c:271`) records
**admission**, not execution; the entry is re-tested after unlocking (`:285`) against the same
`valid == started` predicate; and `WaitForTransformThread` (`decoder.c:24283`) zeroes the queue
with no completeness check and no `decoder->error`.

## 2. Producer edges, pinned to source

Each entry has an explicit **read set** and **write set**, and the write target is a *different*
plane:

| Entry | Reads | Writes | Source |
| --- | --- | --- | --- |
| spatial `index=2` | plane 2, bands 0–3 | plane 1, band 0 | `decoder.c:13045`, `:13051` |
| spatial `index=1` | plane 1, bands 0–3 | plane 0, band 0 | same |
| spatial `index=3` | plane 3, bands 0–3 | plane 2, band 1 | `decoder.c:13173` |
| field-plus `index=2` | plane 2, bands 0–1 | plane 0 **and** 1, band 0 | `decoder.c:13255-13256` |

`TransformInverseSpatialQuantLowpass` reads **all four** bands of the input wavelet
(`wavelet.c:5743-5746`, `:5763-5766`), which is why a `0xE` mask is insufficient: `0xE` names only
the entropy bands, and band 0 of a non-top plane is produced by the adjacent transform. The
review's first counterexample is exactly this.

The subband tables decide which plane receives band 0 from entropy (`decoder.c:11596` intra,
`:11191` group). Planes whose band 0 is absent from those tables get it from the adjacent
transform, and the model treats a missing such band as a defect.

## 3. Proposed contract

State machine per entry: `QUEUED → CLAIMED → ADMIT → EXECUTING → PUBLISHED | FAILED`, plus
`DROPPED`.

- **R1 — claim gated on the published read set.** Not on `valid == started`, and not on an
  entropy mask. An entry becomes claimable only when every band in its read set is `valid` for
  the plane's current generation.
- **R2 — ownership.** `done` becomes a three-valued token (`FREE`/`OWNED`/`CONSUMED`), so
  `num_entries` stops being the only record of what left the queue.
- **R3 — admission re-checks generation, not the predicate.** A failed generation check is
  **terminal**: `decoder->error = CODEC_ERROR_BAD_FRAME` and the entry becomes `FAILED`.
- **R4 — execution fails closed on a missing read band.** The `NOOP` path does not exist in the
  proposed policy: a missing input at execution time is a terminal error, not a silent return.
- **R5 — drain verifies.** The queue may only be discarded when every entry is terminal;
  otherwise the first terminal error is set and the public call fails.
- **R6 — forward progress.** No wait/spin inside `ReconstructWaveletBand`, since it shares
  `EntropyWorkerThreadProc` with the drain loop and the pool is capped at 4
  (`decoder.c:1596`). An entry with missing inputs stays `QUEUED`; an idle worker returns.
- **R7 — generation.** Entries carry the allocation generation, so `ReallocWaveletEx` cannot
  silently stale a queued entry.
- **R8 — output boundary.** The hollow `AllTransformBandsValid` is replaced at its seven live
  sites by the same required-set check, in release configurations.

## 4. Model results (revision 2)

Breadth-first over the reachable state space, so an exhausted frontier is a proof at this
abstraction rather than a sample. Two topologies:

**`spatial_chain`** — planes 0/1/2 at four bands; entries `E2` (reads plane 2, writes plane 1
band 0) and `E1` (reads plane 1, writes plane 0 band 0).

| Policy | Invariants violated | Success-with-missing-band states | States |
| --- | --- | --- | --- |
| `released` | `I1`, `I3` | **310 / 877** | 877, exhaustive |
| `legacy` (rev 1) | `I1`, `I3` | **46 / 110** | 110, exhaustive |
| `proposed` | none | **0 / 480** | 480, exhaustive |

**`fieldplus_temporal`** — plane 2 has two bands; `T2` reads both and writes band 0 into planes 0
and 1.

| Policy | Invariants violated | Success-with-missing-band states | States |
| --- | --- | --- | --- |
| `released` | `I1`, `I3` | **55 / 129** | 129, exhaustive |
| `legacy` (rev 1) | `I1`, `I3` | **51 / 119** | 119, exhaustive |
| `proposed` | none | **0 / 348** | 348, exhaustive |

The middle column of the `false-OKAY states` block is the regression the review asked for, stated
as reachability rather than assertion: **the retired revision-1 rule reaches a public success with
a transform-only band absent, in both topologies, and the proposed rule reaches none.**

An example `legacy` terminal state:

```json
{"missing": [{"index": 0, "band": 0}, {"index": 1, "band": 0}],
 "error": null,
 "entries": {"E2": "DISCARDED", "E1": "DISCARDED"}}
```

Both transforms discarded at the drain, two transform-only bands absent, `error` null, public
result OK — the failure shape revision 1 could not see.

### Two reductions, both stated

- **Entropy events are fired in one canonical order.** They are independent bit-sets whose mutual
  order cannot affect any queue decision, so permuting them explores nothing new. This is a
  symmetry reduction, not a restriction.
- **A plane's generation may change at most once.** Without the cap the counter is unbounded, the
  state space is infinite, and "exhaustive" would be unprovable for the wrong reason. One change
  after a claim is the case the design must survive.

### What the model still does not show

Two planes, one channel, two entries. It does not show three channels, FULL/HALF geometry, the
`index>3` field-plus branch, real `ReconstructWaveletBand` arithmetic, or any timing. It is a
proof about *this* state machine at *this* abstraction. Extending the topology set is the next
step, not a claim already made.

`I2_NO_DOUBLE` is checked but not reachable in either policy, because neither permits a second
claim of the same entry; the TSan observation about two threads inside
`TransformInverseSpatialQuantLowpass` concerns `wavelet[index-1]` vs `wavelet[index]`, not a
duplicate queue execution, and is not claimed here.

## 5. Deterministic test cases Milestone 2 requires

| Case | Construction | Expected |
| --- | --- | --- |
| missing adjacent producer | withhold plane `i-1` band 0 | terminal error, non-OKAY |
| missing temporal input | withhold one of plane 2's two bands | terminal error, non-OKAY |
| claimed-but-not-executed | consume an entry, skip its execution | terminal error, not OKAY |
| duplicate execution | present the same entry twice | second claim refused |
| stale generation | reallocate the wavelet with a queued entry pending | `CANCELLED`/`FAILED`, terminal |
| pool saturation | 4 workers on unsatisfiable entries | drain fails closed |
| positive control | unmodified decode | OKAY, digest equals the modal control |
| FULL/HALF matrix | both resolutions, three channels | required sets as in §2 |

## 6. Source anchoring

`--check` fails closed if any claim above stops matching the frozen tree. It pins: the lowpass
call site, both lowpass publication sites, the highpass publication, both temporal publications,
all four silent `input->band[N] == NULL` returns, both subband tables, the released
`BANDS_ALL_VALID` form and the drain anchors. Currently **`anchor: OK`, zero problems.**

## 7. Files

- `tools/rca195_m2_model.py` — revision-2 model, `--check` / `--json` / `--self-test`.
- `docs/validation/rca195_m2_model_2026-10-20.json` — per-policy results incl. false-OKAY counts.
- `docs/validation/RCA195_BAND0_CONTRACT.md` — Milestone-1 source map.

## 8. Next action

Review the corrected model and the required-bit / producer-edge tables. If accepted, the first
implementation step is a **private test-only** patch implementing R1+R3+R4+R5 only — claim gating
on the read set, generation re-check, fail-closed execution, verifying drain — which is the
minimum that converts both observed false-OKAY shapes into terminal errors. R6/R7/R8 follow only
after that conversion is demonstrated, and the topology set must be extended to three channels and
FULL/HALF before any such patch is called a candidate.
