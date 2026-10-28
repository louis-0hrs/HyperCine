<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Checked C0 integer domain — #48

Child of PR #126, parent 67bd1d835b27f315f669c241c3e4f69b5c1080b8.
The allocation-free model accepted booleans as integer sizes and rounded a
negative aligned size to zero. Floating-point operands could produce fractional
sizes or incidental TypeError exceptions instead of a checked domain failure.

All arithmetic, surface, geometry and chunk model entry points now require actual
Python integers. Alignment and surfaces reject negative dimensions explicitly;
existing checked overflow and geometry/chunk rejection remain intact. Zero-byte
alignment and the highest representable aligned uint64 size remain valid.
The checked-in geometry report is unchanged; no frame allocation or format limit
changes result from these input guards.

All five deterministic arithmetic methods pass, including non-integer/negative
inputs, 32/64-bit overflow, chunk limits and recorded-report equality. The new
method failed before the guards. Full native SDK-free Release CTest: 29/29 PASS,
zero skips with required FFmpeg decode. This remains a C0 mathematical model;
extreme-size SDK/GPU/application compatibility and C1/C2 are pending.

```sh
python3 tests/geometry_extreme/test_audit.py
ctest --test-dir build -C Release --output-on-failure
```

Native test environment: macOS ARM64, AppleClang 21, CMake 3.31.5, Python 3.9.6,
FFmpeg n8.0. Full Release configuration uses HYPERCINE_BUILD_STANDALONE=ON,
HYPERCINE_BUILD_UPSTREAM=OFF, HYPERCINE_ENABLE_CUDA=OFF,
HYPERCINE_BUILD_BENCHMARKS=OFF and HYPERCINE_REQUIRE_EXTERNAL_DECODE=ON.
Supply FFmpeg/ffprobe through PATH or the existing CMake executable cache options.
No protected-main merge or release; frozen upstream remains unchanged.
