# RCA #159 — thread oversubscription, not "1 logical processor"

**Scope:** Linux CPU-only, pristine frozen upstream SDK (`upstream/` untouched).
**Status of this document:** measurement and method correction. No production change, no merge, no release.
**Evidence:** [`docs/validation/rca159_thread_oversubscription_2026-10-18.json`](validation/rca159_thread_oversubscription_2026-10-18.json).

## Why this exists

Issue #159 asks for an independent Linux reproduction of the successful-but-corrupt
decode, plus bounded sanitizer evidence, and it explicitly warns not to describe a
Linux build as identical to the Windows binary. Earlier passes on this lane reported
divergence rates at "1/2/4/8 logical processors", using an affinity tool to pin the
process. Those arms did **not** limit the decoder. This document records the
correction and the measurements that follow from it.

## The method defect

The frozen SDK sizes its worker pools from a CPU count that ignores an affinity mask:

```c
int GetProcessorCount() { return sysconf(_SC_NPROCESSORS_ONLN); }
```

`taskset -c 0` therefore constrains *where* threads may run, but not *how many* the
decoder creates. Measured on the pinned packet: a process pinned to a single CPU
still reports **37 threads** and runs 36 decode worker threads. An arm labelled
"1 logical processor" was in fact a 36-worker decode forced onto one core.

The only knob that actually caps the decoder is the `TAG_CPU_MAX` metadata tag, which
flows into `DecodeEntropyInit()` and clamps the pool size. Measured mapping:

| `TAG_CPU_MAX` | decoder threads | worker threads |
| --- | --- | --- |
| unset | 37 | 36 |
| 1 | 2 | 1 |
| 2 | 5 | 4 |
| 4 | 9 | 8 |
| 8 | 13 | 12 |
| 16 | 21 | 20 |

Earlier passes that varied only affinity were, in effect, varying **only the number of
CPUs available to an unchanged 36-thread decode**. That is a different experiment from
the one they were described as running, and it is the experiment that turns out to
matter.

## What was measured

Two quantities are varied independently and both are recorded per call:

- `affinity_cpus` — CPUs the process may run on (`sched_getaffinity`)
- `worker_threads` — threads the process actually had during the decode
  (`/proc/self/status`, `Threads`, read after the decode returns)

Every cell runs fresh processes with **one decode call each**, so no state from a
previous frame can be involved. This matters: it rules out any carry-over, reuse or
generation mechanism as a prerequisite.

Probe: `tools/hypercine_decoder_thread_probe.cpp`.
Sweep: `ci/rca159_thread_sweep.sh`.
Aggregation: `ci/build_rca159_evidence.py`.

### Result — repository probe, 12 cells, 240 calls

| cell | worker threads | CPUs | oversubscribed | divergent | rate |
| --- | --- | --- | --- | --- | --- |
| `wide_cm0` | 36 | 48 | false | 0/20 | 0.0% |
| `wide_cm1` | 1 | 48 | false | 0/20 | 0.0% |
| `wide_cm2` | 4 | 48 | false | 0/20 | 0.0% |
| `wide_cm4` | 8 | 48 | false | 0/20 | 0.0% |
| `wide_cm8` | 12 | 48 | false | 0/20 | 0.0% |
| `wide_cm16` | 20 | 48 | false | 0/20 | 0.0% |
| `cm4_aff0` | 8 | 1 | **true** | 1/20 | 5.0% |
| `cm4_aff0-1` | 8 | 2 | **true** | 2/20 | 10.0% |
| `cm4_aff0-3` | 8 | 4 | **true** | 2/20 | 10.0% |
| `cm4_aff0-7` | 8 | 8 | false | 0/20 | 0.0% |
| `cm4_aff0-15` | 8 | 16 | false | 0/20 | 0.0% |
| `cm4_aff0-47` | 8 | 48 | false | 0/20 | 0.0% |

Every cell decodes to the same modal frame
`12ed7b2b8d1b06d87fa693db6ac72474ece13632c2d8ffe6981a52f52e1480fd`.
That digest is the image plus 64 sentinel bytes, not the image alone — see
[Hash domain](#hash-domain).
All 240 calls return `CFHD_ERROR_OKAY` (code 0) at 1920×1080, pitch 7680 — including
the divergent ones.

### Combined across all runs

Four independent sweep series, 36 cells, 840 calls:

| group | cells | calls | divergent | rate |
| --- | --- | --- | --- | --- |
| `worker_threads > affinity_cpus` | 14 | 340 | 36 | **10.59%** |
| `worker_threads <= affinity_cpus` | 22 | 500 | 0 | **0.00%** |

Fisher exact, two-sided: **p ≈ 2.3 × 10⁻¹⁵**. There is no cell in either direction that
contradicts the split, and all 36 cells share one modal digest.

> **Corrected by the follow-up pass below.** The two-group split above is supported,
> but the claim that the boundary is *sharp at ratio 1.0* was not. That grid had no
> cell between ratio 1.0 and ratio 2.0, so the transition was never sampled. It is a
> gradient, not a step — see [Ratio sweep](#ratio-sweep--where-the-transition-actually-is).

## Ratio sweep — where the transition actually is

**Evidence:** [`docs/validation/rca159_ratio_boundary_2026-10-18.json`](validation/rca159_ratio_boundary_2026-10-18.json).
**Sweeps:** `ci/rca159_ratio_sweep.sh`, `ci/rca159_interleaved.sh`,
`ci/rca159_smt_control.sh`, `ci/rca159_cliff.sh`.
**Builder:** `ci/build_rca159_ratio_evidence.py`.

95 cells, **6 060 decode calls**, one fresh process per call, same pinned packet and
the same pristine SDK revision as above. All 6 060 calls return `CFHD_ERROR_OKAY`
(code 0) at 1920×1080 / pitch 7680 and share **one** modal digest,
`12ed7b2b…480fd` (image + 64 sentinel bytes `0xCD`; the image-only digest is
different — see [Hash domain](#hash-domain)); only the divergent minority differs.

### The previously unmeasured band is clean

`1 < workers/cpus < 2` was the gap in the coarse grid. Measured directly now:

| band | cells | calls | divergent | rate |
| --- | --- | --- | --- | --- |
| `ratio ≤ 1.0` | 29 | 1 220 | 0 | **0.00%** |
| `1 < ratio < 2` | 23 | 1 220 | 1 | **0.08%** |
| `ratio = 2.0` | 15 | 1 410 | 18 | **1.28%** |
| `ratio ≥ 4` | 18 | 1 300 | 194 | **14.92%** |

So oversubscription is still necessary — nothing diverged at or below ratio 1.0 in
1 220 calls — but a *mild* oversubscription is nearly harmless. The step in the data
is between ratio 2 and ratio 4, **not** at 1.0. Stated as a boundary "exactly where
the ratio crosses 1.0", the earlier wording overstated the sharpness by two whole
ratio units.

### Absolute CPU count, not just the ratio

Grouped by the number of CPUs the process may run on, the same data looks like this:

| affinity CPUs | cells | calls | divergent | rate |
| --- | --- | --- | --- | --- |
| 1 | 8 | 510 | 78 | **15.29%** |
| 2 | 10 | 710 | 94 | **13.24%** |
| 3 | 9 | 540 | 54 | **10.00%** |
| 4 | 12 | 1060 | 32 | **3.02%** |
| 5 | 7 | 430 | 1 | **0.23%** |
| 6 | 11 | 730 | 1 | **0.14%** |
| 7 | 4 | 220 | 0 | **0.00%** |
| 8–16 | 34 | 1860 | 0 | **0.00%** |

`cpus ≤ 4` → 262/3 020 (**8.68%**); `cpus ≥ 5` → 2/3 040 (**0.07%**),
Fisher two-sided **p ≈ 9.5 × 10⁻¹⁹**. At ratio 2.0 specifically, 12 workers on
6 CPUs gave **0/290** while 8 workers on 4 CPUs produced divergent frames — same
ratio, different absolute width, different outcome.

This does **not** establish "absolute CPU count instead of ratio". A dedicated paired
run (4 rounds × 50 calls per arm, interleaved) of the cleanest case — 12 workers on
6 CPUs versus 8 workers on 4 CPUs, both ratio 2.0 — gave **0/200 vs 4/200**,
Fisher **p = 0.12**. That is a suggestive direction, not a result. What the data does
support is that ratio alone is an incomplete predictor: at equal ratio, more
available CPUs never diverged in these runs.

### Controls that remove two alternative explanations

1. **General CPU contention is not the trigger.** 8 busy-loop processes pinned one
   per allowed CPU, against 8 workers on 8 CPUs: **0/40 divergent**, identical to the
   quiet run of the same cell (0/40). Saturation from *outside* the decoder produced
   nothing. The trigger is the decoder's own thread-to-CPU oversubscription.
2. **SMT placement is not the confound.** Every arm here counts logical processors,
   and on this host the sibling pairs are `0-1, 2-3, 4-5, …`, so `taskset -c 0-3`
   grants two physical cores, not four. Holding the *reported* logical count equal:
   8 workers on 8 logical CPUs gave 0/120 whether those were 8 physical cores
   (`0,2,4,…,14`) or 4 cores with both siblings (`0-7`); 4 workers on 4 logical CPUs
   likewise (0/120 vs 2/120); and 8 workers on 2 logical CPUs gave 19.2% (1 core with
   SMT) versus 15.8% (2 physical cores). The logical count is the quantity that
   tracks the rate.

### Method correction to this lane's own record

Two cells were measured twice at identical settings and disagreed: 4 workers on
1 CPU gave **5/30** in one sequential series and **0/30** in the next (and 4 workers
on 2 CPUs gave 4/60 versus 0/110). A sequential sweep cannot tell a real boundary
effect from slow host drift, so every arm in the follow-up was run **interleaved by
round** — all cells back to back, several times — before any comparison was drawn.
The `interleaved` and `cliff` sweeps are that design. Single-cell numbers from a
sequential pass should not be quoted on their own.

## Hash domain

`modal_frame_sha256` in both evidence files is the probe digest
`sha(out.data(), out.size())`. The buffer is allocated as `pitch * height + 64`
and prefilled with `0xCD`. Retained BGRA dumps write only `pitch * height`.
For this geometry that is 8,294,400 bytes (1920×1080, pitch 7680), and the
hashed domain is those bytes plus 64 sentinel bytes (8,294,464).

Recomputed 2026-10-23 with `sha256sum` from retained Linux outputs dated
2026-10-18. Two exemplars of the modal class, `rca159-mech/p39/dec_c00.bgra`
and harness `dumps/p1/dec_c00.bgra`, are byte-identical and both exact-size:

| domain | bytes | sha256 |
| --- | ---: | --- |
| image only | 8,294,400 | `c5e0e052be84a807a899d0501fa73f93bbe9d67e0f1c9e5cfdef97a3ffee54a0` |
| image + 64 bytes `0xCD` | 8,294,464 | `12ed7b2b8d1b06d87fa693db6ac72474ece13632c2d8ffe6981a52f52e1480fd` |

The second value is the historical `modal_frame_sha256`. The first is the
image-only digest of the same bytes. Across the retained exact-size BGRA set,
214 of 243 files share this pair; the other 29 are non-modal frames and were
not used as the modal exemplars. This names the byte domain only. It is not a
Windows parity or release claim. The gate stays
`REFERENCE_DECODER_NONDETERMINISTIC / UNRESOLVED`.

## What this means for the campaign

1. **The trigger is CPU oversubscription, not "low logical processor count".** The
   defect reproduces when the decoder creates more runnable threads than there are
   CPUs to run them on. `TAG_CPU_MAX=1` does not "fix" the worker path by removing a
   race window in the usual sense — it removes oversubscription by leaving a single
   worker, which is why it has looked like a fix in every previous pass. The follow-up
   sweep refines this: the risk is negligible up to ratio 2 and rises steeply by
   ratio 4, so "oversubscribed" and "dangerous" are not the same statement.

2. **No cross-frame state is required.** The first decode in a fresh process, with a
   fresh decoder, already diverges. Any hypothesis that needs a previous frame's
   leftover state — wavelet reuse, generation carry-over, queue residue — cannot be
   the cause of this mode.

3. **The rate depends on the ratio, not on the machine.** Oversubscribed cells ranged
   from 2.5% to 20% within the same session. This explains the rate drift reported
   repeatedly on this issue (11.7% → 33% → 54–62%): those arms had different, and
   mostly unrecorded, thread-to-CPU ratios.

4. **Previous arm labels are not comparable.** Any result in this issue described as
   "N logical processors" must be re-read as "N CPUs available to a pool of unknown
   size". Where the pool size was not recorded, the arm cannot be reconstructed.

## What is *not* claimed

- **No causal mechanism is named here.** Oversubscription is established as a
  necessary condition in these runs; which specific interleaving corrupts the frame
  is not established. A race that only manifests under preemption is consistent with
  the data, but consistent is not proven.
- **No sanitizer conclusion is revised.** The previous ASan/UBSan/TSan results stand
  as recorded; they were obtained on arms whose thread counts are now known to have
  been unrecorded.
- **No Windows parity claim.** These are Linux CPU-only measurements on the frozen
  upstream tree. A Linux build reproducing the Windows modal frame on the pinned
  packet (recorded earlier on this issue) does not extend to this failure mode.

## Reproduction

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DHYPERCINE_BUILD_TESTS=OFF \
      -DHYPERCINE_ENABLE_CUDA=OFF
cmake --build build --target hypercine_decoder_thread_probe

HYPERCINE_THREAD_PROBE=build/hypercine_decoder_thread_probe \
HYPERCINE_PACKET="$HYPERCINE_PACKET" \
HYPERCINE_SWEEP_OUT=rca159-sweep \
./ci/rca159_thread_sweep.sh

python3 ci/build_rca159_evidence.py evidence.json rca159-sweep
```

The packet is supplied by the operator and is not part of this repository. The sweep
records `affinity_cpus` and `worker_threads` on every call so a cell can be
interpreted without knowing how it was launched.

**Platform:** the probe target is built only on Linux (`UNIX AND NOT APPLE`). It reads
`sched_getaffinity()` and `/proc/self/status`, so on Windows or macOS it would have to
report `-1` for the two quantities it exists to measure. Building it there would
produce numbers that look like data and are not, so the target is restricted rather
than stubbed. The previous head of this branch built it unconditionally and the
Windows job failed on `Cannot open include file: 'sched.h'`; that is the reason for
the guard, not a formatting preference.

The follow-up sweeps take the same environment variables:

```sh
./ci/rca159_ratio_sweep.sh   # series a..d, one-step CPU walk at fixed caps
./ci/rca159_interleaved.sh   # every arm per round (drift control)
./ci/rca159_smt_control.sh   # physical cores vs SMT siblings
./ci/rca159_cliff.sh         # fixed cap, CPUs 3..6

python3 ci/build_rca159_ratio_evidence.py evidence.json \
  coarse=<dir> one-step=<dir> interleaved=<dir> smt=<dir> cliff=<dir>
```

## Suggested next step

The ratio walk requested by the previous pass is done, and it replaced the "sharp at
1.0" boundary with a gradient. The remaining questions, in the order the evidence
supports them:

1. **Why does a preempted worker produce a partially reconstructed frame at all?**
   That is the mechanism question this lane keeps deferring, and it is now the only
   one that can turn the rate table into a fix. It needs code-level tracing of the
   inverse stage under a forced preemption, not more rate measurement.
2. **Is the clean `1 < ratio < 2` band clean because of the ratio, or because those
   cells had at least 5 CPUs?** The paired arm was underpowered (p = 0.12). Splitting
   the two variables needs a host with more spare CPUs than this one, or longer runs
   than a bounded evidence budget allows here.
3. **Nothing in this document changes product behaviour.** The gate stays
   `REFERENCE_DECODER_NONDETERMINISTIC / UNRESOLVED` until a mechanism is understood.
