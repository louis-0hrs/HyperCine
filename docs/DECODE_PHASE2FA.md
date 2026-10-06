<!-- Copyright (c) 2026 HyperCine Contributors. MIT OR Apache-2.0. -->
# Decode Phase 2F-A: GPU-native checkpoint generation

## Provenance and scope

Branch decode-phase2fa is based on decode-phase2e at 3e24e7d. The starting
checkout and upstream subtree were clean. Both reference decoder targets built,
all 25 baseline CTests passed (205.64 s), and Phase 2E benchmark and timeline
JSONs existed before implementation. Hardware correction commit 9496ff7 removes
desktop RTX 4090 memory specifications and the unsupported parallel-prefix
prediction. No replacement bandwidth claim is made.

Production encoder sources, upstream, golden manifests, main and tags are
preserved. No push, merge or release is part of this work.

Phase 2E already overlaps the roughly 25 ms host index tail with GPU replay.
The historical 4K noise trace ends host indexing near 25.4 ms, critical Luma
replay near 52.9 ms, and output near 53.2 ms. Removing CPU work cannot be equated
with subtracting that work from latency.

This experiment moves the existing checkpoint architecture to device memory.
It does not implement transition matrices, generalized parallel FSM composition,
dynamic parallelism or a persistent entropy scheduler.

Callable paths remain:

* CudaDecoderPipelinePhase2D: original throughput reference.
* Phase2DFrameBarrier / Phase2DBarrier: retained barrier mode.
* Phase2EFullOverlap / Phase2EHostStreaming: current host-index reference and default.
* Phase2FAGpuIndexBatch / Phase2FAGpuIndex: opt-in complete-band GPU indexing.

The benchmark uses the original Phase 2D class for Phase2DBarrier rows.

## Immutable inputs and execution granularity

The existing bounded CPU tag parser produces GpuEntropyBandDescriptor.
Geometry, channel/level/orientation, codebook, quantization, precision, prediction
flags and payload offsets/bounds remain CPU responsibilities. The compressed
sample is staged to pinned memory and uploaded once. No CPU FSM checkpoint
traversal occurs in the new mode.

LaunchGenerateBandCheckpointsGpu uses one warp-sized block per independent band
launch. Lane 0 advances the serial nibble FSM. Other lanes do not pretend to know
independent restart states. Indexing writes descriptors and continuation state
only, never coefficients.

A launch advances at most 262,144 nibbles. Continuations retain FSM state,
nibble address, coefficient position, segment start and segment count. The host
enqueues a finite number of continuations from immutable payload size.
A short dispatch hint fails closed; extra launches return after termination.
There is no host progress poll or device allocation.

Independent bands execute through a bounded shared stream pool. Index
continuations precede that band's replay in its stream. Replay waits for its own
complete index without an unrelated-band frame barrier.

## Checkpoint contract and equivalence

GpuEntropySegmentDescriptor is unchanged. Field comparisons cover:

| Field | Meaning |
|---|---|
| band_index | Canonical highpass descriptor index, 0 through 26 |
| byte_offset, nibble_idx | Next compressed nibble at segment entry |
| initial_state | Incoming FSM state before that nibble |
| out_coeff_pos | Coefficient advancement at segment entry |
| end_byte_offset, end_nibble_idx | Exclusive replay boundary |
| max_coeff_pos | Original host safety bound |

Both scanners advance position by pre_skip + post_skip and state by next_state.
The same consumed-nibble count, minimum interval of 16 nibbles, and adaptive
target determine boundaries. A new descriptor captures state and position
after advancing the preceding nibble. The larger-band final segment ends before
the marker. Continuation boundaries only save/resume this recurrence; they do
not create checkpoints.

The host's one-segment optimization bypasses traversal and spans the payload.
GPU indexing preserves that descriptor exactly but still validates the payload
before replay. Missing markers therefore fail more strictly than the old host
scanner. Legitimate early markers remain valid; trailing zeros are implicit.

Adaptive thresholds remain 512 B, 4 KiB, 32 KiB, 128 KiB and 512 KiB, producing
1, 4, 8, 16, 32 or 64 target segments. Segmentation is not retuned.

## Device storage and direct replay

Each slot has 27 fixed slices of 64 checkpoints, one index result per band,
one device terminal-error scalar and one pinned host error scalar. No kernel
allocation or transition-summary scratch is used. Actual structure sizes are
emitted by the benchmark; the old prose's 24-byte descriptor size is not assumed.

The bounded device slices and result records are cleared before scanning.
Initcheck exposed untouched ABI padding in member-wise CUDA struct stores;
explicit device initialization fixes full-record diagnostic copies without
changing the checkpoint layout or semantics.

Replay launches up to 64 blocks and reads the device segment count. Unused blocks
exit before reading uninitialized descriptors/results. Existing replay arithmetic
and separate row difference restoration remain.

There is no checkpoint D2H/H2D round trip in decode. GetGpuCheckpoints is an
explicit post-completion validation helper. Stage diagnostics copy index-result
records after the timed interval. The final error-scalar D2H validates output
publication; it is not a checkpoint barrier.

## Bounds, watchdog and errors

The indexer validates codebook ID, table extent, incoming/next state, method,
unsupported peaks, coefficient capacity and segment capacity. Payload bounds
use subtraction to avoid address addition overflow. Payloads and dispatch hints
are capped at 32 MiB, consistent with existing slot sample capacity.

Coefficient arithmetic is widened before checking writes and the existing
capacity + 8 advancement allowance. Invalid states cannot index another
codebook's table. The consumed-nibble bound increases on every non-terminating
transition, including zero-advancement entries. Payload exhaustion without a
marker is terminal. Failure discards the partial checkpoint count. No loop
can run indefinitely.

atomicCAS preserves the first terminal frame error. Replay, difference,
inverse and color check the flag. Guards are uniform within a block and use one
atomic read per block, avoiding an atomic per pixel. Inverse from unrelated,
already-valid dependencies may finish before a later error is discovered;
no valid output can be published afterward.

The ready event means the dependency chain and error copy have drained.
WaitForDecode/PollDecode report Failed for a nonzero scalar. Output getters
require Complete. Pending release and destruction drain GPU uses before memory
reuse/free; malformed-ticket reclamation is tested.

## Stream/event DAG and Phase 2E integration

Per-band dependencies are:

    compressed sample + descriptors + reset error ready
      -> bounded GPU index continuations
      -> index done
      -> zero band
      -> device-count guarded parallel replay
      -> optional row difference restoration
      -> band done

The existing Phase 2E inverse scheduler remains: three highpass events plus
LL readiness feed deepest inverse; each channel stream serializes parent LL
production into the next level. Three final channel events feed color,
the final error copy, and ready.

No host checkpoint publication, host checkpoint barrier or worker batch occurs
in the new mode. The configurable worker pool may remain allocated when
switching modes, but it is idle during GPU indexing. Active index workers are zero.

## Exactness, malformed input and determinism methodology

Stage 1 scans all 27 bands of every golden vector without replay or coefficient
output, comparing every checkpoint field with GenerateSingleBandCheckpointsCpu.
Stage 2 compares CPU, Phase 2E and Phase 2F-A coefficients, both LL intermediates,
planar output, BGRA bytes and frozen hashes. Retained suites independently
compare Phase 2B/C/D against the same reference; expected outputs are unchanged.

Focused deterministic synthetic tables isolate zero/tiny payloads, zero runs,
dense symbols, alternating signs, byte/odd-nibble boundaries, state transitions,
markers, 64 segments, continuation, truncation, missing markers, invalid
codebooks/states, address overflow, coefficient overflow and capacity exhaustion.

Reordering varies policies and delays Luma/chroma/level dispatch. Deepest-first
scheduling must launch inverse while unrelated bands are still indexing.
Corrupting only a sized payload preserves valid container parsing and tests
device failure, blocked output, reclamation and early release.

Default stress checks complete output bytes and checkpoint fields for 200
1080p noise, 100 4K noise and 20 8K flat runs. Sanitizer stress uses two repeats
of each size; --no-stress retains corpus, reordering and malformed-ticket tests.
--sanitizer-corpus retains these integrated checks while omitting the duplicate
standalone Stage 1 scan. --sanitizer-stress also omits that duplicate scan and
adds paired output/checkpoint repeats. The normal suite always runs Stage 1
directly for every corpus band. Sanitizer corpus comparisons still exercise the
same scanner through BGRA and planar decode and compare all checkpoint fields.

## Benchmark methodology and limits

The deterministic encoder sequence supplies five patterns at 1080p and 4K,
plus 8K flat. Each mode uses two warmups per pattern, five timed repeats, and
50 frames per 1–4-slot sweep. Host indexing uses eight workers. GPU indexing
uses four shared streams, with a 2/4/8 shared-pool sweep on 4K noise.

Single-frame latency encloses QueueDecode through WaitForDecode. Multi-slot
latency measures submission through a consumer-stream readiness callback,
including callback overhead. FPS is frames divided by batch wall time, not
the reciprocal of mean multi-slot latency. Downloads/hashes/stage-result copies
are excluded from timing.

Windows process CPU time encloses submission and waiting, including CUDA host
wait behavior. CPU milliseconds per frame must accompany utilization: a slower
mode can show lower percentage simply by producing fewer frames. Index-thread
CPU time uses GetThreadTimes; short tasks may fall below accounting granularity.
Peak worker count measures concurrency, not full hardware-core utilization.
Scheduling CPU and wall durations are separate from traversal.

Index work sums in-kernel elapsed nanoseconds from the device global timer,
excluding launch gaps. Index wall spans earliest band index start through latest
completion. Per-band event spans include queue gaps and contention. Maximum
active index-launch duration is device-instrumented, excluding the final result
store/epilogue; it is not a conversion from advertised SM clock rates.

GPU events are relative to the compressed-copy stream frame-start event.
CPU parse/index timestamps use the host submission clock. They are labeled
separately and cannot be subtracted across clocks. Headline host latency includes
parse and staging. Historical Phase 2E traces remain, and new traces remeasure
Phase 2E beside 2F-A. Phase 2E task records now use actual band IDs; the prior
implementation could associate every band's index timing with the first task.

Peak process working set includes corpus generation and all comparison
pipelines. Device-used bytes are a snapshot including other contexts and all
allocated reference pipelines, not a measured per-slot peak.

External nvidia-smi sampling uses timestamped utilization, memory, SM clock,
temperature and power counters. Row measurement intervals permit approximate
sample attribution; short rows may have no samples. These coarse counters do
not establish SM occupancy or a causal replay slowdown. Instrumented safety
runs are separate from performance measurement.

The raw counter CSV has no header and preserves nvidia-smi output. Columns are
local timestamp, GPU busy %, memory-controller busy %, used MiB, SM MHz,
temperature C and power W. Unavailable utilization values remain in the raw
file and are excluded from the derived averages. All new sources and artifacts
are dual-licensed under MIT OR Apache-2.0.

## Measured outcome: keep F-A experimental

Outcome D. Every measured single-frame pattern is slower in F-A. Four slots do not restore throughput. Keep D/E and the established E default.

Hardware: Ryzen 9 7940HS, 16 logical CPUs; RTX 4090 Laptop GPU; NVIDIA 610.62; CUDA Toolkit 12.8; Release/MSVC. The CUDA driver API reports 13030; this is distinct from the NVIDIA driver version.

[Raw measurements](../benchmarks/results/decode_phase2fa_windows_msvc.json) contain 72 rows. [Analysis](../benchmarks/results/decode_phase2fa_analysis.json) includes approximate [GPU counter attribution](../benchmarks/results/decode_phase2fa_gpu_samples.csv). Three unavailable counter samples were excluded. Instrumented safety workloads did not run during performance collection.

### Per-pattern single-frame performance

Five timed frames after two warmups per pattern. Cells contain average latency in ms / FPS / MP/s. Min/max, CPU time and memory are retained per row in the raw JSON.


| Width | Pattern | D ms / FPS / MP/s | E ms / FPS / MP/s | F-A ms / FPS / MP/s |
|---|---|---|---|---|
| 1920 | flat | 0.92 / 1090.77 / 2261.83 | 2.68 / 373.75 / 775.02 | 4.55 / 219.63 / 455.43 |
| 1920 | gradient | 3.16 / 316.29 / 655.86 | 6.51 / 153.54 / 318.38 | 40.40 / 24.75 / 51.32 |
| 1920 | noise | 17.62 / 56.76 / 117.70 | 16.21 / 61.68 / 127.90 | 706.06 / 1.42 / 2.94 |
| 1920 | texture | 9.08 / 110.15 / 228.41 | 8.11 / 123.28 / 255.64 | 196.94 / 5.08 / 10.53 |
| 1920 | edges | 4.74 / 210.79 / 437.10 | 8.24 / 121.39 / 251.72 | 74.70 / 13.39 / 27.76 |
| 3840 | flat | 1.63 / 613.96 / 5092.40 | 3.20 / 312.10 / 2588.72 | 13.98 / 71.55 / 593.48 |
| 3840 | gradient | 4.43 / 225.92 / 1873.91 | 7.35 / 136.03 / 1128.28 | 111.67 / 8.95 / 74.27 |
| 3840 | noise | 66.57 / 15.02 / 124.60 | 61.52 / 16.26 / 134.83 | 2835.66 / 0.35 / 2.93 |
| 3840 | texture | 17.91 / 55.85 / 463.23 | 17.34 / 57.66 / 478.22 | 775.30 / 1.29 / 10.70 |
| 3840 | edges | 8.74 / 114.44 / 949.18 | 13.78 / 72.58 / 602.00 | 281.65 / 3.55 / 29.45 |
| 7680 | flat | 5.88 / 170.20 / 5646.69 | 8.85 / 112.96 / 3747.79 | 52.42 / 19.08 / 632.88 |

### Five-pattern aggregates


| Width | Mode | Avg ms | FPS | MP/s | CPU ms/frame |
|---|---|---|---|---|---|
| 1920 | Phase2DBarrier | 7.10 | 140.77 | 291.90 | 14.38 |
| 1920 | Phase2EHostStreaming | 8.35 | 119.76 | 248.33 | 19.38 |
| 1920 | Phase2FAGpuIndexBatch | 204.53 | 4.89 | 10.14 | 205.00 |
| 3840 | Phase2DBarrier | 19.85 | 50.37 | 417.79 | 38.12 |
| 3840 | Phase2EHostStreaming | 20.64 | 48.45 | 401.88 | 39.38 |
| 3840 | Phase2FAGpuIndexBatch | 803.65 | 1.24 | 10.32 | 798.12 |

### Slot sweep

50 frames per row, equal five-pattern mix. GPU percentage is approximate sampled busy time, not SM occupancy. Short intervals may have no samples.


| Width | Mode | Slots | Avg ready ms | FPS | MP/s | CPU % of 16 CPUs | GPU % (samples) |
|---|---|---|---|---|---|---|---|
| 1920 | Phase2DBarrier | 1 | 6.27 | 159.32 | 330.37 | 11.51 | n/a |
| 1920 | Phase2EHostStreaming | 1 | 7.58 | 131.89 | 273.48 | 12.11 | n/a |
| 1920 | Phase2FAGpuIndexBatch | 1 | 204.83 | 4.88 | 10.12 | 6.25 | 99.65 (20) |
| 1920 | Phase2DBarrier | 2 | 6.14 | 241.58 | 500.94 | 15.10 | 100.00 (1) |
| 1920 | Phase2EHostStreaming | 2 | 10.52 | 167.05 | 346.40 | 13.70 | n/a |
| 1920 | Phase2FAGpuIndexBatch | 2 | 409.40 | 4.87 | 10.09 | 6.24 | 99.90 (21) |
| 1920 | Phase2DBarrier | 3 | 7.24 | 252.58 | 523.75 | 11.84 | n/a |
| 1920 | Phase2EHostStreaming | 3 | 14.89 | 166.30 | 344.84 | 11.69 | 72.00 (1) |
| 1920 | Phase2FAGpuIndexBatch | 3 | 609.35 | 4.87 | 10.09 | 6.24 | 100.00 (20) |
| 1920 | Phase2DBarrier | 4 | 8.22 | 285.85 | 592.73 | 12.84 | 66.00 (1) |
| 1920 | Phase2EHostStreaming | 4 | 21.19 | 153.97 | 319.26 | 14.43 | n/a |
| 1920 | Phase2FAGpuIndexBatch | 4 | 795.54 | 4.86 | 10.08 | 6.22 | 99.95 (20) |
| 3840 | Phase2DBarrier | 1 | 19.93 | 50.17 | 416.11 | 13.03 | 49.00 (2) |
| 3840 | Phase2EHostStreaming | 1 | 21.36 | 46.81 | 388.22 | 12.07 | 84.50 (2) |
| 3840 | Phase2FAGpuIndexBatch | 1 | 803.62 | 1.24 | 10.32 | 6.23 | 99.99 (79) |
| 3840 | Phase2DBarrier | 2 | 20.33 | 64.49 | 534.92 | 12.22 | 55.00 (2) |
| 3840 | Phase2EHostStreaming | 2 | 26.28 | 61.05 | 506.34 | 16.81 | 87.00 (1) |
| 3840 | Phase2FAGpuIndexBatch | 2 | 1600.82 | 1.24 | 10.33 | 6.24 | 100.00 (79) |
| 3840 | Phase2DBarrier | 3 | 24.60 | 68.75 | 570.22 | 13.29 | 55.50 (2) |
| 3840 | Phase2EHostStreaming | 3 | 33.03 | 62.11 | 515.20 | 13.34 | 80.50 (2) |
| 3840 | Phase2FAGpuIndexBatch | 3 | 2382.80 | 1.24 | 10.33 | 6.23 | 100.00 (79) |
| 3840 | Phase2DBarrier | 4 | 29.04 | 71.21 | 590.63 | 13.35 | 43.00 (2) |
| 3840 | Phase2EHostStreaming | 4 | 37.63 | 65.45 | 542.84 | 16.49 | 95.00 (1) |
| 3840 | Phase2FAGpuIndexBatch | 4 | 3108.59 | 1.24 | 10.32 | 6.23 | 99.97 (79) |

### CPU cost: 4K noise


| Mode | Process CPU ms/frame | CPU % | Peak workers | Last index-thread CPU ms | Queue avg ms |
|---|---|---|---|---|---|
| Phase2EHostStreaming | 150.00 | 15.24 | 8 | 93.75 | 33.25 |
| Phase2FAGpuIndexBatch | 2806.25 | 6.19 | 0 | 0.00 | 3.19 |

## Critical Luma and dependency analysis

[E trace](../benchmarks/results/timeline_phase2e_remeasured_4k_noise.json) and [F-A trace](../benchmarks/results/timeline_phase2fa_4k_noise.json) describe the last timed 4K noise frame. Historical E traces remain untouched. Band 6 is the last-finishing Luma L0 highpass in both; band 7 has the largest payload. All three have 64 checkpoints.


| Band | Bytes | E host index ms | F-A index work ms | F-A index start/end ms | E replay start/end ms | F-A replay start/end ms | Replay span E / F-A ms |
|---|---|---|---|---|---|---|---|
| 6 | 5022296 | 27.88 | 2258.21 | 2.24 / 2260.58 | 28.04 / 56.19 | 2260.58 / 2287.54 | 28.15 / 26.96 |
| 7 | 5030940 | 27.37 | 2254.47 | 2.19 / 2256.84 | 27.54 / 54.85 | 2256.84 / 2283.85 | 27.31 / 27.01 |
| 8 | 4448836 | 22.55 | 1994.22 | 2.28 / 1996.65 | 22.74 / 48.03 | 1996.65 / 2020.58 | 25.29 / 23.93 |


| GPU-clock marker | E ms | F-A ms |
|---|---|---|
| H2D start | 0.01 | 0.01 |
| H2D end | 1.66 | 1.76 |
| First GPU index start | n/a | 2.19 |
| First GPU index complete | n/a | 564.27 |
| First replay event | 1.68 | 564.27 |
| First inverse | 7.76 | 2758.58 |
| Final Luma inverse start | 56.22 | 2827.95 |
| Final Luma inverse end | 56.28 | 2828.03 |
| Color start | 56.31 | 2834.47 |
| Color end | 56.45 | 2834.64 |
| Ready event | 56.45 | 2834.68 |

## Shared-pool sweep


| Shared streams | 4K noise avg ms | Min ms | Max ms |
|---|---|---|---|
| 2 | 4969.03 | 4968.56 | 4969.23 |
| 4 | 2835.37 | 2834.84 | 2835.93 |
| 8 | 2386.20 | 2380.93 | 2389.32 |

## Core codec regression measurements


| Resolution | Encode FPS / MP/s | Encode avg [min,max] ms | Decode FPS / MP/s | Decode avg [min,max] ms | Peak MiB | FPS delta encode / decode % |
|---|---|---|---|---|---|---|
| 1080p | 86.72 / 179.82 | 11.26 [10.78, 14.01] | 527.22 / 1093.23 | 1.90 [1.79, 2.24] | 92.51 | 11.61 / 33.76 |
| 4K_UHD | 25.09 / 208.09 | 38.96 [38.01, 40.32] | 132.59 / 1099.78 | 7.54 [6.90, 7.97] | 229.30 | 14.89 / 5.81 |
| 8K_UHD | 6.64 / 220.33 | 147.15 [141.62, 158.92] | 32.09 / 1064.82 | 31.16 [28.94, 33.25] | 614.14 | 55.08 / 14.77 |

[Current results](../benchmarks/results/reference_windows_msvc_phase2fa.json) are compared with the untouched [baseline](../benchmarks/results/reference_windows_msvc.json). No throughput drop appears. Faster historical comparisons are not claimed as optimizations: production encoder/CPU codec sources are unchanged and collection conditions differ. The benchmark writer adds stdout JSON output for PowerShell saving; numeric formatting matches existing file precision.

Peak process working set: 572.39 MiB. Maximum device-used snapshot: 3547.50 MiB. Both include initialized comparison pipelines and corpus storage, not incremental/per-slot peaks.

## Interpretation, safety margin and recommendation

Host indexing CPU work is eliminated: the last E 4K noise frame records 93.75 ms
across index threads; F-A records zero and has zero active index workers.
Queue wall time falls from 33.25 to 3.19 ms. However, total process CPU cost
in this blocking harness rises from 150 to 2806.25 ms/frame. CUDA wait behavior
is included. Lower machine utilization (15.24% to 6.19%) accompanies roughly
46x lower frame production, so this is not a CPU-efficiency win. Async clients
may avoid spin-wait cost; this experiment does not establish their CPU savings
or acceptable interactive latency.

F-A's last-frame host enqueue span is 2.387 ms. Scheduling thread CPU counters
for E/F-A report zero at the Windows accounting granularity; this is not proof
of zero CPU work. E's host_schedule_wall_ms=0 is an uninstrumented placeholder,
not a measured zero-duration dispatch. E queue time includes host indexing;
no pure dispatch wall time is inferred from it.

Replay spans include zeroing and row prediction restoration; E also includes
checkpoint H2D. These are event-enclosed band spans, not isolated VLC kernel
durations. Band 6 changes from 28.148 to 26.960 ms. Bands 7/8 likewise do not
slow materially. A replay-span regression does not explain the frame slowdown.
This does not prove zero resource contention in every configuration.

The slowest GPU index band is band 6: 2258.21 ms summed body work and
2258.34 ms event span for 5,022,296 bytes. Band 7 has the largest payload:
5,030,940 bytes. Total GPU index body work is 8292.36 ms; index wall is
2831.98 ms. These are different measures of concurrent execution.

F-A's Luma L0 replay ends at 2287.54 ms, but final Luma inverse starts at
2827.95 ms. Its incoming LL depends on lower-level bands queued behind other
scans in the inherited largest-first pool. First inverse starts at 2758.58 ms.
The new critical path is serial indexing plus prerequisite-band scheduling.
Inverse arithmetic remains unchanged.

CPU parse completes at 0.0152 ms for E and 0.0165 ms for F-A, measured from
host submission. GPU trace ready markers are 56.45 and 2834.68 ms. The headline
comparison uses host averages 61.516 and 2835.662 ms, including submission and
waiting. Historical ~53 ms E traces and these samples are separate measurements.
CPU and GPU origins must not be subtracted.
Legacy overlap_saved_ms/overlap_efficiency exports use the earlier mixed-clock
estimator and are not used for conclusions here. Likewise, host_index_cpu_work_ms
is the legacy sum of task wall spans; actual thread CPU accounting uses the new
host_index_thread_cpu_ms field.

Maximum observed active single-index-launch body duration across the benchmark
and stream sweep is **59.833 ms**. The largest Luma payload uses 39 bounded
continuations instead of a multi-second monolithic kernel. The timer stops
before the final result store/epilogue; this is an instrumented body duration,
not an Nsight full-kernel measurement. No TDR settings were changed. Progress,
address and capacity bounds remain mandatory regardless of timing results.

Measured layouts are 28 bytes per checkpoint, 136 per immutable band descriptor
and 56 per index result. Existing device checkpoint slices use 27 x 64 x 28 =
48,384 bytes per slot. New result records and terminal scalar add 27 x 56 + 4 =
1,516 device bytes and four pinned host bytes. The 54 additional event handles
have opaque driver overhead. Existing pinned checkpoint storage remains
allocated for reference-mode switching and is unused by F-A.

At most eight shared entropy streams plus copy, main and three channel streams
are allocated per slot: 13 total, nine used with the default four-stream pool.
The benchmark adds at most four consumer streams. There is no stream per band.
Eight shared streams are the best measured experimental latency configuration
at 2386.20 ms, still about 39x slower than E. Four remains the public default
and the comparable baseline.

Separate index/replay stream priorities were not tested: shared streams fix
their relative priority, and separate pools would change the dispatch model.
Priority is unnecessary for correctness and cannot remove serial FSM dependency.
Progressive publication, dynamic parallelism and persistent kernels were not
attempted. Even perfect replay overlap cannot make the required ~2.26 s
critical-band scan competitive with E's ~61.5 ms frame latency.

D with four slots is throughput-optimal in this sweep: 285.85 FPS at 1080p,
71.21 at 4K. E has the best single-frame noise/texture latency; D wins the
aggregate and low-entropy cases. Retain explicit workload selection and the
existing E default. F-A earns no promotion.

Phase 2F-B is justified only as a limited feasibility study of stronger
intra-band FSM parallelism under a small scratch budget. E still has a VLC
critical tail, while F-A creates a larger scalar-index bottleneck. Do not
reproduce the 518-state transition-summary design or make release integration
depend on this research. Phase 3 should use stable D/E unless a later algorithm
demonstrates measured gains. Optional 8K noise/texture were not added; the
required corpus and 8K flat already establish the negative result.

## Sanitizer coverage and reproduction

| Tool | Focused scanner | Integrated corpus | Additional coverage |
|---|---|---|---|
| Memcheck | 20/20 cases, unfiltered | 11/11, unfiltered | Reordering, malformed recovery, paired stress at all three sizes; zero leaks/errors |
| Initcheck | 20/20 cases, unfiltered | 11/11, unfiltered | Reordering and malformed recovery; zero errors |
| Racecheck | 20/20 cases, unfiltered | 11/11 downstream kernels | Reordering and malformed recovery; zero hazards/errors/warnings |
| Synccheck | 20/20 cases, unfiltered | 11/11 downstream kernels | Reordering and malformed recovery; zero errors |

The corpus Racecheck/Synccheck commands exclude IndexBand: it has one active
lane and no inter-thread synchronization/shared-memory communication. Its
focused runs are unfiltered. Memcheck/Initcheck instrument the real indexer
throughout the entire corpus. Instrumentation is much slower than ordinary
execution; reduced sanitizer stress retains two runs per size, while the normal
suite retains 200/100/20. Safety workloads never supply headline performance.

Representative commands on this machine:

```powershell
cmake -S . -B build -DHYPERCINE_ENABLE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure -V

cmake -S . -B build_cpu -DHYPERCINE_ENABLE_CUDA=OFF
cmake --build build_cpu --config Release
ctest --test-dir build_cpu -C Release --output-on-failure

compute-sanitizer --tool memcheck --leak-check full --error-exitcode 99 build/tests/Release/test_decode_gpu_index.exe
compute-sanitizer --tool memcheck --leak-check full --error-exitcode 99 build/tests/Release/test_decode_phase2fa.exe tests/decode_phase0_golden.txt --sanitizer-stress
compute-sanitizer --tool initcheck --error-exitcode 99 build/tests/Release/test_decode_phase2fa.exe tests/decode_phase0_golden.txt --sanitizer-corpus
compute-sanitizer --tool racecheck --kernel-name-exclude kns=IndexBand --error-exitcode 99 build/tests/Release/test_decode_phase2fa.exe tests/decode_phase0_golden.txt --sanitizer-corpus
compute-sanitizer --tool synccheck --kernel-name-exclude kns=IndexBand --error-exitcode 99 build/tests/Release/test_decode_phase2fa.exe tests/decode_phase0_golden.txt --sanitizer-corpus

build/benchmarks/Release/benchmark_decode_phase2fa.exe > benchmarks/results/decode_phase2fa_windows_msvc.json
build/benchmarks/Release/benchmark_codec.exe --json - > build/phase2fa_codec_benchmark.txt
```

Focused Initcheck/Racecheck/Synccheck use the scanner executable with the same
tool and error-exitcode options. The codec stdout also contains its human-readable
summary; extract the JSON object between its first opening and last closing
brace before saving reference_windows_msvc_phase2fa.json. Run benchmark workloads
after all GPU tests and sanitizer processes exit.

## Final completion report

The final isolated CUDA CTest run passed 27/27 in 636.34 seconds; the CUDA-disabled
build passed 5/5 in 4.77 seconds. Machine-readable checks are in
[validation JSON](../benchmarks/results/decode_phase2fa_validation.json), with
the recorded sanitizer summaries and CTest evidence in
[validation logs](../benchmarks/results/decode_phase2fa_validation.txt).
The implementation's measured source hashes are recorded in
[provenance JSON](../benchmarks/results/decode_phase2fa_provenance.json).

No push, merge, tag movement or release was performed. Main remains at 51435890ba5853e87e8cf1ddc7cc747954bd9e80.

| # | Requested item | Result |
|---|---|---|
| 1 | Branch | decode-phase2fa |
| 2 | Base commit | 3e24e7da42bdde5f5c59237c7ae1de38a2749637 |
| 3 | Documentation correction | 9496ff7e31e700bb9431253647edc6f4993d6075 |
| 4 | Implementation commit | 6eb421d402ea88c625ba57caa017278b1746cd80 |
| 5 | Files changed | 19 source/build/test files, two decoder documents and nine new measurement/validation artifacts; full list below |
| 6 | Production encoder | Unchanged; benchmark JSON writer only |
| 7 | Upstream | Pristine; frozen subtree 837528ef9e9a55c416e9592ecb54e1c8f1ff9cf8 matches base |
| 8 | Phase 2D | Original class and barrier alias preserved, built and measured |
| 9 | Phase 2E | Host-streaming/full-overlap path and default preserved |
| 10 | GPU index architecture | Device payload -> serial band FSM -> device checkpoint descriptors; no coefficients in Stage 1 |
| 11 | Execution granularity | One 32-lane warp per band launch, lane 0 advances; at most 262144 nibbles per launch |
| 12 | Checkpoint format | Unchanged 28-byte descriptor; exact byte/nibble/state/position/end/safety fields |
| 13 | Capacity | 64 entries per band, 27 preallocated slices; existing adaptive 1/4/8/16/32/64 segmentation |
| 14 | Bounds/watchdog | 32 MiB payload cap, finite continuations, checked addresses/states/positions/capacity; no TDR configuration changes |
| 15 | Device storage | No checkpoint round trip in decode; diagnostic D2H happens after completion outside timing |
| 16 | Replay integration | Reads device count and descriptors; guarded replay/difference/inverse/color; first terminal error preserved |
| 17 | Streams | Bounded shared pool, four default, 2/4/8 tested; three channel streams plus copy/main; no stream per band |
| 18 | Stream priority | Not tested; separate index/replay priorities require a different pool topology |
| 19 | Checkpoint equivalence | Zero field mismatches across all 27 VLC bands of all 11 golden vectors; direct Stage 1 and integrated validation |
| 20 | Synthetic tests | 20/20 deterministic cases; all four focused sanitizer tools clean |
| 21 | Malformed input | Bounds/state/capacity/termination/overflow cases fail safely; no valid output, reclaim and pending release verified; legitimate early marker retained |
| 22 | Coefficient exactness | 30/30 bands exact, maximum error zero; older B/C/D suites retain common CPU-reference comparisons |
| 23 | DeviceBGRA exactness | Full bytes and frozen hashes exact; both LL levels and planar YUV exact |
| 24 | Golden corpus | 11/11 PASS; expected outputs not regenerated |
| 25 | Determinism | 200/200 1080p noise, 100/100 4K noise, 20/20 8K flat; paired sanitizer repeats for each |
| 26 | Sanitizers | Memcheck/initcheck full integrated corpus clean; racecheck/synccheck downstream corpus clean excluding the single-lane scanner, separately checked in focused suites; zero leaks in Memcheck |
| 27 | Maximum index launch | 59.833 ms observed body duration in isolated benchmark; excludes final store/epilogue |
| 28 | 1080p noise E latency | 16.21278 ms mean |
| 29 | 1080p noise F-A latency | 706.06102 ms mean |
| 30 | 4K noise E latency | 61.51622 ms mean |
| 31 | 4K noise F-A latency | 2835.66152 ms mean |
| 32 | Critical Luma E index | Band 6, 5,022,296 bytes: 27.8831 ms host wall duration |
| 33 | Critical Luma F-A index | 2258.21 ms body work; GPU event 2.23539 -> 2260.58 ms |
| 34 | Critical Luma E replay | GPU event 28.0381 -> 56.1859 ms |
| 35 | Critical Luma F-A replay | GPU event 2260.58 -> 2287.54 ms |
| 36 | Replay contention | 28.148 -> 26.960 ms band span; no measured regression here; scalar indexing and queued prerequisites dominate |
| 37 | CPU relief | Index CPU 93.75 -> 0 ms and queue wall 33.25 -> 3.19 ms; blocking process CPU/frame 150 -> 2806.25 ms; lower utilization is not efficiency |
| 38 | 1080p five-pattern aggregate | D 7.104 ms / 140.77 FPS; E 8.350 ms / 119.76 FPS; F-A 204.532 ms / 4.889 FPS |
| 39 | 4K five-pattern aggregate | D 19.853 ms / 50.37 FPS; E 20.639 ms / 48.45 FPS; F-A 803.652 ms / 1.244 FPS |
| 40 | 8K flat | D 5.876 ms; E 8.853 ms; F-A 52.423 ms |
| 41 | Slot sweep | 1-4 slots, 50 frames each per mode and resolution; latency, FPS, MP/s, CPU and approximate GPU counters in tables/JSON |
| 42 | Latency-optimal mode | E for noise/texture; D for aggregates and low entropy; keep workload-specific selection |
| 43 | Throughput-optimal mode | D with four slots: 285.85 FPS 1080p, 71.21 FPS 4K |
| 44 | CPU test suite | 5/5 PASS (4.77 s), CUDA OFF configured and compiled |
| 45 | CUDA test suite | 27/27 PASS (636.34 s), all earlier suites retained |
| 46 | Remaining critical path | F-A serial GPU indexing and lower-level LL prerequisites queued in shared pool; E retains VLC tail |
| 47 | New default? | No. Outcome D; F-A remains explicit experimental mode; D/E retained |
| 48 | Phase 2F-B? | Limited small-memory intra-band-parallelism feasibility research only; no release dependency; Phase 3 can use stable D/E |

### Changed files

- benchmarks/CMakeLists.txt
- benchmarks/benchmark_codec.cpp
- benchmarks/benchmark_decode_phase2fa.cpp
- benchmarks/results/decode_phase2fa_analysis.json
- benchmarks/results/decode_phase2fa_gpu_samples.csv
- benchmarks/results/decode_phase2fa_provenance.json
- benchmarks/results/decode_phase2fa_validation.json
- benchmarks/results/decode_phase2fa_validation.txt
- benchmarks/results/decode_phase2fa_windows_msvc.json
- benchmarks/results/reference_windows_msvc_phase2fa.json
- benchmarks/results/timeline_phase2e_remeasured_4k_noise.json
- benchmarks/results/timeline_phase2fa_4k_noise.json
- docs/DECODE_PHASE2E.md
- docs/DECODE_PHASE2FA.md
- include/hypercine/decode_color_cuda.h
- include/hypercine/decode_entropy_cuda.h
- include/hypercine/decode_gpu_index.h
- include/hypercine/decode_phase2e.h
- src/CMakeLists.txt
- src/cuda/decode_color_cuda.cu
- src/cuda/decode_entropy_cuda.cu
- src/cuda/decode_error_guard.cuh
- src/cuda/decode_gpu_index.cu
- src/cuda/decode_gpu_index_stub.cpp
- src/cuda/decode_inverse_kernels.cuh
- src/cuda/decode_phase2e_pipeline.cu
- src/cuda/decode_phase2e_stub.cpp
- tests/CMakeLists.txt
- tests/test_decode_gpu_index.cu
- tests/test_decode_phase2fa.cpp
