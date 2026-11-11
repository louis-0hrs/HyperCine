<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# CPU API boundary audit and validation — 2026-11-11

Issue: [#362](https://github.com/louis-0hrs/HyperCine/issues/362).

The direct scalar CPU wavelet entry points now reject overflowing dimensions
and strides before allocation or buffer access. Their 2D wrappers translate
scratch allocation failures into the existing `HYPERCINE_STATUS_OUT_OF_MEMORY`
status. The public main header also compiles and links as the first include in
both C11 and C++17 translation units at `-O0`.

This is a bounded public CPU API correction. The filter arithmetic, codec
packets, frozen `upstream/`, direct NEON implementations and GPU implementations
are unchanged. Native Windows, ARM64, CUDA, Metal, original SDK and private
corpus qualification are not established by this run.

## Confirmed defects and correction

The audit baseline was main
`dca0c0707a0d460fb8cb7ca7d198c0a7aa4be8e7`.

| Defect reproduced on the baseline | Correction |
| --- | --- |
| `HyperCine_InverseHorizontal26_CPU` evaluated `2 * width` inside its stride guard. Passing `INT_MAX` produced a UBSan signed-overflow diagnostic before any sample access. | Refuse dimensions above the representable doubled range before multiplication. |
| `HyperCine_InverseSpatial2D_CPU` doubled the subband width and height before checking them. `subband_height = INT_MAX` produced the same UBSan error. | Check both dimensions before deriving reconstructed geometry. |
| Direct forward/inverse CPU functions used signed `row * stride` expressions without validating the complete active extent. The internal SIMD dispatcher already had a guard, but direct public calls bypassed it. | Apply a shared scalar extent predicate to every input and output plane of all six public CPU functions. |
| A 6x6 forward 2D call with invalid `src_stride = 0` attempted scratch allocation first; injected allocation failure escaped as `std::bad_alloc`. | Validate every stride before allocation. Translate `std::bad_alloc` and implementation-specific vector size refusal to `OUT_OF_MEMORY`, retaining RAII cleanup. |
| Including `hypercine.h` first failed because the frozen allocator header required `size_t`. With that include dependency supplied externally, C11 `-O0` linking still failed for `HyperCine_GetVersionString`. | Include `<stddef.h>` in the HyperCine wrapper and make the header-only version function `static inline`. No upstream edit. |

The extent predicate checks the last active sample against both signed-int
indexing and the platform pointer-distance bound. It does not require unused
padding after the final row. As before, callers must supply real storage of the
declared extent; a raw-pointer API cannot determine an allocation's actual size.

The regressions require rejection before any output store or scratch
allocation. Allocation-failure tests independently fail the first and second
scratch allocations of each 2D wrapper, verify unchanged caller storage and
zero retained scratch allocations, then retry successfully. Successful padded
transforms retain independent constant-signal expected values and untouched
padding. No codec failure-injection state was added.

## Execution evidence

Executed locally on Linux x86_64 with GCC 14.2.0, CMake 3.31.6, Python 3.12.14
and FFmpeg 7.1.5. These are local execution receipts, not GitHub Actions checks.

| Check | Actual result |
| --- | --- |
| Release standalone build, upstream/CUDA/Metal/Vulkan disabled | PASS |
| Strict full standalone runner with external decode required | **128 registered, 128 PASS, 0 FAIL, 0 SKIP** |
| Direct CPU boundary regression | **116 rejected calls**, six valid padded transforms, four injected scratch allocation failures with cleanup and successful retry |
| Focused AddressSanitizer + UndefinedBehaviorSanitizer | PASS on the same regression |
| Standalone inventory guard unit suite | **32/32 PASS**, including removal of the new mandatory registration |
| C11 first-include/header-only version compile, link and run at `-O0` | PASS |
| C++17 first-include/header-only version compile, link and run at `-O0` | PASS |
| Existing 4K external geometry test inside the full suite | 19 independent FFmpeg p10 decodes, 16-frame sequence pixel identity and PSNR gate PASS |
| Frozen upstream working tree and diff checks | Unchanged |

The full suite completed in 222.67 seconds; that is test-run wall time, not a
codec performance measurement. `wavelet_cpu_bounds` is mandatory in
`ci/run_standalone.py`; dropping its CTest registration fails the inventory gate.
The two main-header smoke targets are registered in regular SDK build mode.
They intentionally do not enter the strict standalone compile database, since
the main header exposes frozen SDK types.

LeakSanitizer reported that it cannot operate under the execution environment's
ptrace configuration. The recorded sanitizer pass therefore used
`ASAN_OPTIONS=detect_leaks=0`: address and undefined-behavior checks remained
active, while **LeakSanitizer is NOT RUN**. The focused test's separate live
allocation counter passed all four failure/cleanup cases. This is not a claim
that LeakSanitizer passed.

## Source and receipt binding

The [sanitized execution receipt](cpu-api-boundaries-20261111.json) records the
SHA-256 of all nine changed source/test/build files, exact compiler/tool
versions and every registered CTest result. The test run used the working-tree
candidate on the baseline commit; do not describe the baseline SHA alone as
the patched source. Documentation-only publication does not change these
source fingerprints.

- Receipt SHA-256: `4671ac433a33551eab6934970fe306d4e2a44a382c41910240743cf3a964b1e7`.
- Sanitized full CTest completion-summary SHA-256:
  `fb39815304e7f02068233727c0f389c2ba1a5fb0e5dc47f3a70b591c8086c455`.
- Full raw configure/build/test logs remain outside Git; public evidence
  contains no machine paths, private reference data or environment dumps.

## Portable reproduction

Use available CMake, a native C/C++ compiler, Python, FFmpeg and ffprobe:

```sh
cmake -S . -B build-api-safety \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_ENABLE_METAL=OFF \
  -DHYPERCINE_ENABLE_VULKAN=OFF -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build-api-safety --parallel 2
python3 -B ci/run_standalone.py --build-dir build-api-safety \
  --system Linux --machine x86_64
```

For the small public-header checks, bind `HYPERCINE_AUDIT_DIR` to an existing
private scratch directory and keep compiler output outside Git:

```sh
cc -std=c11 -O0 -I include -I upstream/Common \
  tests/test_public_header.c -o "$HYPERCINE_AUDIT_DIR/header-c"
"$HYPERCINE_AUDIT_DIR/header-c"
c++ -std=c++17 -O0 -I include -I upstream/Common \
  tests/test_public_header.cpp -o "$HYPERCINE_AUDIT_DIR/header-cpp"
"$HYPERCINE_AUDIT_DIR/header-cpp"
c++ -std=c++17 -O1 -g -fsanitize=address,undefined \
  -fno-sanitize-recover=all -fno-omit-frame-pointer -I include \
  tests/test_wavelet_cpu_bounds.cpp src/cpu/wavelet_cpu.cpp \
  -o "$HYPERCINE_AUDIT_DIR/wavelet-api-sanitized"
"$HYPERCINE_AUDIT_DIR/wavelet-api-sanitized"
```

On an unrestricted host the last command should retain LeakSanitizer's default
behavior. The explicit leak-detection limitation above belongs only to this
recorded execution environment. Native Windows and ARM64 acceptance, including
separate review of the direct NEON boundary contract, remain follow-up work.
