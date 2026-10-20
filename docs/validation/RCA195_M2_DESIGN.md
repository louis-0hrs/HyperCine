# RCA-195 Milestone 2 — liveness-safe band-0 publication contract (DESIGN)

**Status: `DESIGN_ONLY`.** No decoder patch, no `upstream/` edit, no merge, no
release. This document is the answer to the Milestone-1 acceptance review that
asked for the design *before* any implementation.

The design is accompanied by an executable model,
`tools/rca195_m2_model.py`, which is not a measurement. It encodes the released
queue discipline and the proposed one over one interpreter and searches the
interleaving space for counterexamples to five invariants. `upstream/` remains
frozen and is read only by the anchors cited here.

## 0. What Milestone 1 established, and what it did not

| | |
|---|---|
| Established | The released API can return `CFHD_ERROR_OKAY` with a differing frame while a required transform was never executed. 120/120 induced calls across four arms; **2/30** with an entry that was never even claimed. |
| Not established | Natural frequency, the cause of any particular spontaneous corruption, behaviour outside HALF/1080p/YUV422/Linux-gcc, or a remedy. |

Milestone 2 is therefore a **design**, not a rate measurement and not a patch.

## 1. The defect, stated as a state machine

The current queue has **two states** and one of them is a lie:

| Field | Written where | Means |
| --- | --- | --- |
| `queue[].done = 1` | `entropy_threading.c:271` | "somebody took this entry" — *not* "it ran" |
| `num_entries--` | `:273` | the entry left the queue |
| `next_entry++` | `:272` | pointer bookkeeping |
| `band_valid_flags \|= mask` | `decoder.c:24098` under `entropy_worker_new.lock` | the band's coefficients exist |
| `band_started_flags \|= mask` | `decoder.c:24127`, **lock compiled out** (`_DELAYED_THREAD_START==1`) | "an entropy decoder intends to produce this band" |

An entry is **claimable** when `band_valid_flags == band_started_flags`
(`image.h:230`). That comparison is satisfied by a band that is in *neither*
word, so `0xE == 0xE` admits a wavelet whose band 0 does not exist. After the
claim the worker unlocks and re-tests the *same* predicate (`:285`); if a
producer raised a `started` bit in between, the re-test fails, `wavelet` is set
to `NULL`, and the entry is **neither executed nor requeued nor reported**.
Then `WaitForTransformThread` (`decoder.c:24283`) waits for the pool and zeroes
`started`, `num_entries`, `next_entry`, `free_entry` with no completeness check
and no `decoder->error` assignment.

Two distinct failure shapes follow, and the v3 ledger saw both:

- **claimed-but-not-executed** — the entry was consumed from the queue and the
  re-check skipped it. 118/120 induced calls.
- **never-admitted** — `started != valid` for the whole call, so the entry was
  never claimable and the drain discarded it. 2/30 in arm B.

## 2. Proposed state machine

One entry, five states, one owner at a time. `generation` is the wavelet
allocation generation the entry was queued against.

```
                 (claimable)
   QUEUED ──────────────────► CLAIMED
      │                          │
      │                          │  (generation unchanged)
      │                          ▼
      │                      EXECUTING
      │                          │
      │              ┌───────────┴───────────┐
      │              ▼                       ▼
      │          PUBLISHED                FAILED
      │        (band 0 written)      (terminal error)
      │                                      │
      └──────────────► CANCELLED ◄───────────┘
          (superseded generation)     (stale or aborted)
```

Rules, each of which maps to one of the reviewer's five items:

**R1 — claim is gated on the producer relation, not on two flag words.**
An entry is claimable when the *required input bands* of its own wavelet are
`PUBLISHED` for the entry's generation. The required set is re-derived from
`DecodedBandsValid` (`decoder.c:24136`), which already encodes it per topology:

| Transform | index | required input mask | rationale (source) |
| --- | --- | --- | --- |
| field-plus | 2 | `0x0` | temporal; both bands produced by earlier entries |
| field-plus | 3, 5 | `0xF` | end of a chain; all four bands required |
| field-plus | 0, 1, 4 | `0xE` | band 0 produced by an earlier entry |
| spatial | 2 | `0xF` | top of pyramid |
| spatial | 0, 1 | `0xE` | band 0 produced by an earlier entry |

These are **input** masks. `DecodedBandsValid`'s `threaded_band_mask` names the
band that another entry is expected to produce, which is why it is excluded from
the requirement rather than added to it. A blanket `0xF` is wrong for exactly
this reason: it deadlocks `spatial index == 1`, because entropy never delivers
that band.

**R2 — the claim transfers ownership; the queue is not mutated past it.**
`done` becomes an ownership token with three values (`FREE`, `OWNED`,
`CONSUMED`), and the drain loop's `num_entries` bookkeeping stops being the only
record. This removes the `next_entry`/`free_entry` aliasing that made the v3
probe's `admitted`/`unadmitted` derivations non-obvious.

**R3 — the post-unlock re-check is a generation check, not a predicate re-run.**
Re-running `valid == started` after unlocking is what turns a transient producer
state into a silent skip. The proposed rule re-checks only that the wavelet
allocation has not been superseded (see R6). A failed generation check is
**terminal**, not silent: it sets `decoder->error = CODEC_ERROR_BAD_FRAME` and
marks the entry `FAILED`.

**R4 — the drain verifies and propagates.**
`WaitForTransformThread` may only discard the queue when every entry is in a
terminal state. Otherwise it sets the first terminal error and leaves the public
call to fail. There is no path in which it returns `OKAY` with a live entry.

**R5 — forward progress.**
The drain loop and `ReconstructWaveletBand` run in *one* worker function
(`EntropyWorkerThreadProc`), and the entropy pool is capped at 4 threads
(`decoder.c:1596`). A worker that waits for a band inside `ReconstructWaveletBand`
removes capacity to publish it. Therefore:

- no wait, spin or sleep inside `ReconstructWaveletBand`;
- an entry whose inputs are missing is left `QUEUED`, never occupied by a
  waiting worker;
- when a worker finds nothing claimable it **returns**, so the pool is never
  fully parked on unsatisfiable entries;
- the caller-side drain fails closed instead of waiting for a state that cannot
  arrive.

Memory ordering: `band_valid_flags` and the ownership token are written under
`entropy_worker_new.lock` and read under the same lock. `band_started_flags`
today is written with the lock compiled out; under the proposal it is either
folded into the same lock or removed from the decision, because no correctness
argument may depend on a word written without synchronisation.

**R6 — cancellation and generation.**
`transform->wavelet[index]` is reallocated across calls
(`ReallocWaveletEx` at `decoder.c:11983`, `12022`, `12064`), so a queued entry
can outlive the allocation it was queued against. Each queue entry records the
generation it was queued under; a mismatch is `CANCELLED` with a terminal error
if the band was still required, and `CANCELLED` silently only if the entry is
provably redundant. Buffer reuse is therefore no longer a source of silent
staleness.

**R7 — output boundary.**
`AllTransformBandsValid` (`decoder.c:2050`) is compiled unconditionally
(`#if DEBUG || 1`) and checks only `wavelet != NULL`; its band test is commented
out. The one helper that inspects band 0, `AllLowpassBandsValid` (`:2078`), sits
behind `#if DEBUG`, which no build file in this tree enables. The proposal
replaces the hollow call at the seven live sites with a check on the same
per-topology required set used by R1, in release configurations.

## 3. What the model proves, and what it does not

`tools/rca195_m2_model.py` implements both policies over a small interpreter and
enumerates interleavings of {claim, execute, raise-started, publish, drain}.

Three scenarios, run under each policy:

| Scenario | Shape | released | proposed |
| --- | --- | --- | --- |
| `claim_skip` | entry claimable, then a producer raises `started` before the re-check | `I1`, `I3` | clean, **exhaustive** (922 interleavings) |
| `never_admitted` | producer raises `started`, never publishes `valid` | `I1`, `I3` | clean, **exhaustive** (922) |
| `chain` | two dependent entries | `I1`, `I3` | clean, **TRUNCATED at 20 000** |

Invariants checked: `I1_FAIL_CLOSED`, `I2_NO_DOUBLE`, `I3_NO_SILENT`,
`I4_BOUNDED`, `I5_PROGRESS`.

**Read the third row honestly: `chain` was cut off at the search limit.** An
empty violation list there is an absence in the explored subspace, not a proof
over all interleavings. The model records `search_truncated` per scenario so the
two cannot be confused, and the self-test fails if a scenario that should be
exhaustive is truncated or if a violation is reported.

`I2_NO_DOUBLE` is *not* reproduced by the released policy in the model, because
the frozen code cannot execute one entry twice through this path. The TSan
finding that two threads enter `TransformInverseSpatialQuantLowpass` on one
allocation is a separate observation about `wavelet[index-1]` vs `wavelet[index]`,
not a duplicate queue execution, and the model does not claim it.

## 4. Deterministic test cases Milestone 2 requires

Each is separately testable and deterministic; none needs a race to hit.

| Case | Construction | Expected |
| --- | --- | --- |
| missing producer | withhold the `valid` publication for one `(channel, index)` band | terminal error, non-OKAY public return |
| claimed-but-not-executed | consume an entry, skip its execution | terminal error, not `OKAY` |
| duplicate execution | present the same entry twice | second claim refused; no double write |
| stale generation | reallocate the wavelet with a queued entry pending | entry `CANCELLED`, terminal error if required |
| pool saturation | 4 workers, all on unsatisfiable entries | drain fails closed within the step bound |
| positive control | unmodified decode | `OKAY`, digest equals the modal control |
| FULL/HALF matrix | both resolutions, all three channels | required-bit sets as in §2 R1 |

The existing v3 probe already provides the first two shapes as *injections*; the
work in Milestone 2 is to assert the **error** instead of the false OKAY.

## 5. Explicitly not in this design

- No production busy-wait, no unbounded spin, no timeout-based "detect".
- No change to `upstream/`. A candidate implementation, if approved, is a
  private test-only copy plus a separately reviewed wrapper — the same shape as
  the Milestone-1 probe.
- No claim about FULL, Windows, other GOP structures or other qualities. The
  model's required-bit matrix is derived from source; it is not a measurement.
- No statement about the natural frequency of the defect. `#120` stays
  `REFERENCE_DECODER_NONDETERMINISTIC / UNRESOLVED`.
- Nothing here implicates our separately implemented CUDA decoder (#187).

## 6. Files

- `tools/rca195_m2_model.py` — the executable design model, `--check`,
  `--json`, `--self-test`.
- `docs/validation/rca195_m2_model_2026-10-20.json` — machine-readable
  interleaving results, including which searches were truncated.
- `docs/validation/RCA195_BAND0_CONTRACT.md` — Milestone-1 source map (unchanged
  by this document).

## 7. Next action

Review this state machine and its required-bit matrix. If the matrix is
accepted, the first implementation step is a **private test-only** patch that
implements R1+R3+R4 only (claim gating, generation re-check, verifying drain) —
enough to convert the two observed false-OKAY shapes into terminal errors, and
nothing else. R5/R6/R7 follow only after that converts the injected cases.
