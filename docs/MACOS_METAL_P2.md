<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Resident Metal FilmScan3 pyramid — Issue #32

Based on main `8631ef591f6e09f97a7551caeaf1c3d460185f9d` (Metal P1, PR #30).
This optional backend retains the existing 1920×1080 BGRA8 → YUV422 p10,
progressive GOP1 FilmScan3 contract. Other qualities, 4K/8K, decoder changes
and production promotion are outside this PR. Metal remains default OFF.

## Execution and ownership

The internal `Frontend::Transform` boundary returns a complete three-channel
pyramid. Its scalar implementation uses the existing color/forward functions.
The encoder's existing quantizer, rate control, coefficient hash, Table17/VLC,
metadata and writer consume these bands unchanged. The portable public API and
all integer kernels remain unchanged. Core edits are restricted to this stage
adapter and optional host profiling; no Quality-State mathematics is modified.

Each Metal frontend owns a workspace, separately from every other encoder.
For a fixed geometry and input pitch, its 42 shared Metal buffers persist:
input, three color planes, 36 wavelet bands, and two reusable horizontal scratch
buffers. Allocation checks precede command creation. A different valid geometry
or pitch replaces the workspace only after all new allocations succeed; no
previous command remains in flight. Concurrent calls on one stateful encoder
remain unsupported; independent instances own separate buffers/queues.

One input memcpy and **one command-buffer submission/wait** execute color plus
27 wavelet kernel dispatches. LL flows directly into the next level on GPU.
Separate tracked-resource compute encoders order horizontal, vertical and next
level/channel uses of shared scratch. There is no inter-frame overlap, private
memory migration, asynchronous caller lifetime or implicit CPU fallback.

Production reads 30 bands: three L3 LL and 27 highpass planes. L1/L2 LL retain
layout metadata only on the host. The scalar implementation also releases
unneeded LL/color storage after producing each channel. Audit mode downloads
all three colors and all 36 bands, including intermediate LL and padding,
and compares each stored int16 sample against the original scalar functions.
`hypercine_encode_filmscan3_metal` uses that auditor around the **actual resident
path**; it does not substitute the old per-level GPU path. The individual
`Convert`/`Forward` entrypoints remain for signed saturation/border tests.

All buffers start zeroed. Kernels overwrite every active sample on every frame;
untouched pitch padding stays zero, with no uninitialized padding readback.
No host quantization writes into resident GPU storage. 135-row L3 output is
preserved, without another wavelet on that odd-height output.

## Resources and failure behavior

| Per 1080p production frame | P1 | P2 |
| --- | ---: | ---: |
| Kernel dispatches | 28 | 28 |
| Command submissions / waits | 10 / 10 | 1 / 1 |
| Metal buffer allocations | 67 each frame | 42 first frame, 0 after |
| Host→shared copies | 10 | 1 |
| Shared→host copies | 39 | 30 |
| Host→shared copied bytes | 19,180,800 | 8,294,400 |
| Shared→host copied bytes | 19,180,800 | 8,294,400 |
| Peak explicit shared buffers | 16,588,800 | 31,622,400 retained |

P1 counts/bytes are derived from the frozen P1 allocation and copy sites;
P2 counters are asserted for 105 changing sequential frames. P2 trades higher
retained GPU buffer capacity for reuse and fewer synchronizations. These are
unified-memory resources, not a second physically separate VRAM pool.
`MTLDevice.currentAllocatedSize` includes framework/device allocations and is
reported separately from the workspace/call footprint and process peak RSS.
`peak_buffer_bytes` is the largest single workspace or legacy stage call;
geometry-changing audits can temporarily retain an old and new workspace, or
combine resident and legacy stage buffers. Device allocation and process RSS
include those overlaps. The fixed-layout production benchmark has one resident
workspace, so its 31,622,400-byte footprint is also its explicit owned peak.

A command status other than Completed throws before readback/state update.
A deterministic one-shot test hook throws after a real completed GPU command,
before readback. It verifies unchanged frame index, previous sample size and
limiter, followed by exact recovery using the same buffers. It does **not**
claim to induce physical device loss or a genuine driver execution failure.
Required-device initialization/factory failure and opt-in CPU fallback are
separate tests. Invalid input, overflow/budget dimensions, changing geometry,
padded/short last rows, reset/reuse, two independent instances and repeated
resource destruction are covered. macOS arm64 LeakSanitizer is unavailable;
ASan uses `detect_leaks=0`; a separate process-exit `leaks` check is recorded.

## Reference and validation contract

The NAS is read-only. Both original archives were independently rehashed this
run before using local extractions:

- Tier A: `fdc9cd0e881db417e23423c8cd3f9f1e27161f2c946e40edfa2bcb8b56d58dcf`.
- Tier C: `9667d078ca54dccaf4f849f0136a5a8aae674c2c5e603e29b6cfc225aab21b06`.

CTest verifies the actual original SDK/CUDA artifacts and each input SHA before
comparing autonomous limiter/quant/scale state, all 30 quantized coefficient
hashes, payload/framed bands, complete normalized bytes, and raw SDK bytes with
explicitly synchronized variable metadata. Audit and production packets also
compare directly with scalar CPU bytes. Independent FFmpeg decodes real Metal
packets, checking 10-bit geometry and >35 dB p10 quality separately from encoder
hash equality. The fixed-Q2 packet/decode hashes remain regression gates.

The unquantized Windows SDK internal-stage oracle remains pending under #23;
the unquantized Metal oracle is the scalar Mac CPU. No reproduction of the
128px upstream SIMD exception is attempted. Windows CUDA execution, Adobe and
Resolve were not run on this Mac.

## Reproduce

Use the P1 reproduction configuration in `MACOS_METAL_BACKEND.md`, enabling
`HYPERCINE_BUILD_BENCHMARKS=ON` for the new native standalone `benchmark_codec`
target. Use actual extracted Tier A/sequence paths and FFmpeg executables.

```sh
cmake --build build --parallel 4
ctest --test-dir build -C Release --output-on-failure
./build/benchmark_codec > p2-benchmark.json
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  ctest --test-dir build-debug -C Debug --output-on-failure
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build-sanitized -C RelWithDebInfo --output-on-failure
leaks --atExit -- ./build/test_metal_filmscan3
```

For baseline benchmark reproduction, configure and build unchanged P1/main in
its own checkout first. Compile `validation/metal-p2/benchmark-p1.cpp` with
**P1 headers and P1 library** (`-O3 -DNDEBUG -std=c++17 -arch arm64`, root/include
search paths, Metal/Foundation frameworks). New private interfaces must not be
used to compile a harness linked to the old library. The benchmark uses Apple
CommonCrypto for SHA only, outside the core codec, and never includes hashing,
input generation, validation or file I/O in the encode timing.

## Measurement boundaries

Apple M2 Pro / 10 CPU cores (6 performance + 4 efficiency), 16 GPU cores,
16 GB unified memory, native arm64, macOS 26.5.2 (25F84), Clang 21,
macOS SDK 26.5, AC attached. Fine-grained temperature/energy/clock telemetry
was not collected. Warm runs follow cold allocation and one additional warmup.
Each flat/gradient/noise scenario contains 30 alternating CPU/Metal measured
pairs. Separate 16- and 120-frame changing-complexity stateful scenarios start
at state zero; cold first-frame timing is retained, and no state is discarded
or reset between their frames. Input and output SHA are retained per pair.
Each sample proves raw CPU/Metal byte equality outside the timed region.

Timing includes all per-encode GPU allocation, host allocation, upload,
dispatch, synchronization, readback, quantization, coefficient hashing, entropy,
framing and destruction. Pipeline compilation/device creation and fixture
creation are excluded. The CPU path has the same host codec and input/state.
P2 records allocation/zero-fill, upload, readback, submission, wait and complete
GPU command start/end timestamps. Batch GPU execution overlaps CPU wait and
must not be added to it. Color/wavelet per-kernel timestamps are not measured
in the combined P2 command; zero legacy timer fields mean unmeasured, not zero
GPU work. The unchanged P1 reports color and wavelet command durations.

The frozen P1 library exposes aggregate host quant/entropy/framing/orchestration
as a residual of total minus upload/readback/submission/wait; it has no separate
host-entropy timer. P2 host profiling separately times quantization, coefficient hashing, VLC entropy,
and framing/state; scalar conversion and wavelet timing is also recorded.
Residual orchestration/destruction is included in total, outside those named
intervals. Profiling uses steady clock, is optional at the internal frontend
boundary, and does not alter byte generation.

The native `benchmark_codec` is the SDK-free FilmScan3 encoder benchmark for
this configuration; the existing SDK encode/decode benchmark remains unchanged
in SDK builds. The recorded Windows MSVC baseline is explicitly compared as
context in the machine-readable report. Different hardware, generator, thread
and API contracts prevent a cross-host speedup or regression conclusion.
No performance claim beyond the measured 1080p M2-Pro workload is made.

## Final native results (2026-10-16 local)

| Configuration | Complete CTest | Elapsed (correctness runs, not benchmarks) |
| --- | --- | ---: |
| Release, real Metal + original Tier A/Tier C + FFmpeg | **35/35 PASS**, zero skips | 181.20 s |
| Debug, Metal API and GPU validation + original corpora | **35/35 PASS**, zero skips | 921.70 s |
| C++/ObjC++ ASan + UBSan + original corpora | **35/35 PASS**, zero skips, no findings | 528.01 s |
| Native arm64 CPU-only CI gate | **26/26 PASS**, zero skips | 80.84 s |

The Debug and sanitizer correctness runs overlapped; their elapsed times are
not performance comparisons. All four benchmark processes ran sequentially
after those suites finished. The separate native process-exit leak check found
**0 leaks / 0 bytes**. CPU-only linkage is libc++ and libSystem only. All critical
tested source SHA values are frozen in `validation/metal-p2/tested-source-files.json`.
No source algorithm was changed after those final suites.

Actual Windows inventory checks verified **280/280 Tier A files** and
**480/480 Tier C files**; all **16 + 120 frames**, autonomous state, 30 bands,
entropy/framing and normalized packets match the originals. With synchronized
variable metadata their raw SDK packets match too. The resident Metal auditor
compares every color value and unquantized stored band against scalar CPU;
production benchmark packets are independently equal to CPU/P1 packets.
The independent FFmpeg p10 decode/PSNR gates and all existing Q2 goldens pass.

| 1080p workload | P1 median ms | P2 median ms | Reduction vs P1 | P2 throughput fps |
| --- | ---: | ---: | ---: | ---: |
| flat | 28.770 | 20.171 | 29.89% | 49.58 |
| gradient | 31.080 | 22.579 | 27.35% | 44.27 |
| noise | 92.772 | 83.979 | 9.48% | 11.91 |
| 16-frame-stateful | 30.345 | 22.196 | 26.85% | 31.15 |
| 120-frame-stateful | 31.218 | 22.563 | 27.72% | 27.40 |

Medians above are the mean of each process's median. Throughput uses complete
encode durations, rather than the reciprocal of median latency. The changing
sequences mix workload complexities, retain the cold first frame and preserve
all rate-control state; their aggregate throughput is consequently different
from a reciprocal median. Each repeated-pattern row uses **60 warm pairs per implementation**,
from two P2 processes and two bracketing P1 processes (30 warm pairs/process).
All **232 input/state/full-packet SHA cases per process** match across all four
processes; no bitstream fields are masked in this benchmark comparison.
CPU medians remain stable within about 1% across the bracketing runs.

For warm noise, P2 averages approximately **4.94 ms quantization, 10.80 ms
coefficient hashing, 65.62 ms VLC entropy, and 0.31 ms framing/state**.
GPU batch execution averages about 1.05 ms and overlaps roughly 1.75 ms host
waiting; upload and readback average 0.21 and 0.27 ms. Host entropy dominates
this workload. This report identifies a future CPU profiling/optimization target;
it makes no entropy or format change in P2. Production coefficient FNV reporting also has a measurable cost and remains
included. The full scalar stage auditor is excluded from benchmark timing,
as described above.

The fixed-layout counter gate proves 42 first-frame buffer allocations, zero
subsequent Metal buffer allocations, one command/wait/upload and 30 readbacks
per frame. Peak device-reported allocations and RSS are recorded in every raw
run. Process peak RSS is 106,463,232 / 106,332,160 bytes for P1 and
105,578,496 / 105,562,112 bytes for P2; this is not a separate GPU memory pool.

The legacy Windows baseline records 77.6958 encode fps / 12.5634 ms average
latency / 96,985,088-byte memory peak at 1080p. It uses a different CPU, compiler,
SDK implementation, threading and generated input. The summary labels it
**NOT_COMPARABLE**, without claiming a cross-platform speedup or regression.
The demonstrated improvement is only P2 versus frozen P1 on this M2 Pro.

Evidence:
[validation matrix](validation/metal-p2/validation.json),
[benchmark statistics](validation/metal-p2/benchmark-summary.json),
[hardware/binary provenance](validation/metal-p2/benchmark-provenance.json),
[16-frame original corpus](validation/metal-p2/tier-a.json),
[120-frame original corpus](validation/metal-p2/sequence.json),
[independent FFmpeg](validation/metal-p2/ffmpeg.json),
[native logs](validation/metal-p2/logs/).
The raw P1-before/P2-first/P2-second/P1-after JSON files accompany these reports.
Recompute their statistics and validate hashes/counters using:

```sh
python3 benchmarks/summarize_metal_p2.py \
  --p1 docs/validation/metal-p2/p1-before.json docs/validation/metal-p2/p1-after.json \
  --p2 docs/validation/metal-p2/p2-first.json docs/validation/metal-p2/p2-second.json \
  --windows-baseline benchmarks/results/reference_windows_msvc.json \
  --output /tmp/metal-p2-summary.json
```

Hosted CPU CI is recorded on the Draft PR. No automatic merge, issue closure,
4K/8K qualification or production release is performed.
