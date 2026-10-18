<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Five macOS pull request reviews, 2026-10-18

The exact source heads and measured checks are recorded in `report.json`.
These are five scoped review tasks from issues #46, #45, #23, #65 and #7.
The parent codec milestones remain open.

| PR | Independent review result |
| --- | --- |
| [#133](https://github.com/louis-0hrs/HyperCine/pull/133) | No actionable finding in strict integer parsing. Fresh native quality-only CTest 3/3, including the actual probe process and all six profiles. |
| [#135](https://github.com/louis-0hrs/HyperCine/pull/135) | No actionable finding in the stated contract/model scope. Fresh native contract CTest 5/5, including the optional P7 fault injection. The 42-buffer accounting, padded input length, plane strides and temporary workspace overlap match the current Metal source. This remains an allocation estimate; no 4K capability acceptance. |
| [#139](https://github.com/louis-0hrs/HyperCine/pull/139) | P2 finding: p10 comparisons check similarity without validating 0..1023 samples. Two identical invalid outputs can have zero error; 1024 versus 1023 can also pass the quality threshold. Six optimized-process negative subcases failed before this child fix. Both original-decoder replay and autonomous FilmScan3 now validate actual and reference pixels before comparison. |
| [#140](https://github.com/louis-0hrs/HyperCine/pull/140) | Eleven native runner methods pass. The previously reported CMake boolean defect remains a required child integration from #141; this parent alone is not recommended for acceptance. Required inventory, native target and stale-evidence handling otherwise match their stated scope. |
| [#141](https://github.com/louis-0hrs/HyperCine/pull/141) | Twelve native runner methods pass, including comparison with actual CMake truth evaluation. Mandatory Metal registrations are required for enabled non-ON spellings. Fresh native full Metal CTest 34/34 in 112.79 seconds with TRUE, including all four GPU-required tests and independent FFmpeg decode. |

## Child fix

A shared p10 reader requires exactly `width * height * 4` bytes for planar
YUV422 p10 little endian and validates every actual/reference sample. Perfect
valid equality retains `null` PSNR; the existing >35 dB gate is unchanged.
Regression cases cover every position of a small three-plane frame, both
comparison sides, identical invalid pixels, empty/truncated/extra storage,
little endian values and poor quality. All 14 validation methods pass with
Python optimization enabled. The complete native child standalone suite passes
29/29 in 81.83 seconds, with external FFmpeg decode and 16/120-frame manifest
replay. Thirteen privacy scanner regression methods also pass.

## Reproduce

Check out the exact reviewed head for each review. All paths below are relative
to that checkout; configure external tools locally without publishing their paths.

```sh
# #133, quality-only mode
cmake -S . -B build-quality -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DHYPERCINE_BUILD_QUALITY_STATE_ONLY=ON \
  -DHYPERCINE_BUILD_UPSTREAM=OFF -DHYPERCINE_ENABLE_CUDA=OFF
cmake --build build-quality --parallel 4
ctest --test-dir build-quality -C Release --output-on-failure

# #135, optional P7 source supplied through a local variable
cmake -S tests/macos_contracts -B build-contracts \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DHYPERCINE_P7_AUDIT_ROOT="$HYPERCINE_P7_AUDIT_ROOT"
cmake --build build-contracts --parallel 4
ctest --test-dir build-contracts -C Release --output-on-failure

# #140 and #141 at their own source heads
python3 ci/test_standalone_gate.py

# This child, actual standalone encode/decode and the 16/120-frame manifests
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_BENCHMARKS=OFF \
  -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --parallel 4
PYTHONOPTIMIZE=1 python3 ci/run_standalone.py --build-dir build \
  --system Darwin --machine arm64
```

For native #141 Metal qualification, use the same standalone configuration
with `-DHYPERCINE_ENABLE_METAL=TRUE` at its exact head and run its complete
runner. CPU fallback cannot satisfy its required GPU tests.

No private corpus, generated build tree or raw host log is part of this PR.
No upstream source or existing owner worktree was changed.
