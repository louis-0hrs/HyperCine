<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Five macOS queue readiness tasks — 2026-10-17

Source: protected main `e87ef0258ba37f5c78f1557c59d9c7331df5521c`.
Native ARM64, macOS 26.5.2, AppleClang 21.0.0, CMake 3.31.5, Python 3.9.6.
This is one current-main readiness review spanning five issues, with an
independent scalar audit harness. No existing implementation branch was changed.
Machine-readable evidence: [report.json](report.json).

| Issue | Work performed | Result and remaining gate |
| --- | --- | --- |
| [#23](https://github.com/louis-0hrs/HyperCine/issues/23) | Fresh full native standalone regression with privately configured real SDK Tier A and sequence packets | **PASS for the tested scope**: original 16/120-frame raw-packet gates, input hashes, autonomous state, 30 quantized bands/payloads, normalized packets and explicit-metadata equality checks. Unquantized signed SDK intermediate dumps remain **PENDING**; do not close #23 on quantized hashes alone. |
| [#44](https://github.com/louis-0hrs/HyperCine/issues/44) | Reconcile actual main API with pending P4 and run a bounded 4K negative probe | Main still exposes the 1080p `FilmScan3Encoder`, without P4 session geometry. The smoke command rejects 3840x2160 at the legacy 2,097,152-pixel guard **before encoding**. This is an expected guard, not a new P4 regression. #50/#101 acceptance and original SDK 4K parity remain separate gates. |
| [#46](https://github.com/louis-0hrs/HyperCine/issues/46) | Build and run the shared-quality prerequisite on native ARM64; audit the oracle probe's CLI | **2/2 quality-only CTest PASS**, covering six-profile deterministic unit vectors. Found a test-helper input defect: malformed profile/index strings can be accepted or truncated. The separate focused fix is [Draft PR #133](https://github.com/louis-0hrs/HyperCine/pull/133), head `be0d01ddad0b9a6c8700efe56b75f06a470081ac`: full quality-only Release and ASan/UBSan CTest **3/3 PASS** each; five process-test methods include 70 malformed/range cases that fail against the old helper. No six-profile binary oracle was configured in this run; P6 full-packet integration stays **BLOCKED_BY_P4_P5**. |
| [#47](https://github.com/louis-0hrs/HyperCine/issues/47) | Independently replay P7.A head `fd844fd058e0d79d4eabd39769e453c65ea78ba3` | Both plane-contract unit tests PASS. The pinned FFmpeg baseline matches all **24 compressed packets and 24 complete decoded YUV422p10 frames**, including per-plane hashes. This is an independent FFmpeg oracle; original SDK pixel parity and a native HyperCine decoder remain pending. No decoder implementation started. |
| [#70](https://github.com/louis-0hrs/HyperCine/issues/70) | Verify the documented scalar arithmetic independently; run the native ISA selection test | **1,572,864 signed vectors / 25,165,824 coefficient comparisons PASS**, also under ASan/UBSan. Native Auto remains Scalar and forced NEON is rejected. This verifies a preparation oracle, not an implemented NEON backend. P4 acceptance remains required. |

## Native tests actually executed

- Current-main standalone Release full CTest: **31/31 PASS**, no skips,
  97.95 seconds. Includes required external FFmpeg decode, fixture integrity,
  original SDK Tier A and sequence replay, parser/guardrails and native ISA tests.
- Current-main quality-only full CTest: **2/2 PASS**, 1.20 seconds.
- Standalone CI inventory tests **6/6 PASS** and privacy unit tests **13/13 PASS**.
- P7.A independent audit: **2/2 plane-contract unit tests PASS**, plus a
  successful full public-fixture baseline comparison using FFmpeg n8.0.
- Added scalar-plan standalone CTest project: **1/1 PASS**. The same exhaustive
  harness passed a separate ASan/UBSan binary; leak detection was disabled.

FFmpeg/ffprobe were not on PATH in the initial shell. Required-decode configure
failed as intended. Existing verified local binaries were supplied through the
existing CMake cache variables; there was no install or system configuration
change. Corpus paths and raw logs stay outside Git.

## Scope of the independent scalar oracle

[scalar_plan_oracle.cpp](../../../tests/macos_readiness/scalar_plan_oracle.cpp)
implements the first/interior/last 2-6 highpass equations with independent int64
arithmetic, quotient/remainder floor division and final int16 clamping. It
compares both horizontal and transposed vertical production scalar filters.
Each of eight taps sweeps all 65,536 signed int16 values, with the other taps
fixed separately to zero, minimum and maximum. Length is eight; base and row
storage satisfy the 16-byte alignment contract. Padding write canaries must
remain intact.

This is a one-tap sweep in three contexts, **not all eight-tap combinations**.
It does not cover prescale 2, arbitrary tails, the entire pyramid, or any NEON
instruction. Write canaries and these generously padded arrays do not prove
absence of logical row-padding reads; that remains a separate future safety
gate. No timing here is an optimization or acceleration claim.

## Reproduce

From this branch, with existing FFmpeg/ffprobe binaries on PATH:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_ENABLE_METAL=OFF \
  -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=OFF \
  -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure

cmake -S . -B build/quality -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_QUALITY_STATE_ONLY=ON -DHYPERCINE_BUILD_UPSTREAM=OFF
cmake --build build/quality --config Release --parallel 4
ctest --test-dir build/quality -C Release --output-on-failure

cmake -S tests/macos_readiness -B build/scalar-plan -DCMAKE_BUILD_TYPE=Release
cmake --build build/scalar-plan --config Release --parallel 4
ctest --test-dir build/scalar-plan -C Release --output-on-failure

cmake -S tests/macos_readiness -B build/scalar-plan-asan \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build build/scalar-plan-asan --config Release --parallel 4
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build/scalar-plan-asan \
  -C Release --output-on-failure
```

Set existing `HYPERCINE_REFERENCE_CORPUS_ROOT` and `HYPERCINE_FS3_SEQUENCE_ROOT`
privately to verified corpora to reproduce the 31-test inventory. Without them,
the two actual-packet tests are not registered; do not report them as executed.
`HYPERCINE_GOLDEN_DIR` is the private local discovery convention, not an
automatically consumed CMake option. Required-decode configure also accepts
explicit `HYPERCINE_FFMPEG_EXECUTABLE` / `HYPERCINE_FFPROBE_EXECUTABLE` paths.

For P7, use the existing isolated PR #83 source and its documented audit CMake
project `tests/macos_decode_portability`; this review does not duplicate that
harness or its frozen baseline onto main.

The 4K smoke probe intentionally exits 1 and must leave no output sample:

```sh
build/hypercine_encode_smoke --profile filmscan3 flat rejected.cfhd 3840 2160
```

The geometry guard executes before the FilmScan3-specific check; this command
is not a P4 encode attempt and says nothing about pending P4's measured 4K
behavior. Metal, CUDA, Vulkan, Windows/Linux runtime, 8K+, performance and
commercial decoder acceptance were not qualified in this review. All five
multi-phase issues remain open for their remaining gates.
