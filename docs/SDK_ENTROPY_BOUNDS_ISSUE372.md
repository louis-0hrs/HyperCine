<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under the MIT License or Apache License, Version 2.0.
See LICENSE-MIT and LICENSE-APACHE for details. -->

# SDK entropy wrapper bounds — issue #372

This correction addresses audit finding A08 in the host-side wrappers around the
frozen GoPro SDK. It changes admission of invalid requests and exposes a CPU-only
test target for the actual wrappers. It does not modify `upstream/`, codec tables,
quantization, bit packing or GPU kernels.

## API and supported relation

`EncodeOneExternalBand` and `MatchUpstreamHighpass` now require an explicit
`channel_count` immediately after the transform pointer array. Every repository
caller passes its actual channel extent; the asynchronous Phase 2A worker retains
that extent in its pool. There is no count-less overload or implicit default of
three. This is a source API change: external users of these experimental C++
wrappers must pass the extent of their live array.

The validated layout is a prepared **spatial GOP1** pyramid. Each wavelet has a
unique level, and its declared count fits the actual SDK arrays. The top LL band
is lowpass/subband zero. Highpass subbands follow the existing descending-wavelet,
LH/HL/HH enumeration; the total must equal the prepared encoder's subband count.
Temporal/field/GOP2 layouts are rejected. Their different band relationships have
not been qualified for these external-buffer wrappers.

For this spatial layout, the prepared relationship is
`num_levels == num_wavelets == num_spatial + 1`. The wrapper bounds `num_spatial`
against the physical wavelet/prescale arrays and level limit before doing that
addition or reaching the SDK's subband arithmetic.

Both entry points use `src/cpu/entropy_sdk_guard.h` before reading selected array
entries or invoking SDK selectors. Checks cover negative and oversized channel,
level, band and subband values; physical and declared array counts; missing or
ambiguous wavelets; lowpass/highpass relationships; matching positive geometry;
pitch and representability of the wrapper's signed SDK sizes and bit offsets.
Enumeration also validates the structure before traversing it. Nearby host
scheduler, refresh and assembly paths reject invalid transform structure before
using their nested arrays.

`RefreshUpstreamPyramid` has a separate, narrower conversion contract: exactly
three transforms and three YUV frame planes, each with a present `band[0]` buffer.
The encoder's spatial count must match every prepared transform before conversion
or quantization starts. This prevents `ComputeGroupTransformQuant` from replacing
a checked transform count with an unchecked encoder count. It does not change the
external-band APIs' support for an actual one-entry pointer array.

`SetCodingFlags` in the frozen SDK only checks the upper bound of its subband
override array. The wrapper now checks the lower bound and the prepared subband
count first, then validates the selected codebook against `CODEC_NUM_CODESETS`.
`LoadEntropyTables` and external-band encoding share this checked selection.

Before copying table entries, the wrapper requires the original generated
allocation contract: `VALBOOK.length == VALUE_TABLE_LENGTH`,
`VALBOOK.size == VALUE_TABLE_SIZE`, and the runbook length equals
`NEW_CODEBOOK_LENGTH`. The latter defaults to 3072 in frozen
`upstream/Codec/codebooks.c`; the wrapper preserves the same explicit build-macro
override. A separately built SDK with incompatible table extents is rejected.
An invalid table request clears prior output instead of leaving stale tables.

The caller still owns pointer validity, lifetime and synchronization. It must
provide the actual array extent and a coefficient buffer covering the declared
dimensions. These checks cannot infer allocated storage from an arbitrary raw
pointer. They do not make concurrent mutation of the same SDK objects safe or
validate every internal coefficient/codebook value in a corrupted SDK instance.

## Real SDK test target

`hypercine_entropy_cpu` compiles the actual `entropy_parallel.cpp` and
`entropy_stages.cpp` implementation once and links the pristine `CFHDCodecStatic`
archive. CUDA consumers use the same host library. The `entropy_sdk_bounds` test
is available whenever the SDK target is built, including CUDA-disabled builds.
SDK-disabled builds report the omitted test.
Manual static-archive consumers must also link `hypercine_entropy_cpu`; CMake
consumers of `hypercine_wavelet` inherit that dependency through its PUBLIC link.

The test opens, prepares and encodes a deterministic 256×256 BGRA frame through
the real SDK, refreshes its actual pyramid, and checks every one of the 30 bands.
It compares wrapper fragments with direct `EncodeLowPassBand` or
`EncodeQuantizedBand` results. Highpass staging is compared with that same direct
SDK result. Additional positive cases cover an actual one-entry transform array,
int8/int16 transport equality and the legal peaks fallback. Positives run again
after the negative cases to detect a poisoned encoder.

Negative controls cover invalid indices, a short transform array, malformed
counts and geometry, mismatched band relationships, unsupported temporal layout,
invalid codebook overrides, missing tables and incorrect table extents including
`INT_MAX`. They require rejection and empty/unsuccessful output without changing
the prepared encoder or wavelet objects. All assertions use explicit checks and
remain active under `NDEBUG`.

Refresh-specific negatives also preserve `FRAME`, all three plane objects, and
the bytes in the original frame, wavelet, input and scratch allocations. These
views are recorded before metadata is damaged. The negative refresh calls use
changed RGB pixels, so a conversion performed before a later rejection would
change the retained plane data and fail the preservation check. A valid refresh
and the complete band comparisons run again after these controls.

Portable reproduction (with the normal SDK development dependencies, including
`uuid-dev` on Debian-family Linux systems):

```sh
cmake -S . -B build-sdk-bounds -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_UPSTREAM=ON -DHYPERCINE_BUILD_STANDALONE=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_TESTS=ON \
  -DHYPERCINE_BUILD_BENCHMARKS=OFF
cmake --build build-sdk-bounds --target test_entropy_sdk_bounds --parallel
ctest --test-dir build-sdk-bounds -R '^entropy_sdk_bounds$' --output-on-failure
```

## Qualification boundary

The scoped Linux x86-64 GCC 14.2 Release test passed with **133 negative controls
and 69 positive comparisons**. The complete available SDK CTest inventory was
then executed on the unchanged 15 implementation/build/test files: **61 passed,
zero failed, one explicitly skipped**, out of 62 registered tests (60.27 seconds).
The skipped `ffmpeg_oracle_qualification` rejects this environment's FFmpeg 7.1.5
as unqualified; its measured list contains n8.0 and 9.0.2. CTest exits zero, but
this is **not** an all-pass oracle or release qualification. The decoder allowlist
and gate behavior were not changed. Two initial nested-configure failures were
resolved by propagating the genuine local uuid development prefix, then rerunning
the entire inventory with unchanged repository source.

[Source-bound full-suite evidence](validation/SDK_ENTROPY_BOUNDS_372_20261112.json)
records the explicit blocker, prior attempt, environment and focused controls.
The combined integration has separate source-bound SDK and standalone receipts.

Focused ASan/UBSan instruments the actual wrapper translation units and test while
linking the unchanged SDK archive. This is wrapper coverage, not whole-SDK
sanitizer qualification. LeakSanitizer cannot operate under this environment's
process tracing, so that scoped run explicitly disables leak detection; it makes
no leak-freedom claim.

The updated CUDA caller files still require compilation with a real CUDA toolkit
and native pipeline regression tests. No CUDA toolkit/device or native Windows,
Metal or Vulkan qualification is implied by these Linux CPU results. The test
does not establish full-frame decoder pixel correctness or a new codec profile.
