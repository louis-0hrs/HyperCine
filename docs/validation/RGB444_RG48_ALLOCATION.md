<!-- Copyright (c) 2026 HyperCine Contributors. -->
<!-- Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# RG48 importer allocation-failure qualification (#37)

## Scope

This continues the source-import slice in Draft #142. The existing planar-only
allocation children #167/#176 remain separate. The importer and allocator are
unchanged: a new isolated executable compiles the actual
`src/cpu/rgb444_frontend.cpp` and intercepts its aligned allocation calls.
Ordinary allocation is untouched; no production fault hook is added.

For each of the three destination-plane allocation ordinals, the test requires
an actual `bad_alloc`, exactly the expected allocation attempts and partial
frees, and the original frame's three addresses, dimensions, pitch, samples
and zero padding to remain unchanged. Each failure is followed by a successful
retry with different dimensions and independently calculated R/G/B values.
The padded source includes an exact final readable row; both inputs must remain
unchanged. All tracked aligned allocations must be freed after each scenario.
Four malformed views must reject before the first allocation, even with that
allocation armed to fail. Assertions remain active in Release builds.

`rgb444_rg48_allocation_failure` is registered in the normal standalone CTest
suite and required CI inventory. The CI helper rejects omission of this case.
Tests OFF creates neither the executable nor a CTest registration.

Current-main scheduler #219 is carried through a normal merge from main
`cabdf7097e50d3d666acefe4d7a1a3866b33bc8e`, preserving its five files verbatim.
This is regression coverage, not a new scheduler or original-SDK qualification.

## Reproduction

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_ENABLE_METAL=OFF \
  -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=OFF \
  -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=OFF
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
python3 ci/test_standalone_gate.py
python3 -m unittest discover -s ci -p 'test_privacy*.py' -v
```

Use a second build directory with
`-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"`
and `-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"` for sanitizer
validation. Run CTest with `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. Python is not instrumented.
The aligned-allocation counter checks this executable's cleanup; it does not
establish a full-process leak check.

Hosted standalone CI requires external FFmpeg/ffprobe decode. For that full
gate, set `HYPERCINE_REQUIRE_EXTERNAL_DECODE=ON` and run
`python3 ci/run_standalone.py --build-dir build --system Darwin --machine arm64`
on native macOS ARM64 (or the corresponding runner OS/architecture).

## Executed results (2026-10-23)

Native macOS 26.5.2 ARM64, AppleClang 21.0.0, CMake 3.31.5 and Python 3.9.6,
using Unix Makefiles:

- Clean SDK-free Release build and complete registered CTest: **42/42 PASS**,
  zero skips, 23.33 s.
- Separate clean ASan/UBSan build and complete registered CTest: **42/42 PASS**,
  zero skips, 76.95 s. All 26 C++ compilation entries carry the sanitizer flags;
  ASan runtime linkage verified. Leak detection disabled.
- Standalone CI helper: **11/11 PASS**, including missing-allocation-test
  rejection. Privacy discovery: **158/158 PASS**, zero skips, 88.986 s.
- New executable also builds with `-Wall -Wextra -Werror`. Actual allocation
  failure coverage: three injected failures and retries; four preallocation
  rejections; zero remaining tracked aligned allocations.
- Two separately compiled scratch mutations are detected with exit 1:
  removing the packed-view `ValidatePlane` call makes the armed allocator
  run before malformed-input rejection; replacing green samples with zero
  violates the independent component comparison. Tracked production source
  is untouched by these controls.
- Tests OFF: zero CTest registrations; allocation-test target absent.

Ten external-decode tests are **NOT_RUN / not registered** because FFmpeg and
ffprobe are unavailable locally. Actual private F1 source replay is also
**NOT_RUN**: the configured corpus root is not readable. Historical F1 results
in the earlier source-import report are not claimed as a current-head replay.
Hosted exact-head CI results must be checked separately on Draft #142.

Source fingerprints (SHA-256):

```text
src/cpu/rgb444_frontend.cpp
04575b46adf087fa900313f4133df77cbaf79b9e8fd1072aabe81acfb8bae3b4
tests/test_rgb444_rg48_allocation.cpp
32cc5a6ea97dcb013c36388a3d3dc65021c56fbab3eb06924b6d2c6447ec35c1
```

## Proof limits

This test qualifies a source API's failure/recovery behavior. No RGB encoder
packet is emitted and no signed internal SDK stage is available. Full RGB
bitstream equality, native Windows/Linux execution, GPU paths and performance
remain outside this local test result. `upstream/` and existing corpus pins are
unchanged. Issue #37 remains open for its actual encoder/reference gates.
