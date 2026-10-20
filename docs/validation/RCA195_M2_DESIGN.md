# RCA-195 Milestone 2 — liveness-safe band-0 publication contract (DESIGN, rev 5)

**Status: `DESIGN_ONLY` / `REDUCED_ORDER_QUOTIENT_PROVEN` / 4 of 5 topologies exhaustive.** No
decoder patch, no `upstream/` edit, no merge, no release.

Revision 5 answers the fourth independent review. That review accepted rev 4 as
`REV4_REVIEW_POSITIVE_FOR_HONEST_BOUND / FULL_SCHEDULER_SAFETY_PENDING` and named exactly two
remaining engineering tasks: a **sound, observation-preserving reduction**, or a compositional
per-`(channel,index,generation)` proof with explicit cross-channel worker interaction. Both are now
delivered, and each comes with the proof that makes it more than a smaller space.

## 0. Revisions

| Rev | Disposition | What it established |
| --- | --- | --- |
| 1 | `MODEL_INCOMPLETE` | — (own-input-plane publication, free publications, atomic claim/execute) |
| 2 | `SOURCE_MODEL_REV2_IMPROVED` | real producer edges, correct output plane, scheduled transitions |
| 3 | `REV3_STRUCTURAL_PROGRESS / ..._BLOCKED` | worker identity, COMPUTED/PUBLISHED split, field-plus HALF/FULL, three channels — but the "partial order" was a canonical prefix |
| 4 | `REV4_REVIEW_POSITIVE_FOR_HONEST_BOUND` | true per-band order, canonical reduction **disproved**, bounded prefixes labelled `INCONCLUSIVE` |
| 5 | this document | a **proven** reduced order (exact quotient) and a **compositional** three-channel proof |

## 1. The reduced order, and why it is sound rather than merely smaller

Revision 4's honest position was that the true partial-order space is too large: `fieldplus_full`
exceeded memory at 4 M states and three topologies stayed `INCONCLUSIVE`. The tempting shortcut —
raise the cap until something finishes and call it proven — is explicitly forbidden by the review,
and rightly so.

The reduction here is different in kind. Under `proposed` and `legacy`, the `started` flag word is
**never read by any decision**:

| Read site | Which policy reads it |
| --- | --- |
| `Plane.release_ok()` (`valid == started`) | `released` only |
| `entropy_actions()` precondition | `started`-gated events, which `reduced` does not emit |
| state key, clone | bookkeeping, not a decision |

So `reduced` drops the word and the per-band `e_started` announcements entirely: a band is either
unpublished or published. That is only legitimate if the dropped bits are genuinely unobservable,
and the model now **checks** that instead of asserting it:

- **`started_is_unobservable()`** permutes the `started` word over every band entropy may announce,
  for every reachable state, and recomputes the observable projection — entries, planes (`valid`,
  `generation`), busy set, **claimable set**, error and drain state. 708 804 probes on
  `spatial_chain`, zero projections changed.
- **`reduction_check()`** compares the full reachable *observable projection sets* of `partial` and
  `reduced` — not endpoints, not samples. For the topologies where the partial space finishes the
  two sets are **identical**: `only_partial = 0`, `only_reduced = 0`.

Because the comparison is over projections reached by *all* actions (claim, admit, execute, publish,
generation change, drain), it includes worker interleavings; the quotient is not an entropy-only
argument.

**`released` is refused, not approximated.** Its predicate is literally `valid == started`, so the
word is observable there. `Machine(...)` raises rather than model a different decoder:

```
ValueError: entropy='reduced' is unsound for the released policy: its claim predicate reads `started`
```

This asymmetry is itself the finding: the shipped bug is exactly that the *shipped* policy depends on
an announcement word that does not mean completion. `proposed` does not depend on it, which is why
the proposed contract admits a much smaller exhaustive analysis.

## 2. Results — with the quotient, exhaustiveness is actually reached

| Topology | partial (cap) | reduced | reduced exhaustive | quotient | unobservable | reduced liveness |
| --- | --- | --- | --- | --- | --- | --- |
| `spatial_highpass` | 176 | **46** | yes | **exact** | yes | clean (1 OKAY, 0 stranded) |
| `spatial_chain` | 76 440 (capped) | **3 056** | yes | **exact** | yes | clean |
| `fieldplus_half` | 83 507 (capped) | **8 476** | yes | **exact** | yes | clean |
| `fieldplus_full` | 105 130 (capped) | **542 464** | yes | n/a (partial capped) | yes | not established |
| `spatial_three_channel` | 355 198 (capped) | 206 374 | no | n/a | yes | not established |

Four of five topologies are now **exhaustive on the reduced order**, where rev 4 had one. The
partial column stays as the honest upper bound on what was enumerated in the unreduced space;
`fieldplus_full` and the three-channel case remain capped there, which is why their quotient is
reported `n/a` rather than claimed.

`I4_UNPROVEN` in a violation list is a truncation marker, not a safety defect.

### False OKAY, both controls intact

| Topology | released | legacy (rev 1) | proposed (partial) | proposed (reduced) |
| --- | --- | --- | --- | --- |
| `spatial_chain` | 7 132 | 25 194 | 0 | **0** |
| `spatial_highpass` | 404 | 90 | 0 | **0** |
| `fieldplus_half` | 5 248 | 15 800 | 0 | **0** |
| `fieldplus_full` | 2 722 | 7 943 | 0 | **0** |
| `spatial_three_channel` | 898 | 5 426 | 0 | **0** |

The reduced order reproduces the same zero for `proposed` and the same failures for both controls,
which is what an exact quotient must do.

## 3. Three channels: proof by decomposition, not by a product search

A full product search over three channels does not fit. The argument is instead three claims, each
checkable:

1. **Independence (structural).** `channel_independence()` reads the topology table and confirms no
   entry reads or writes another channel's plane: every `(plane_index, band)` an entry touches
   resolves to that entry's own channel. Result: `independent: true`, channels `[0, 1, 2]`.
2. **Per-channel safety (exhaustive).** Each channel is modelled alone — same entry, same read and
   write sets, same required output — and explored to completion: **3 056 states each, exhaustive,
   zero safety violations, 8 OKAY terminals each**.
3. **Fail-closed draining (structural).** The drain predicate requires every entry of every channel
   to be terminal with no terminal error. A channel that starved for a worker therefore yields
   `CODEC_ERROR_BAD_FRAME`, never a silent success. Verified on a constructed state: drain with a
   non-terminal ch1 entry reports `reported_ok: False`.

Together these cover every cross-channel worker interleaving, because a worker slot is the *only*
resource one channel can take from another, and the conclusion holds whether or not a channel ever
obtains one. The three-channel product space itself is not claimed exhaustive; the decomposition is.

## 4. Contract (unchanged from rev 4, restated)

`QUEUED → CLAIMED → ADMIT → EXECUTING → COMPUTED → PUBLISHED | FAILED`, plus `DROPPED`.

- **R1** claim gated on the published read set for the current generation.
- **R2** ownership token; a second live producer of the same `(channel, index, band, generation)` is
  refused (`I7_SINGLE_OWNER`).
- **R3** admission re-checks generation; failure is terminal.
- **R4** execution fails closed on a missing read band.
- **R5** publication is its own transition and re-checks generation.
- **R6** drain verifies completion and propagates the first terminal error.
- **R7** no wait/spin inside `ReconstructWaveletBand` (shared with the drain loop, pool capped at 4,
  `decoder.c:1596`).
- **R8** the hollow `AllTransformBandsValid` is replaced by the required-set closure.

## 5. Negative controls (all passing)

| Case | Expected | Measured |
| --- | --- | --- |
| spatial index 1, `0xE` satisfied, index-2 band 0 withheld, `proposed` | 0 claims | 0 |
| same state, `legacy` | ≥1 claim | 62 claim states |
| temporal index 2, all producers withheld, `proposed` | 0 claims | 0 |
| same state, `legacy` | ≥1 claim | 1 008 claim states |
| stale generation after claim | terminal, never OKAY | fails closed |
| duplicate writer of a published band | refused | `I7` never fires |
| four workers, unsatisfiable entries | never OKAY | 0 OKAY |
| later band's `started` before the earliest band's | reachable | reachable (59 049 states) |
| per-channel output closure detects missing ch1 | yes | yes |
| `reduced` against `released` | refused | `ValueError` |

The canonical-prefix witness stays as a red control and still fires:

```json
{"witness": {"earlier": "e_started:(0, 0, 1)", "later": "e_valid:(0, 2, 3)",
             "claimable_after_earlier": [], "claimable_after_later": ["E2"]},
 "canonical_reaches_window": false}
```

## 6. What this still does not establish

- **`fieldplus_full` and three channels are not exhaustive as products.** `fieldplus_full` is
  exhaustive on the reduced order (542 464 states) but its *partial* space was capped, so its
  quotient is `n/a`. Three channels rely on the decomposition, not on a product search.
- **No memory-ordering model.** The happens-before argument remains a design statement, not a
  checked property. No C11/C++ atomic order is verified.
- **No timing, no chroma geometry, no real arithmetic.** `I2_NO_DOUBLE` is enforced but unreachable
  because no policy permits a second claim of the same entry.
- **The reduction is checked on the reachable set, not proved symbolically.** It is exact for these
  five topologies; a new topology needs its own `reduction_check` run.

## 7. Source anchoring

`--check` remains the CTest gate and is anchor-only (**0.1 s**, 5 edges): it pins the lowpass call
site, both lowpass publications, the `index > 4` / `== 4` mapping, the highpass publication, both
temporal publications and the assert-only temporal guard, all four silent returns, both subband
tables, `CODEC_NUM_SUBBANDS_HALF` / `DECODED_SUBBAND_MASK_HALF` / the `CanSkipChannel` HALF branch,
the released `BANDS_ALL_VALID` and `BANDS_ALL_STARTED` forms, the queue/drain anchors and the hollow
`AllTransformBandsValid`. **`anchor: OK`, zero problems.**

## 8. Next action

The two tasks the review named are delivered: an observation-preserving reduction with a proof that
it is exact, and a compositional three-channel argument with explicit worker interaction. The
private scratch-SDK intervention stays blocked until this model is accepted at its stated scope —
it is still `DESIGN_ONLY`, still no patch, and the remaining `INCONCLUSIVE` entries are labelled
rather than smoothed over.

No production `upstream/` patch, no spin/poll fix, no merge, no release. `#120` remains
`REFERENCE_DECODER_NONDETERMINISTIC / UNRESOLVED`.

## 9. Files

- `tools/rca195_m2_model.py` — revision 5; `--check`, `--json`, `--self-test`,
  `--canonical-witness`, `--reduction`, `--confluence`, `--negative-cases`.
- `docs/validation/rca195_m2_model_2026-10-20.json` — per-topology partial and reduced results,
  quotient equality, unobservability probes, compositional proof and negative controls.
- `docs/validation/RCA195_BAND0_CONTRACT.md` — Milestone-1 source map.
