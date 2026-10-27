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

## Evidence tiers

This document separates three claims that are easy to read into each other, and
each section below is labelled with the tier it belongs to:

| Tier | Meaning | Where |
| --- | --- | --- |
| **OBSERVED** | directly measured on the recorded hardware and tools, with run counts | [What was measured](#what-was-measured-observed), [Ratio sweep](#ratio-sweep--where-the-transition-actually-is-observed-and-correlated), [Hash domain](#hash-domain-observed) |
| **CORRELATED** | a statistical association with the test that supports it, stated as association only | [Ratio sweep](#ratio-sweep--where-the-transition-actually-is-observed-and-correlated) (Fisher tests) |
| **PROVEN CAUSE** | a mechanism shown to produce the defect | none — see [What is *not* claimed](#what-is-not-claimed) |

Nothing in the third tier exists for this defect. The gate stays
`REFERENCE_DECODER_NONDETERMINISTIC / UNRESOLVED`.

## What was measured (OBSERVED)

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

## Ratio sweep — where the transition actually is (OBSERVED and CORRELATED)

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

## Hash domain (OBSERVED)

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

### PROVEN CAUSE: empty

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

Exit 0 means those five labels were all supplied, each directory existed, and
each contributed at least one cell. A missing directory is recorded in
`sweeps_missing` and the process exits 3 (or 1 when no cell was read). It does
not skip the gap and still report success. `environment.logical_cpus` is
`os.cpu_count()` of the builder process, labelled as the builder host. Sweep
records do not contain the measurement host's logical CPU count; a previous
revision stamped the integer 48 anyway.

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

## Mechanism — why a preempted worker returns a prefix

**Evidence:** [`docs/validation/rca159_prefix_mechanism_2026-10-24.json`](validation/rca159_prefix_mechanism_2026-10-24.json).
No new rate sweep. The four retained divergent dumps and the one retained modal dump from the earlier oversubscribed run were re-measured. `upstream/` was not modified. No decoder fix was applied. The gate stays `REFERENCE_DECODER_NONDETERMINISTIC / UNRESOLVED`.

### Packet and the path that is actually taken

The pinned packet is an intra frame (`SAMPLE` = 9), spatial transform (`TRANSFORM_TYPE` = 0), 3 channels, 3 wavelets, 10 subbands, 2 spatial levels, 1920×1080, precision 10, encoded format YUV 4:2:2 (tag 84 = 1), colorspace BT.709 (tag 91 = 2), quality low word 6. The progressive sample flag is present (tag 68 = 1). The probe asks for full resolution and BGRA. The SDK maps BGRA + YUV 4:2:2 to internal `DECODED_FORMAT_RGB32` (`upstream/DecoderSDK/SampleDecoder.cpp`).

That is **not** the half-resolution / lowpass copy. `CopyLowpass16sToBuffer` → `ConvertLowpass16sToRGBNoIPPFast` is the half-resolution and lowpass-only branch inside `ReconstructSampleFrameToBuffer`. For this full-resolution progressive RGB32 decode the live call is:

`DecodeSampleIntraFrame` → `ReconstructSampleFrameToBuffer` → `TransformInverseSpatialUniversalThreadedToOutput` → `InvertHorizontalStrip16sYUVtoRGB` → `ConvertRow16uToDitheredRGB`

(`upstream/Codec/decoder.c` around the progressive full-resolution RGB8 arm, the `InvertHorizontalStrip16sYUVtoRGB` call). The non-threaded `BANDS_ALL_VALID` test in the same file (the `#else` of `_THREADED_DECODER`) is not on this path. The live queue gate is `BANDS_ALL_STARTED` (`decoder.c`, threaded arm of the subband decode).

### What the four dumps actually are

Re-measured against the retained modal dump (image-only digest `c5e0e052be84a807a899d0501fa73f93bbe9d67e0f1c9e5cfdef97a3ffee54a0`). File row 0 of the modal dump matches the top of the source picture, not the bottom, so the prefix is the top of the image.

| dump | divergent rows | tail bit-identical | channels bit-identical on the whole frame | bytes equal to `0xCD` in the divergent region |
| --- | --- | --- | --- | --- |
| c2 | 0..341 (342) | yes (738 rows) | R, A | 0 |
| c19 | 0..437 (438) | yes (642 rows) | R, A | 0 |
| c18 | 0..745 (746) | yes (334 rows) | R, A | 0 |
| c11 | 0..1079 (1080) | n/a (whole frame) | B, A | 0.0023 |

The `0xCD` figure is the fraction of bytes, not a filled sentinel block. The probe sentinel is not what occupies the prefix.

Row-shift test, this pass, whole row equal to the modal row at offset k ∈ {±1, ±2, ±4, ±8, ±16}: **0 / every compared row**, on all four dumps. A per-row constant delta is also false: in the divergent region c2 has 288 distinct B deltas and 63 distinct G deltas, and sampled rows are not constant. The same shape holds for c19 and c18. c11 has 136 distinct R deltas and 57 distinct G deltas. So this is not a row displacement and not the per-row DC offset described for #160.

### Channel selectivity is one chroma plane

`ConvertRow16uToDitheredRGB` (`upstream/Codec/convert.c`) documents the swap: U is plane 2, V is plane 1. In the RGB32 store, R is computed from Y and V only, B from Y and U only, G from all three, and A is the constant `RGBA_DEFAULT_ALPHA` (255).

A whole frame whose R and A bytes match the modal frame while B and G move (c2, c18, c19) is a changed U plane with Y and V unchanged. A frame whose B and A match while R and G move (c11) is a changed V plane with Y and U unchanged. It cannot be "whichever RGB channel lost the race": R does not read U, and B does not read V. Which chroma moves changes per run because each channel is its own transform and its own worker. A matching alpha byte only means both frames wrote the constant; there is no fourth wavelet. These four dumps do not show a torn Y plane. That does not prove Y cannot race.

### The predicate can release the consumer early

The old test required every band bit. The live macro does not:

```c
#define BANDS_ALL_VALID(wavelet)  ((wavelet)->band_valid_flags == (wavelet)->band_started_flags)
#define BANDS_ALL_STARTED(wavelet) (((wavelet)->band_started_flags & ((1u << num_bands) - 2)) == ((1u << num_bands) - 2))
```

`BANDS_ALL_STARTED` ignores band 0. For a 4-band spatial wavelet the inverse is queued once bands 1..3 have been *started*. The entropy worker then accepts that queue entry when `band_valid_flags == band_started_flags` (`upstream/Codec/entropy_threading.c`). If bands 1..3 are both started and valid, and band 0 has not been started, both words are `0xE` and the macro is true **while the lowpass band is still absent**. `DecodedBandsValid` in `decoder.c` is the check that would wait for every band not produced by this inverse; the worker does not call it. `ReconstructWaveletBand` repeats the same macro and, for a spatial wavelet with `index > 0`, calls `TransformInverseSpatialQuantLowpass`, which reads `input->band[0]` from the top of the band and writes the next level's lowpass from the top.

`AllTransformBandsValid`, which `ReconstructSampleFrameToBuffer` uses before the output inverse, only checks that the wavelet pointer is non-NULL. It does not look at the band flags. A lowpass that was reconstructed too early is still rendered.

`AllocImage` does not clear the coefficient buffer. The `memset` of the image memory is under `#if (0 && DEBUG)` (`upstream/Codec/image.c`). A consumer that runs ahead therefore reads uninitialized rows, not a previous frame (this failure already happens on the first decode of a fresh process) and not the probe's `0xCD` output sentinel. Uninitialized anonymous pages are typically demand-zero; that was not measured inside the wavelet.

### Why the visible damage is a top prefix of even length

The inverse walks rows from the top. Rows the producer has already written reconstruct to the modal value. Rows it has not written yet reconstruct from the unread contents. If the producer is behind at the start and overtakes the reader, the stored child band is wrong for a top prefix and bit-identical afterwards. The output stage then just converts that band.

Each spatial level doubles the row count. The final output writes two image rows per final-wavelet row, and the final vertical filter reads lowpass rows `row-1`, `row` and `row+1` (`InvertSpatialMiddleRow16sToOutput` in `upstream/Codec/spatial.c`). A tear of K rows on the 270-row band above the final wavelet therefore shows up as `4K` image rows, plus one extra final-wavelet row of support, which is `4K+2` image rows. The three partial dumps are all `2 (mod 4)`: 342 = 4×85+2, 438 = 4×109+2, 746 = 4×186+2. The full-frame dump is 1080 = 4×270, the entire 270-row band, so the extra support row falls off the bottom. 85, 109 and 186 are not band heights (the pyramid steps are 540, 270 and 135). The stop row is where the two workers happened to cross, not a subband boundary.

c11 (the whole frame, V rather than U) is the same race when the reader finishes before the producer writes any row. A prefix is the same race when the producer catches up mid-band. Which of those two schedulings happened on each dump was not traced.

### What this does not prove

- No wavelet row was logged during a live decode. The level (the 270-row band versus a torn final high band) is the geometry that fits the `mod 4` result, not a captured schedule.
- The unread coefficient bytes were not sampled, so "demand-zero" is the usual fresh-process behaviour, not a measurement.
- Nothing here says the luma channel cannot lose the same race.
- No fix was built. The change that matches the predicate bug would be to accept a queued inverse only from `DecodedBandsValid` (or the old full-bit mask), and not to mark the queue entry done while band 0 is still missing. That stays a proposal. The gate is unchanged.

### Next action

One forced-preemption decode, private instrumented copy only, logging for each channel the wavelet index at the moment `BANDS_ALL_VALID` accepts the queue entry, whether band 0's valid bit is clear, and the first lowpass row still equal to the allocation fill. Compare that channel to the chroma plane that moved. Do not change the tracked `upstream/` tree to do it.

## The unread rows are zero-filled, measured

The section above stated that "the unread coefficient bytes were not sampled, so
'demand-zero' is the usual fresh-process behaviour, not a measurement." That gap
is now closed on this host.

Sixty fresh processes, one decode each, 8 workers on one allowed CPU
(`cpu_max=8`, `taskset -c 0`), same pinned 900396-byte packet. Each decode wrote
its BGRA to its own directory. Eight of sixty frames (13.3%) differed from the
modal digest; for each divergent frame the whole output was compared byte-wise
against the modal frame.

| prefix rows | prefix mod 4 | plane moved | B changed | G changed | R changed | affected plane byte 0x00 |
| ---: | ---: | --- | ---: | ---: | ---: | ---: |
| 1080 | 0 | V | 0.0% | 100.0% | 100.0% | 100.0% |
| 678 | 2 | V | 0.0% | 99.6% | 99.7% | 99.0% |
| 234 | 2 | V | 0.0% | 99.2% | 99.2% | 97.4% |
| 1080 | 0 | V | 0.0% | 100.0% | 100.0% | 100.0% |
| 806 | 2 | U | 99.2% | 99.7% | 0.0% | 99.3% |
| 134 | 2 | V | 0.0% | 96.9% | 96.9% | 93.7% |
| 882 | 2 | V | 0.0% | 99.8% | 99.9% | 99.3% |
| 1080 | 0 | U | 99.3% | 100.0% | 0.0% | 100.0% |

Three results follow, each of which the earlier text only predicted:

1. **The affected plane is exactly `0x00` in the corrupted prefix** — 93.7% to
   100% of its bytes over all eight frames. Not a previous frame, not the probe
   `0xCD` sentinel, not a stale reconstruction: the reader consumed rows the
   producer had not written, and the allocation was demand-zero. The prediction
   is now a measurement on the same geometry the defect occurs on.
2. **The unaffected plane is byte-identical to the modal frame across the whole
   image** (0.0% changed), while the third channel moves. That is the U/V split:
   when U (Cb) is the plane that lost the race, B and G move and R stays exact;
   when V (Cr) is, R moves and B stays exact. Six of eight frames lost V, two
   lost U — the loser is whichever channel's queue entry the reader accepted
   early, not a fixed channel.
3. **Every prefix length is even, and five of eight are `2 (mod 4)`**, matching
   the "K rows on the 270-row band above the final wavelet, plus one support
   row" geometry derived above. The lengths are 134, 234, 678, 806, 882 and
   1080; the three full-frame cases are `0 (mod 4)` because the extra support
   row falls off the bottom, exactly as the geometry predicts.

What this still does not do: no wavelet row was logged during a live decode, so
the ordering of producer and reader is inferred from the output geometry rather
than captured. The demand-zero result explains *what* the reader consumed, not
*which* scheduling step let it run. That remains the instrumented-copy
experiment, and it needs an owner decision because it cannot be done in the
tracked `upstream/` tree.
