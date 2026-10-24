# RCA 160 — pre-registered A/B on the low-LP decode divergence

**Status: hypotheses NOT supported. Both candidate mechanisms were instrumented, both
were proven capable of firing, and neither fires during the observed divergence.**

Test-only diagnostic. No production code, no `upstream/` change. The repository's
frozen `upstream/` tree is untouched; all instrumentation existed only in private
copies outside the repository, and this document is the only artifact published.

## Basis

[#159](https://github.com/louis-0hrs/HyperCine/issues/159) observed, on a Linux
synthetic 1920x1080 FilmScan3 source (not the pinned Windows packet):

| Condition | Calls | Divergent |
| --- | --- | --- |
| 48 LP, default threading | 45 | 0 |
| 1 LP, default threading | 60 | 4 (6.7%) |

The pre-registration for this run was posted before any code existed:
[#160 comment](https://github.com/louis-0hrs/HyperCine/issues/160#issuecomment-6252535227).

## Instruments

- **I1** — at `WaitForTransformThread`, report transform-queue entries that are about
  to be discarded without execution (`decoder->transform_queue.num_entries != 0`).
- **I2** — restore the strict band-validity predicate that the upstream tree replaced
  with a `valid == started` comparison, and apply it in the release path.

Both were applied only to private scratch copies. Four variants were built from one
pristine snapshot: A (control), B (I1), C (I2), D (I1+I2).

## Result — 1 logical processor

| Arm | Instrumentation | Calls | Unique digests | Divergent | Rate |
| --- | --- | --- | --- | --- | --- |
| A | none (control) | 60 | 10 | 9 | 15.0% |
| B | I1 only | 60 | 6 | 5 | 8.3% |
| C | I2 only | 60 | 7 | 6 | 10.0% |
| D | I1 + I2 | 60 | 6 | 9 | 15.0% |

Negative control, 48 LP: **A 0/45** (1 unique digest), **D 0/45** (1 unique digest).

All 330 calls across all arms returned `CFHD_ERROR_OKAY`, 1920x1080, pitch 7680.
The modal digest is **byte-identical across every arm** — the instruments changed
nothing about the output of good calls.

## Verdict against the pre-registered criteria

| Criterion | Outcome |
| --- | --- |
| 1. Arm A reproduces silent corruption | **met** — 9/60 (15%), all `OKAY` |
| 2. B/C/D eliminate silent corruption | **NOT met** — 5/60, 6/60, 9/60 remain silent-corrupt |
| 3. Modal digest identical across arms | **met** — no behavioural change |
| 4. Instruments silent at 48 LP | **met** — zero fires, modal unchanged |

Arm differences are not statistically distinguishable: A vs B p=0.26, A vs C p=0.41,
A vs D p=1.00, A vs pooled B+C+D p=0.42. No arm eliminates the effect, and none
reduces it beyond noise.

## Why the null result is meaningful, not an instrument defect

Both instruments were proven capable of firing with forced positive controls:

- I1 forced unconditional: emitted `RCA160_I1 residual_entries=0` on stderr, output
  unchanged.
- I2 forced to fail: produced `status=decode_failed, code=2048`, i.e. the error path
  is reachable and reported.

So the observation is "the instrument did not fire", not "the instrument could not
fire". That distinction is what makes this a falsification rather than an inconclusive
run.

## What this falsifies

- **H1 (discarded transform-queue entries) is not the mechanism for these calls.** At
  the point where the queue is reset, `num_entries` was already 0 on every one of the
  180 instrumented calls. No work was being dropped there.
- **H2 (band-validity gate accepting never-announced bands) is not the mechanism for
  these calls.** Enabling the strict predicate in release did not reject any of the
  180 instrumented calls, so every wavelet reaching that check genuinely had all its
  declared bands valid.

Both defects remain real code-quality findings — they are unreachable in *these*
runs, which is a different statement from "they are harmless".

## What remains open

The divergence itself is confirmed and reproducible (9/60 at 1 LP, 0/45 at 48 LP,
contemporaneous control in the same session). It is therefore real, and it is
scheduling- or concurrency-sensitive, but it is not explained by either of the two
mechanisms the source map proposed.

The pre-registered `TAG_CPU_MAX=1` result from #159 (0/150) is **not** a fix: that
setting routes the decoder away from the worker path entirely
(`threading = pool.thread_count > 1 ? threading : 0`), so it removes the symptom by
removing the concurrency.

## Next checks, in priority order

1. Instrument the **worker-pool completion path itself** rather than its callers:
   timestamp per-queue-entry enqueue/claim/complete and compare per-generation
   counts, so a lost or double-consumed entry becomes visible directly.
2. Repeat the same A/B with the **output buffer canary** enabled, to separate
   "frame reconstructed from stale coefficients" from "frame written by a late
   worker after return". The #159 TSan pair at the band flag is consistent with the
   latter and is not covered by either instrument here.
3. Only after 1–2 produce a positive: consider a production-facing issue. Not now.

## Boundaries

- No CUDA, no GPU, no host or VM reconfiguration, no process kills.
- `upstream/` unchanged in the repository; instrumentation confined to private copies.
- No claim about the Windows 4/45 result or about 1080p causality on the pinned
  packet. That arm remains `BLOCKED_MATCHED_INPUT`.
- No production decoder change, no safety-gate downgrade, no merge, no release.
