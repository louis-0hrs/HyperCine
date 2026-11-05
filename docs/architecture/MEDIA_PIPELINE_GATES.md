<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Media pipeline admission and regression gates

Proposed P0 acceptance contract for issue #34. See the
[ADR](MEDIA_ENGINE_ADR.md) and [capability matrix](BACKEND_CAPABILITY_MATRIX.md).
These gates describe required evidence; this document does not assert that every
backend/profile has passed them. A gate is PASS, FAIL, PENDING, or NOT_APPLICABLE
with an explicit reason and reviewer. Missing hardware or corpus means PENDING.

## A. CineForm integer and packet correctness

| Gate | Required check | Evidence / current entry points |
| --- | --- | --- |
| A1 input contract | Deterministic seed, exact input SHA256, geometry, stride/orientation, color/range, packing, profile, GOP, quality and metadata | Test generators and `tests/validate_windows_reference.py`; local external corpora configured through `HYPERCINE_GOLDEN_DIR` |
| A2 stage parity | Exact signed int16 color and wavelet results; explicit rounding, prescale, overflow/saturation, quant and scale tables | `test_standalone`, `test_filmscan3`, `hypercine_reference_probe`, `tests/test_reference_sample.py`; compare all 30 coefficient and payload band hashes where the profile defines them |
| A3 authentic oracle | Same bytes and parameters through pristine GoPro reference; original signed intermediate dumps when available | #23 and #36; matching a wrapper-generated expectation alone does not complete the upstream stage oracle |
| A4 complete samples | Compare raw packet SHA256 after synchronizing variable metadata; separately document every permitted normalization field and compare all remaining bytes | `tests/validate_filmscan3.py`, `tests/compare_sdk_oracle.py`; normalization cannot remove quant, band sizes, entropy or sequence effects |
| A5 temporal state | Authentic Tier A 16-frame and Tier C 120-frame sequences; per-frame input, state before/after, previous sample size and all hashes | FilmScan3 manifests plus actual external corpus tests; manifest checks are distinct from full raw-packet replay |
| A6 compatibility | Independent FFmpeg decode of the exact emitted packets; validate output frame count, dimensions, format and pixel content, not just process success | `filmscan3_external_decode`, `standalone_external_decode`; pristine SDK decode and application import gates remain separate |
| A7 profile expansion | Six qualities, RGB444 p12, RGBA4444 p12, Bayer, stereo, interlaced/FIELDPLUS/GOP2 each have explicit fixtures and oracle coverage | #29/#36 and decoder issues; a YUV422 FilmScan3 pass grants no other profile |

Bit-exact modes require zero mismatches. Lossless modes require mathematical
pixel equality. Lossy FilmScan3 round trips require PSNR >35 dB under a documented
pixel-domain metric in addition to packet/stage checks; high PSNR cannot excuse
a declared bit-exact mismatch. Record clipping, alpha and metadata policy.
Cross-platform comparisons retain canonical serialization/endian conventions.

The scalar CPU implementation is the execution reference for compute adapters.
Its authenticity is a separate GoPro oracle gate; CPU/GPU agreement cannot prove
that both implement the standard correctly. Do not use golden tables or a sample
header as runtime encoder input. Record source revision, fixture hash and oracle
provenance for every claimed pass.

## B. Media ASIC / software adapter conformance

For NVENC, AMF, VPL, VideoToolbox or another future adapter, record the requested
codec/profile/level, chroma, precision, range, transfer, frame ordering, GOP,
rate-control and metadata policy. Verify the **actual** output stream, decode it
independently, count every frame, check timestamps and inspect color/alpha/HDR
metadata. Test unsupported tuples and resource exhaustion before admission.

Do not require identical H.264/HEVC/AV1 encoder bytes across vendors. Use declared
quality/bitrate targets, independent decode, deterministic inputs and latency/
throughput measurement under equivalent presets and memory contracts. CFHD
compute parity remains governed by gate A. Legal distribution review in the ADR
is an additional adapter admission requirement.

## C. Memory, scheduling and recovery

| Case | Required behavior |
| --- | --- |
| Bounds | Reject null/incomplete planes, undersized or misaligned pitch, invalid bit packing, checked arithmetic overflow and unsupported geometry before allocation/dispatch |
| Layout | Exercise padded rows, bottom-up/top-down, Y/Cr/Cb versus Y/Cb/Cr, p10/p12 meaningful bits, endian order, full/studio range and chroma siting with known vectors |
| Ownership | Release caller handles immediately after submit, retain backing storage until completion; prohibit input mutation/output reuse while leased |
| Ordering | Cross-queue dependencies, concurrent independent sessions, cancellation, shutdown and pool reuse preserve content and do not release in-flight memory |
| Interop | Test matching and mismatching devices, incompatible layout, failed import, fence transfer and host visibility; reject required-zero-copy if a staging copy is needed |
| Failure | Inject initialization, allocation and post-dispatch failures; no partial output or quality-state commit; required-GPU never falls back silently |
| Device loss | Invalidate device-generation capabilities and pools; fail pending frames, recreate explicitly; no automatic stateful replay |
| Budget | Check per-surface, workspace and sample bounds separately; record peak RAM/device memory and fail safely without unbounded allocations |

Run ASan/UBSan for portable CPU paths and relevant GPU tooling on actual devices.
CUDA qualification includes genuine device instrumentation and required
memcheck/racecheck/initcheck/synccheck coverage as applicable, with padding and
scratch initialization checked. A host-only sanitizer build cannot certify CUDA
kernels. Metal validation and Vulkan validation layers supplement content and
lifetime tests; a clean API log alone cannot prove mathematical correctness.
Existing Linux/CUDA safety holds (#13/#38) remain holds until their own evidence
passes. Do not disable instrumentation to obtain a green result.

## D. End-to-end performance and provenance

Before any optimization claim or acceleration milestone, run `benchmark_codec`
and compare with `benchmarks/results/reference_windows_msvc.json`. If the build
cannot run that benchmark or the hardware/workload differs, record PENDING or
non-comparable evidence; do not infer a speedup from unrelated measurements.
This documentation-only P0 makes no performance claim.

Record FPS, megapixels/s, actual per-frame latency (average/min/max and preferably
median/p95/p99), initialization/cold-start, peak host/device memory, source/binary
hashes, compiler/flags, OS, driver/runtime, stage backend, queue/stream count and
fixture hashes. Warm up each active slot and retain repeated-run samples with
variance. Stateful runs must preserve sequence order and rate state.

State the timing boundary: host allocation, staging/pinning, H2D/upload, color,
wavelets, readback/D2H, CPU quant/entropy, sample writer, command submission and
completion waits. Report host-pageable end-to-end, pinned-host, device-resident
and kernel-only measurements as different classes. Account for copies on unified
memory too. Exclude I/O only when explicitly documented. Never label throughput
reciprocal as measured single-frame latency. No dropped frames or implicit CPU
fallback in a GPU-required measurement. Explain regressions before promotion.

A public evidence record identifies the exact operation/capability tuple,
source and fixture hashes, build options, sanitized device class/driver versions,
gate states, mismatch count, measurements and reproduction commands. Keep raw
logs, private corpora and machine identifiers outside Git. Publish no internal
paths or network locations. Run `python3 ci/privacy_guard.py --staged`, inspect
the staged diff, and scan **every outgoing commit** before push.

## Reproduction and review boundary

The existing SDK-free CPU suite can be run from repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_ENABLE_METAL=OFF \
  -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=OFF \
  -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
```

This requires CMake 3.25+, a C++17 compiler, Python 3 and FFmpeg/ffprobe on PATH.
External corpus tests are optional registrations in the baseline CMake file;
absence of those tests is not a corpus PASS. Configure the existing corpus cache
variables locally from `HYPERCINE_GOLDEN_DIR`; publish neither its value nor raw
CMake cache/logs. Required-GPU runs need their own backend build and real hardware.

ADR acceptance permits only the next reviewed CPU-only interface PoC. It does
not promote a decoder, widen geometry/profile support, clear a safety hold or
claim zero-copy. Review the ownership/session model, request-based capability
selection and licensing boundaries before adding adapters. Do not close issue
#34 until the maintainer accepts P0 and assigns the separate implementation work.

## P0 document validation (2026-10-16)

The documentation branch ran the Release SDK-free CPU build and the full
registered CTest suite: **17/17 passed** (16.74 seconds). FFmpeg/ffprobe were
unavailable, so this local run used `HYPERCINE_REQUIRE_EXTERNAL_DECODE=OFF` (the
baseline default); external decode tests were not registered and remain PENDING.
External corpus replay and GPU qualification were not performed. The stricter
reproduction command above intentionally requires the missing decode tools.
All 11 privacy-guard unit tests passed. Relative document links and code fences
were checked. Changes are restricted to these three architecture documents;
no codec behavior or performance result is changed.
