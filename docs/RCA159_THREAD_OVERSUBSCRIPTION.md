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

The boundary is sharp and sits where the ratio crosses 1.0:

- 8 workers on 8 CPUs → 0/20 and 0/40 divergent across two series
- 8 workers on 4 CPUs → 2/20 and 2/40
- 8 workers on 2 CPUs → 4/20 and 1/40
- 8 workers on 1 CPU → 1/20 and 7/40

## What this means for the campaign

1. **The trigger is CPU oversubscription, not "low logical processor count".** The
   defect reproduces when the decoder creates more runnable threads than there are
   CPUs to run them on. `TAG_CPU_MAX=1` does not "fix" the worker path by removing a
   race window in the usual sense — it removes oversubscription by leaving a single
   worker, which is why it has looked like a fix in every previous pass.

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

## Suggested next step

Pin `TAG_CPU_MAX` so that `worker_threads <= affinity_cpus`, then vary one thing:
the number of CPUs. If the rate tracks the ratio through 1.0 and collapses above it,
oversubscription is confirmed as the trigger for the prefix mode independently of
thread-pool internals — and the remaining question becomes why a preempted worker
produces a *partially reconstructed frame* rather than no frame at all.
