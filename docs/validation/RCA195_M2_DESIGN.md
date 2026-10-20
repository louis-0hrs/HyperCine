# RCA-195 Milestone 2 — liveness-safe band-0 publication contract (DESIGN, rev 4)

**Status: `DESIGN_ONLY` / `PARTIAL_ORDER_SAFETY_BOUNDED` / large topologies `INCONCLUSIVE`.** No
decoder patch, no `upstream/` edit, no merge, no release.

Revision 4 answers the third independent source review. All three findings were real, and the first
one **invalidated a claim I had made in revision 3**: the "partial order" was still a canonical
prefix in disguise, and the confluence check I used to justify it compared endpoints rather than
intermediate observations. The check now disproves the reduction instead of confirming it.

## 0. Revisions

| Rev | Disposition | What it fixed / broke |
| --- | --- | --- |
| 1 | `MODEL_INCOMPLETE` | own-input-plane publication, free publications, atomic claim/execute |
| 2 | `SOURCE_MODEL_REV2_IMPROVED` | real producer edges, correct output plane, scheduled transitions |
| 3 | `REV3_STRUCTURAL_PROGRESS / PARTIAL_ORDER_AND_3CH_OUTPUT_GATE_BLOCKED` | worker identity, COMPUTED/PUBLISHED split, field-plus HALF/FULL, three channels — **but** `entropy=partial` returned only the first eligible event, and `confluence()` compared endpoints |
| 4 | this document | true per-band partial order, observable-aware confluence that **fails**, per-channel output closure |

## 1. The review's three findings, and what each now is

### 1.1 `entropy='partial'` was a canonical prefix — fixed, and the fault is now a regression test

Revision 3's `entropy_action()` looped over the outstanding events and **returned the first
eligible one**. With a normal consecutive `started → valid` per band the earliest outstanding event
is always eligible, so no later band could ever be selected first. The advertised
independent-readiness subsets were unreachable. `events_done` was also a prefix cursor, incremented
by *any* event, so a later event could advance it past an unsent earlier one and strand it.

Revision 4 replaces the cursor with an explicit outstanding-event set:

- `Machine.pending` is a `frozenset` of event indices, part of the state key.
- `entropy_actions()` returns **every** enabled event, not the first.
- `entropy='canonical'` still exists, but only as a labelled reduction that must now justify itself.
- `--canonical-witness` measures whether the reduction is faithful.

It is not. Measured on `spatial_chain`:

```json
{"witness": {"earlier": "e_started:(0, 0, 1)",
             "later":   "e_valid:(0, 2, 3)",
             "claimable_after_earlier": [],
             "claimable_after_later":   ["E2"]},
 "canonical_reaches_window": false}
```

Publishing `e_valid:(0,2,3)` makes `E2` claimable. Publishing the earlier-numbered
`e_started:(0,0,1)` first does not. The two orders **end in the same state**, which is why the
revision-3 endpoint comparison reported zero conflicts over 93 760 pairs — it was answering the
wrong question. `canonical_reaches_window: false` is the proof that the canonical schedule forecloses
a reachable interleaving. **The canonical reduction is not valid, and revision 3's
"exhaustive partial-order" claim is withdrawn.**

A source-pinned witness is now part of `--self-test`; if it ever disappears, the test fails and asks
for re-justification rather than silently keeping the claim.

### 1.2 `confluence()` compared endpoints — now compares observations

The check applies A→B and B→A and compares:

- the successor key (endpoint), and
- `_observables()` after each prefix: the set of currently claimable entries, worker occupancy, and
  error/report state.

A pair whose prefixes expose different claimability is a counterexample even when the endpoints
agree. That is exactly the case above. The old `pairs_checked`/`conflict=False` column is therefore
not a confluence proof and is no longer printed as one.

### 1.3 Three-channel output closure was ch0-only — fixed

`required_output` for `spatial_three_channel` was `[(0,0)]`, and `required_bands()` seeded the closure
from channel 0 alone, so a public OKAY could lack ch1/ch2 entirely. It is now
`[(0,0,0), (1,0,0), (2,0,0)]` — every channel's final lowpass — and `required_bands()` no longer
forces channel 0.

The regression test is a **closure comparison on a constructed state**, not a search: publish ch0
and ch2, leave ch1's adjacent producer unrun, then compare the per-channel closure against the
retired ch0-only closure. The retired closure reports nothing missing (false OKAY); the per-channel
closure reports the ch1 gap. Withholding ch1 and hoping `I1` fires does **not** work — with the chain
never started no state reports OKAY, so the invariant is vacuously satisfied. That trap is now
documented in the test rather than hit twice.

## 2. Producer edges, pinned to source

| Entry | Reads | Writes | Source |
| --- | --- | --- | --- |
| spatial `index=2` | plane 2, bands 0–3 | plane 1, band 0 | `decoder.c:13045`, `:13051` |
| spatial `index=1` | plane 1, bands 0–3 | plane 0, band 0 | same |
| spatial `index=3` | plane 3, bands 0–3 | plane 2, band 1 | `decoder.c:13173` |
| field-plus `index>3` | plane `index`, bands 0–3 | plane `index-1`, or `index-2` for `index==4`, band 0 | `decoder.c:13064`, `:13112` |
| field-plus `index=3` | plane 3, bands 0–3 | plane 2, band 1 | `decoder.c:13173` |
| field-plus `index=2` | plane 2, bands 0–1 | planes 0 **and** 1, band 0 | `decoder.c:13253-13254` |

`TransformInverseSpatialQuantLowpass` reads all four bands (`wavelet.c:5743-5746`, `:5763-5766`), so
a `0xE` mask is insufficient: band 0 of a non-top plane comes from the adjacent transform. The
temporal inverse has no guard at all — only `assert(BANDS_ALL_VALID(temporal))`
(`decoder.c:13247`), compiled out in Release.

`CODEC_NUM_SUBBANDS_HALF == 11` (`codec.h:648`) makes FULL/HALF structural: HALF decodes subbands
0–10, so a group has planes 5/4/3 plus temporal 2 and the frame planes are filled by the temporal
inverse plus entropy. Under `TRANSFORM_TYPE_FIELDPLUS` there is **no** `index == 1` branch at either
resolution.

## 3. Proposed contract

`QUEUED → CLAIMED → ADMIT → EXECUTING → COMPUTED → PUBLISHED | FAILED`, plus `DROPPED`.

- **R1** claim gated on the published read set for the current generation.
- **R2** ownership token; a second live producer of the same `(channel, index, band, generation)` is
  refused (`I7_SINGLE_OWNER`).
- **R3** admission re-checks generation; failure is terminal.
- **R4** execution fails closed on a missing read band; the silent-return path does not exist.
- **R5** publication is its own transition and re-checks generation, so an allocation swap between
  compute and publish is terminal rather than a write into a stale buffer.
- **R6** drain verifies; incomplete → first terminal error, no OKAY.
- **R7** no wait/spin inside `ReconstructWaveletBand` (shared with the drain loop, pool capped at 4,
  `decoder.c:1596`).
- **R8** the hollow `AllTransformBandsValid` (non-NULL only, `decoder.c:2050-2076`) is replaced by the
  required-set closure at its live sites.

## 4. Results, with their exact scope

All sweeps use the **true partial order** and a 60 000-state cap unless stated. `I4_UNPROVEN` in a
violation list is a truncation marker, not a safety defect.

| Topology | states | exhaustive | proposed safety | false-OKAY rel / leg / prop |
| --- | --- | --- | --- | --- |
| `spatial_chain` | 76 440 | no | clean (prefix) | 7 132 / 25 194 / **0** |
| `spatial_highpass` | 176 | **yes** | clean | 404 / 90 / **0** |
| `fieldplus_half` | 83 507 | no | clean (prefix) | 5 248 / 15 800 / **0** |
| `fieldplus_full` | 105 130 | no | clean (prefix) | 2 722 / 7 943 / **0** |
| `spatial_three_channel` | 355 198 | no | clean (prefix) | 898 / 5 426 / **0** |

**What this does and does not establish.** On every explored prefix the proposed rule produced no
safety violation, and both controls (`released`, `legacy`) still produced false OKAY in every
topology. That is a bounded prefix result, **not** a completed proof: only `spatial_highpass`
finished. The earlier claims of "480", "70", "422" and "446" exhaustive states were canonical-prefix
counts and are withdrawn — the true spaces are two to three orders of magnitude larger, and
`fieldplus_full` exceeds memory at 4 M states.

### Liveness

Under `drain=quiescent`, reverse-reachability to an OKAY terminal:

| Topology | OKAY terminals | stranded | scope |
| --- | --- | --- | --- |
| `spatial_chain` | 1 | 0 | exhaustive (64 342 states) |
| `spatial_highpass` | 1 | 0 | exhaustive (89 states) |
| others | — | — | **not established** (`INCONCLUSIVE`) |

### Negative controls

| Case | Result |
| --- | --- |
| spatial index 1, `0xE` satisfied, index-2 band 0 withheld, proposed | 0 claims |
| same state, legacy | 62 claim states |
| temporal index 2, all producers withheld, proposed | 0 claims |
| same state, legacy | 1 008 claim states |
| stale generation after claim | never OKAY |
| duplicate writer of a published band | `I7` never fires |
| four workers, unsatisfiable entries | 0 OKAY |
| out-of-order `started` reachable before earliest band | **yes** (59049 states) |
| three-channel ch1 output gap detected by the closure | **yes** |

## 5. What remains unproven

- **No completed partial-order proof** for `spatial_chain`, `fieldplus_half`, `fieldplus_full` or
  `spatial_three_channel`. Bounded prefixes only.
- **No symbolic confluence argument.** The reduction is *disproved*, not replaced by a smaller valid
  one. A future reduction would need its own justification against worker-interleaved observations.
- **No memory-ordering model.** The happens-before argument is a design statement.
- **No chroma geometry**, no real arithmetic, no timing. `I2_NO_DOUBLE` is enforced but unreachable
  because no policy permits a second claim.

## 6. Source anchoring

`--check` pins the lowpass call site and both publications, the `index > 4` / `== 4` mapping, the
highpass publication, both temporal publications and the assert-only temporal guard, all four silent
returns, both subband tables, `CODEC_NUM_SUBBANDS_HALF` / `DECODED_SUBBAND_MASK_HALF` / the
`CanSkipChannel` HALF branch, the released `BANDS_ALL_VALID` and `BANDS_ALL_STARTED` forms, the
queue/drain anchors and the hollow `AllTransformBandsValid`. **`anchor: OK`, zero problems.**

## 7. Next action

The three findings are addressed and two of them changed the verdict from "proven" to "bounded".
That correction is the deliverable: a model that had claimed exhaustive partial-order safety now
reports `INCONCLUSIVE` where it cannot prove the claim.

Review of this revision is the next step. A bounded private scratch-SDK intervention stays blocked
until someone accepts this model at its stated, smaller scope — or until a valid reduction or a
larger exhaustive run replaces the `INCONCLUSIVE` entries. No production `upstream/` patch, no
spin/poll fix, no merge, no release. `#120` remains
`REFERENCE_DECODER_NONDETERMINISTIC / UNRESOLVED`.

## 8. Files

- `tools/rca195_m2_model.py` — revision 4; `--check`, `--json`, `--self-test`, `--canonical-witness`,
  `--confluence`, `--negative-cases`.
- `docs/validation/rca195_m2_model_2026-10-20.json` — bounded per-topology results, the canonical
  witness, and the negative controls.
- `docs/validation/RCA195_BAND0_CONTRACT.md` — Milestone-1 source map.
