<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# FilmScan3 entropy P3 — Issue 42

Base: PR41 `db98869012a75fa628110e14fb25f93697a78754`, still draft at start.
Isolated branch `codex/macos-vlc-entropy-p3`; no edits to P2's checkout or upstream.

## Stage A: frozen instruments

`tests/entropy_legacy.h` independently freezes the complete old Bits and
PackHighpass source from PR41. It is only compiled into tests/diagnostics.
The test compares complete payloads, exact capacity and rejected short capacity,
all signed int16 values, cubic/saturation boundaries, long RLE thresholds,
padded pitch, invalid geometry and 1001 reused bands. It also compares the real
27 quantized bands for each 1080p flat/gradient/noise input against the oracle.

`validation/metal-p3/p2-before.json` is a fresh native run from the unchanged
PR41 encoder, before production edits. The benchmark executable SHA256 is
`7194990bd1b6b79300d6487c4387b1ae35320803810c45eb97baeb880a4fdf22`.
The existing benchmark records 30 warm CPU/Metal pairs per fixed input after
two initial frames, plus separate changing 16/120-frame stateful sequences.
Inputs, packets and limiter state are retained per frame. Timings include the
unchanged quantization, all public coefficient hashes and full packet writer.

`validation/metal-p3/profile_entropy.cpp` is a separate diagnostic harness:
it reports all 27 band sizes and nonzero counts for three real patterns,
complete PackHighpass duration and split-pass classification/mapping/RLE/legacy
bit packing with reserved temporary vector bytes. Split passes add temporary
storage and change cache behavior: their durations must not be summed and
presented as production latency. No per-symbol clocks enter production.

Reproduce the diagnostic after building the standalone library:

```sh
c++ -O3 -DNDEBUG -std=c++17 -arch arm64 -Iinclude \
  docs/validation/metal-p3/profile_entropy.cpp build/libhypercine_standalone.a \
  -framework Metal -framework Foundation -o /tmp/p3-profile
/tmp/p3-profile > /tmp/p3-profile.json
```

The standalone CMake test automatically runs on existing native CPU CI for
Apple ARM64, Linux x64 and Windows MSVC x64. GPU gates run locally on the Mac.
No optional band parallelism is introduced in Stage A/B. Encoder sequence
state and framing order remain sequential.

## Stage B: equivalent word writer

`detail::EntropyBits` accepts unsigned 1–32-bit codewords; the actual Table17
maximum including sign is 26. Between calls the pending count is 0–31, so
appending at most 32 bits requires at most 63 unsigned bits. A completed word
is emitted byte-by-byte in explicit big-endian order. Finish pads with zeros
to a 32-bit boundary and is idempotent. Bounds and codeword checks precede
cache mutation. Failed band allocations/packing remain local; the existing
encoder commits sequence state only after the completed packet exists.

A band reserves at most `min(capacity, 4*(active_coefficients+1))` bytes once.
The 26-bit maximum plus end marker and word padding fit in that bound, even
for all-nonzero int16 extrema. No output-vector growth is needed. This trades
more reserved virtual capacity for eliminating repeated growth/copies; it does
not zero-fill or emit that reserved space. The cubic table uses the original
integer operations and tie policy, now evaluated at compile time. Immutable
constant initialization has no session lifetime, lock or first-use race.
The run-code table and its descending order are unchanged.

The 30-pair microbenchmark `benchmark_entropy.cpp` uses a 960×540 band, two
warmup pairs, alternating legacy/fast order and complete byte equality outside
the timer. It covers zero, dense extrema, alternating zero/nonzero, dense random
and one nonzero per 321 samples. `writer-only-microbenchmark.json` precedes the
empty-run guard, `final-microbenchmark.json` includes it. In this diagnostic,
skipping empty RLE calls reduced dense-extrema median from 3.055 to 2.076 ms
and dense-random median from 3.453 to 2.570 ms. Zero/sparse remain approximately
0.44/0.24 ms and alternating approximately 1.92 ms. Production ABBA results
are reported separately. The old oracle does not use either optimization.

No workers or shared mutable caches are introduced; TSAN/thread-pool gates
for optional Stage C do not apply. Stage C is deferred to separate review;
this PR does not parallelize rate control or omit coefficient integrity work.

## Corpus and long sequence verification

Both NAS ZIP files were freshly read and SHA256 checked; see `nas-sha256.txt`.
The complete suites validate each actual input/artifact in the extracted
16/120-frame corpora. The Metal auditor compares intermediate planes against
scalar CPU, and actual SDK payloads/coefficients/packets against the originals.
`raw-sequence.json` additionally proves **all 120 raw SDK packets** identical
with synchronized metadata, directly comparing Metal to CPU too. Tier A's
raw synchronized 16-frame comparison is in `tier-a.json`. Q2 fixture identities
remain covered by the unchanged existing regression gates.

`stress_sequence.cpp` independently encodes 1001 actual 1080p frames in each
of a CPU and a required-Metal encoder, with noise every 127 frames, flat input
otherwise, changing frame index and resets at frames 500 and 800. Every full
packet, all 30 coefficient hashes, quant/scale tables compare directly. This
is correctness stress, excluded from performance trials. See `stress-sequence.json`.
The core still links only libc++/libSystem in the native CPU-only configuration.

A first hosted MSVC build exposed a missing explicit `<string>` include in
the new test, corrected before final CI. Production MSVC compilation passed
on that first run; no codec algorithm was changed for the include fix.

`allocation_entropy.cpp` overrides ordinary allocation only in its own
standalone diagnostic process. Linking that identical harness to frozen P2
and current P3 libraries counts actual output-vector allocations. For a dense
960×540 extrema band they drop from **22** (4,194,303 cumulative allocated
bytes, 2,097,152 final capacity) to **1** (2,073,604 bytes/capacity); emitted
bytes remain 1,684,804. For a zero band, they drop from **13** to **1**, but
reserved capacity grows from 4,096 to 2,073,604 bytes for only 2,636 emitted
bytes. This is a real capacity tradeoff; process peak RSS is reported below,
separately from reserved capacity. The allocation counter is never linked
into the codec or production timing benchmark.

## Final native results (2026-10-16)

Apple M2 Pro (6P+4E, 16 GPU cores, 16 GiB), native ARM64, Rosetta off,
macOS 26.5.2 (25F84), AppleClang 21, Release `-O3 -DNDEBUG`, AC attached.
No recorded thermal/performance warning; temperature, clock and energy
telemetry were not measured. Fine-grained CPU/Metal Instruments sampling was
not captured; split-pass diagnostic profiling is described above.

| Local correctness gate | Passed | Elapsed |
| --- | ---: | ---: |
| Metal Release + actual NAS + FFmpeg | 36/36 | 165.87 s |
| CPU-only Release + FFmpeg | 27/27 | 77.20 s |
| Metal Debug + API/GPU validation + actual NAS | 36/36 | 866.97 s |
| Metal C++/ObjC++ ASan/UBSan + actual NAS | 36/36 | 511.03 s |

Zero skips and no sanitizer findings. The test-only explicit `<string>`
include was subsequently rebuilt and its oracle test rerun in Release,
Debug and sanitized configurations; the CPU-only full suite was rerun too.
The encoder algorithms did not change after those suites. Correctness jobs
overlapped, so elapsed times are not performance comparisons. LeakSanitizer
is unsupported on this Apple ARM64 setup (`detect_leaks=0`). A separate
native `leaks --atExit -- build/test_entropy_bitexact` check reports **0 leaks /
0 leaked bytes**; see `logs/leaks.log`.

Hosted standalone CI on source `43bba9fa6be191f2cc83f593567e7c1f1b475d40`
passes on Windows MSVC x64, Linux x64, Apple ARM64 Release and ASan/UBSan;
see [run 38842736373](https://github.com/louis-0hrs/HyperCine/actions/runs/38842736373).
The separate original SDK Windows CPU CI also passes. Native GPU gates
are local; hosted CPU runners do not validate Metal.

Performance processes ran sequentially **after all builds and correctness
suites ended**. The unchanged benchmark executable alternates CPU/Metal order
within each process. Complete P2/P3 processes run ABBA/ABBA, bracketed by
fresh frozen-P1 processes. Each fixed-pattern row contains **120 warm pairs
per P2/P3 implementation** (four processes × 30), after two initial frames.
Stateful rows preserve all 16/120 frames including the cold first frame.
Every process matches all 232 initial P2 input/state/complete-packet identities;
P1 matches too. No masking or disabled coefficient hashes. The initial
pre-production-edit `p2-before.json` establishes identity and is excluded
from the final performance statistics.

| 1080p workload | P1 median ms | P2 median ms | P3 median ms | P3 reduction vs P2 | P3 fps |
| --- | ---: | ---: | ---: | ---: | ---: |
| flat | 28.385 | 20.153 | 19.729 | 2.10% | 50.66 |
| gradient | 30.785 | 22.557 | 21.484 | 4.76% | 46.61 |
| noise | 92.316 | 83.410 | 38.671 | 53.64% | 25.81 |
| 16-frame-stateful | 30.068 | 22.170 | 21.033 | 5.13% | 41.84 |
| 120-frame-stateful | 30.786 | 22.521 | 21.560 | 4.27% | 39.93 |

FPS uses total encoded frames divided by total measured time, rather than
the reciprocal of median latency. Full average/min/max/p95 latency,
megapixels/s, sample sizes, cold first-frame latency and per-process medians
are in `benchmark-summary.json`; input/output SHA and stage timers are in
the accompanying raw files. `benchmark-provenance.json` records exact order,
timestamps and executable hashes. Recompute with:

```sh
python3 docs/validation/metal-p3/summarize.py
```

Noise p95 falls from **84.323 to 39.253 ms**, average entropy from
**65.283 to 20.532 ms** (68.55% reduction), without removing quantization,
coefficient FNV hashing, metadata or packet assembly. The remaining entropy
cost is reported, not claimed eliminated. No workload's median/p95 regresses
in this sample. There is no claim for other hardware, resolutions, codecs
or SDK threading.

P2 process peak RSS ranges **105,578,496–105,971,712 bytes**; P3 ranges
**105,496,576–109,690,880 bytes**. One P3 process has a roughly 3.5% higher
peak than the largest P2 observation. These are complete CPU+Metal benchmark
process maxima, not isolated entropy allocations or dedicated GPU VRAM.
The greater zero-band reserved capacity is explicitly documented above.

The required `benchmark_codec` runs are compared against
`benchmarks/results/reference_windows_msvc.json` in the machine-readable
report: **NOT_COMPARABLE** across hosts, input generators, SDK implementation
and thread contracts. That recorded Windows 1080p reference has 77.6958 fps,
12.5634 ms average and 96,985,088-byte peak RSS. It supplies context only;
no cross-host speedup/regression claim is made.

Artifacts include `tested-source-files.json`, both archive SHA values, raw
synchronized 16/120-frame SDK packet comparisons, all native CTest logs,
CPU/Metal 1001-frame stress, FFmpeg p10 quality gates and hosted CI records.
`upstream/`, GPU backends, public APIs, Quality-State math and frame ordering
remain untouched. No automatic merge, issue closure or release promotion.
