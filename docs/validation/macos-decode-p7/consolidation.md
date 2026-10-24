<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# P7.A combined integration qualification

Refs #47; native macOS ARM64, 2026-10-18. Main base:
`e094682266b75151ab1a6f3ed2741ca3088c627e`.

This isolated review branch consolidates exactly these previously reviewed sources:

| Source | Revision | Contribution |
| --- | --- | --- |
| #83 | `fd844fd058e0d79d4eabd39769e453c65ea78ba3` | Readiness audit and public independent FFmpeg fixture |
| #136 | `8f547c6719f4ec7a529764d0b0525b5ee9f9afcf` | Strict JSON evidence types, roots and duplicate keys |
| #150 | `77b8715fd702043bc04863c9bbf975540aa95cf2` | Exact integer plane geometry |

All test helper and fixture files match the #150 combined source byte for byte.
The original #83 fixture manifest is unchanged. Only the readiness document gains
an updated status section, and this integration report is new. Existing source
branches are unchanged. The three historical reports retain their original test
counts; those counts are not this combined run's evidence.

Toolchain: AppleClang 21.0.0, Python 3.9.6, FFmpeg n8.0. The dedicated P7 suite
passed **3/3 CTest** (2.61 seconds), including real independent decoding of all
24 public SDK CFHD packets into 24 YUV422p10 frames with the frozen plane hashes.
Full standalone Release CTest passed **32/32** (74.79 seconds), including
required external FFmpeg decode and the four task-graph worker configurations.
No tests were skipped in either suite.

Reproduction, with locally configured native executable variables:

```sh
cmake -S tests/macos_decode_portability -B build-p7 \
  -DFFMPEG_EXECUTABLE="$FFMPEG" -DFFPROBE_EXECUTABLE="$FFPROBE"
ctest --test-dir build-p7 -C Release --output-on-failure

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_TESTS=ON \
  -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON \
  -DHYPERCINE_FFMPEG_EXECUTABLE="$FFMPEG" \
  -DHYPERCINE_FFPROBE_EXECUTABLE="$FFPROBE"
cmake --build build --parallel 4
ctest --test-dir build -C Release --output-on-failure
```

The dedicated P7 tests remain a separate CMake project; the root CTest count does
not include them. Initial configuration attempts detected the default upstream
option and unavailable tools on PATH; the explicit options and executable
variables above corrected configuration without changing code or excluding tests.

No private corpus, original SDK pixel comparison, new sanitizer run, CUDA/Metal
execution, decoder port or acceleration claim. No root CMake/runtime codec change.
P7.B remains blocked by #13/#16 shared-core safety/integration and qualified
matched SDK output. Review this combined P7.A diff before any main integration;
keep #47 open for the actual native decoder.
