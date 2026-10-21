# RCA-195 Milestone 2 — liveness-safe band-0 publication contract (DESIGN, rev 6)

**Status: `DESIGN_ONLY` / `REDUCED_ORDER_QUOTIENT_PROVEN` / test-verdict taxonomy closed.** No
decoder patch, no `upstream/` edit, no merge, no release.

Revision 6 answers the fifth independent review, which found one structural defect in the *reporting
path* rather than in the model: `--json` called `negative_cases()` and discarded its failure list, so
a broken stale-generation or starvation control could fail while the same run printed `MODEL PASS`
and exited 0. That review also repeated the earlier request for a verdict taxonomy and for at least
one genuinely exhaustive stale control. All three are now closed.

Revision 5 answered the fourth independent review, which accepted rev 4 as
`REV4_REVIEW_POSITIVE_FOR_HONEST_BOUND / FULL_SCHEDULER_SAFETY_PENDING` and named exactly two
remaining engineering tasks: a **sound, observation-preserving reduction**, or a compositional
per-`(channel,index,generation)` proof with explicit cross-channel worker interaction. Both were
delivered, and each came with the proof that makes it more than a smaller space.

## 0. Revisions

| Rev | Disposition | What it established |
| --- | --- | --- |
| 1 | `MODEL_INCOMPLETE` | — (own-input-plane publication, free publications, atomic claim/execute) |
| 2 | `SOURCE_MODEL_REV2_IMPROVED` | real producer edges, correct output plane, scheduled transitions |
| 3 | `REV3_STRUCTURAL_PROGRESS / ..._BLOCKED` | worker identity, COMPUTED/PUBLISHED split, field-plus HALF/FULL, three channels — but the "partial order" was a canonical prefix |
| 4 | `REV4_REVIEW_POSITIVE_FOR_HONEST_BOUND` | true per-band order, canonical reduction **disproved**, bounded prefixes labelled `INCONCLUSIVE` |
| 5 | rev 5 | a **proven** reduced order (exact quotient) and a **compositional** three-channel proof |
| 6 | this document | a single verdict function, three named states, and a **proven** stale control |

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

## 5. Negative controls: three verdicts, and every control has a premise

Revision 5 published a control row as `ok` when its search had been **truncated**, on the basis
that no violation had been found in the prefix. The fifth review named this correctly: a prefix that
contains no counterexample is not evidence for the property, and it is certainly not a
`NEGATIVE_TEST_PASS`. Two further defects came with it, and all three are now closed.

**A single verdict function.** `model_verdict()` is the only place that decides pass/fail. It
receives the control failures and inconclusive list explicitly, and `--json` and
`--negative-cases` both read it, so the two paths cannot disagree. Previously `main()` computed the
verdict from `safe and controls and disproved` and dropped `_failures` on the floor, which meant a
broken control still printed `MODEL PASS` and exited 0.

| Verdict | Meaning | Exit |
| --- | --- | --- |
| `PASS_EXHAUSTIVE` | premise constructed, property held on **every** reachable state | 0 |
| `PREFIX_ONLY_INCONCLUSIVE` | the search hit its cap; the control decided nothing | 3 |
| `FAIL` | the premise held and the property did not | 2 |

**Every control now states its premise.** An `ok` row used to mean "no invariant fired", which a
run that never reached the interesting state also satisfies. Each control therefore reports what it
actually constructed: the stale controls must show at least one `stale-generation` or
`stale-before-publication` terminal **and** at least one OKAY terminal; the starvation control must
show that a drain was reachable and carried an `incomplete:` error. Without those the control proves
nothing, and the model says so instead of counting the silence as a pass.

| Case | Verdict | Premise it constructed | Measured |
| --- | --- | --- | --- |
| spatial index 1, `0xE` satisfied, index-2 band 0 withheld, `proposed` | `PASS_EXHAUSTIVE` | 38 states | 0 claims |
| same state, `legacy` | `PASS_EXHAUSTIVE` | 126 states | 62 claim states |
| temporal index 2, all producers withheld, `proposed` | `PASS_EXHAUSTIVE` | 38 states | 0 claims |
| same state, `legacy` | `PASS_EXHAUSTIVE` | 1 664 states | 1 008 claim states |
| **stale generation, single entry** (`spatial_highpass`, true partial order) | `PASS_EXHAUSTIVE` | 366 states, 2 stale terminals + 1 OKAY | fails closed |
| stale generation, chain (**reduced** order) | `PASS_EXHAUSTIVE` | 13 529 states, 2 `stale-generation` + 2 `stale-before-publication` + 1 OKAY | fails closed |
| stale generation, chain (**partial** order) | `PREFIX_ONLY_INCONCLUSIVE` | 411 841 states vs 120 k cap | not counted |
| duplicate writer of a published band | `PASS_EXHAUSTIVE` | 128 682 states | `I7` never fires |
| four workers, unsatisfiable entries | `PASS_EXHAUSTIVE` | 39 366 states, 19 683 drains, all `incomplete:` | 0 OKAY |
| later band's `started` before the earliest band's | `PASS_EXHAUSTIVE` | 59 049 states | reachable |
| per-channel output closure detects missing ch1 | `PASS_EXHAUSTIVE` | constructed state | detects it |
| `reduced` against `released` | `PASS_EXHAUSTIVE` | constructor | `ValueError` |

**The exhaustive stale control the review asked for.** `spatial_highpass` has exactly one transform
entry, so the claim→stale→execute window is decidable on **366 states in milliseconds** under the
true partial order — no cap, no reduction, no argument. It is not vacuous: the run reaches both
stale mechanisms *and* one OKAY terminal, so it proves "a stale entry fails closed" rather than
"nothing ever happened".

The chain cannot be exhausted on the partial order (411 841 states against the cap, measured, not
guessed), so that control is reported as the bound it is and the **reduced** run carries the proof —
which is legitimate only because `started_is_unobservable()` and `reduction_check()` license the
reduction independently.

**A regression for the reporting path.** `--verdict-regression` builds a report that is green in
every other respect, injects one control failure, one truncation, one safety violation and one
control that stopped firing, and asserts the exit code of each. It needs no BFS, so it sits in the
300 s CTest budget next to `--check`. Mutation-tested: restoring rev 5's `negative_controls: True`
makes it fail with `exit 0/verdict PASS (expected 2/FAIL)` on exactly the case the reviewer
described.

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

Rev 5's two tasks remain delivered: an observation-preserving reduction with a proof that it is
exact, and a compositional three-channel argument with explicit worker interaction. Rev 6 closes
the fifth review's reporting defect and the verdict-taxonomy request that came with rev 4.

The gate the fourth review set is unchanged and is **not** claimed here: after the verdict fix, the
Linux owner may prepare **one** preregistered, narrow private scratch-copy SDK experiment (R1 read-set
admission, R3/R5 generation re-check, R4 missing-input `CODEC_ERROR_BAD_FRAME` propagation, R6
drain verification; forced missing producer and claimed-without-execution must be non-OKAY, the
complete control must succeed without hanging, at 2 and 4 workers). That is not prepared by this
commit and not authorised by it.

No production `upstream/` patch, no spin/poll fix, no merge, no release. `#120` remains
`REFERENCE_DECODER_NONDETERMINISTIC / UNRESOLVED`.

## 9. Files

- `tools/rca195_m2_model.py` — revision 6; `--check`, `--json`, `--self-test`,
  `--verdict-regression`, `--canonical-witness`, `--reduction`, `--confluence`, `--negative-cases`.
  `model_verdict()` is the single pass predicate for both report paths.
- `docs/validation/rca195_m2_model_2026-10-20.json` — per-topology partial and reduced results,
  quotient equality, unobservability probes, compositional proof, per-control verdicts with their
  premises, and `negative_case_verdict`.
- `docs/validation/RCA195_BAND0_CONTRACT.md` — Milestone-1 source map.
