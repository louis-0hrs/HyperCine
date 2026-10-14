<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Native macOS CPU baseline — Phase 0

Date: 2026-10-14 (Europe/Berlin). Refs #1.

**Result: native arm64 configuration succeeds; the full CPU codec build is blocked by upstream x86/SSE2 dependencies.** The standalone transform test builds and passes natively. Three codec tests cannot run. This is a feasibility baseline, not a codec port, bitstream-parity result, performance result, or commercial release qualification.

## Source and environment

Fetched `origin/main` and created `codex/macos-cpu-baseline` from
`51435890ba5853e87e8cf1ddc7cc747954bd9e80` (also local `main`).
Reviewed `AGENTS.md`, `CONTRIBUTING.md`, `README.md`, `docs/UPSTREAM.md`,
root/source/test CMake files and Windows CPU CI.

| Item | Observed value |
| --- | --- |
| Host `uname -m` | `arm64` |
| macOS | 26.5.2, build 25F84 |
| Apple Clang | 21.0.0 (`clang-2100.1.1.101`), target `arm64-apple-darwin25.5.0` |
| SDK | `/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk` |
| CMake | 3.31.5 |
| Ninja | Not installed; attempted configure exits 1 |
| Fallback generator | Unix Makefiles, GNU Make 3.81 |
| Rosetta | `sysctl -n sysctl.proc_translated` returns `0` outside sandbox |
| Build architecture | Explicit `CMAKE_OSX_ARCHITECTURES=arm64` |

The sandbox denied the first Rosetta-status query; a separate read outside it
returned 0. Make's version banner describes its historical build system as
`i386-apple-darwin11.3.0`; this does not describe the generated binaries.
`file` confirms **Mach-O 64-bit executable arm64** for the C/C++ compiler-ID
programs, `test_transform_wavelet`, and `WaveletDemo`, and **Mach-O 64-bit object
arm64** for `wavelet_cpu.cpp.o`. No x86_64/Rosetta configuration was substituted.

Raw outputs: [toolchain](macos-cpu-baseline-logs/toolchain.txt),
[Rosetta check](macos-cpu-baseline-logs/rosetta.txt),
[binary and fixture identification](macos-cpu-baseline-logs/architecture.txt).
Logs preserve actual stdout/stderr, commands and exit codes (trailing line
whitespace removed for Git whitespace checks); project sources
were unchanged during these attempts. Compiler diagnostic paths reflect this host.

## Reproduction and observed results

Run from the repository root on Apple Silicon with Command Line Tools and CMake
3.25+ installed. These commands intentionally expose the current build failure.

```sh
git fetch origin main
git switch -c codex/macos-cpu-baseline origin/main
uname -m
sw_vers
clang --version
cmake --version
ninja --version
make --version
sysctl -n sysctl.proc_translated
xcrun --show-sdk-path

# Initial Ninja attempt: fails because Ninja is unavailable.
cmake -S . -B build/macos-cpu-ninja -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_UPSTREAM=ON \
  -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=OFF

# Available native generator: configuration succeeds.
cmake -S . -B build/macos-cpu -G 'Unix Makefiles' \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_UPSTREAM=ON \
  -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=OFF
cmake --build build/macos-cpu --config Release --parallel 4

# Independently buildable test, followed by the entire configured CPU suite.
cmake --build build/macos-cpu --config Release --target test_transform_wavelet
ctest --test-dir build/macos-cpu -C Release --output-on-failure
ctest --test-dir build/macos-cpu -C Release -R '^test_transform_wavelet$' -V

file build/macos-cpu/tests/test_transform_wavelet \
  build/macos-cpu/upstream/WaveletDemo \
  build/macos-cpu/src/CMakeFiles/hypercine_wavelet.dir/cpu/wavelet_cpu.cpp.o \
  build/macos-cpu/CMakeFiles/3.31.5/CompilerIdC/a.out \
  build/macos-cpu/CMakeFiles/3.31.5/CompilerIdCXX/a.out \
  upstream/Example/videos/CFHDsample422.AVI
```

On another CMake version, adjust the compiler-ID directory in the final `file`
command. Reuse the branch if it already exists; use fresh build directories to
reproduce initial configuration.

| Step | Exit | Evidence |
| --- | --- | --- |
| Ninja configure | 1 | [Log](macos-cpu-baseline-logs/configure-ninja.txt): missing build program |
| Unix Makefiles configure | 0 | [Log](macos-cpu-baseline-logs/configure.txt): AppleClang C/C++ detected |
| Full Release build | 2 | [Log](macos-cpu-baseline-logs/build.txt): SDK and wrapper fail at x86 intrinsic headers |
| Transform target build | 0 | [Log](macos-cpu-baseline-logs/build-transform.txt) |
| Full CPU CTest | 8 | [Log](macos-cpu-baseline-logs/ctest.txt): 1 passed, 3 not run |
| Verbose transform CTest | 0 | [Log](macos-cpu-baseline-logs/transform-verbose.txt) |

The required full CTest run was performed against the configured directory
`build/macos-cpu` rather than an unrelated `build` tree. Its result is failing,
not a passing full-suite gate.

## Test coverage and limits

| CPU test | Actual execution | Result / blocker |
| --- | --- | --- |
| `test_transform_wavelet` | Yes, native arm64 | PASS; black/white checks, nine deterministic patterns, repeated-transform equality, and inverse reconstruction (observed max difference 0 LSB for all nine patterns) |
| `test_codec_roundtrip` | No | NOT RUN; executable missing because required `CFHDCodecStatic` fails to compile |
| `test_yuy2_stability` | No | NOT RUN; same SDK dependency |
| `test_avi_sample` | No | NOT RUN; same SDK dependency; checked-in AVI exists and is identified as 1920×1080 RIFF AVI |

The transform test uses the standalone scalar `upstream/Example/WaveletDemo/utils.c`.
It does not exercise the SDK codec, optimized spatial wrapper, encode/decode,
lossy PSNR, lossless bit-exactness, or compatibility with external applications.
Its repeatability check is within this arm64 execution, not a new comparison
against Windows output. The known upstream decode dither and worker races in
`docs/UPSTREAM.md` still apply; no decoder ran here and no new bitstream or pixel
parity is claimed.

## Blocker classification

| Category | Evidence and assessment |
| --- | --- |
| CMake / toolchain | Ninja absent; Unix Makefiles configure succeeds. Missing OpenMP is nonfatal. Upstream's CMake minimum-version deprecation warning is nonfatal. No CMake change needed for this probe. |
| Legacy SDK | `CFHDCodecStatic` includes x86-dependent translation units in the default source glob; the immutable SDK is not an arm64-ready reference codec. Later compiler/linker issues may remain hidden. |
| SSE2 / x86 intrinsics | First hard failure: unconditional `<emmintrin.h>` at `upstream/Codec/DemoasicFrames.cpp:29`; wrapper fails through `spatial.h` → `image.h` → `allocator.h:28`, also unconditionally including `<emmintrin.h>`. Apple Clang reports “This header is only meant to be used on x86 and x64 architecture” plus missing `__builtin_ia32_*`. |
| Alignment | No codec runtime validation possible. The standalone scalar test has no SIMD-buffer contract. Existing codec requirements for aligned allocations and padded stride must remain part of any future ARM path. |
| Platform APIs | No separately established API/link blocker in the primary build; intrinsic compilation failure prevents full assessment of Apple-specific allocation, threading, metadata and framework integration. |
| Tests / fixtures | Transform builds and executes; AVI fixture present. Three tests are blocked by the codec build, not reported as passing or silently disabled. |

`_XMMOPT` defaults to 1 in `upstream/Codec/config.h`, but merely disabling it
cannot remove unconditional x86 includes. A separate syntax-only probe confirms
the same architecture error without changing any repository file:

```sh
clang++ -arch arm64 -D_XMMOPT=0 -fsyntax-only upstream/Codec/DemoasicFrames.cpp
```

Exit 1; [complete probe output](macos-cpu-baseline-logs/xmmopt-disabled-probe.txt).
This is a header-barrier probe, not a replacement for the full CMake build.
Suppressing diagnostics, excluding required codec units, or disabling tests
would not establish a working codec. No small safe CMake/wrapper guard was
identified that resolves this barrier while preserving codec functionality.

## Go / No-Go and next step

**No-Go for treating the present CPU reference codec as native-arm64 supported,
or resolving it with a small build-only adjustment. Go for a separately scoped
ARM CPU portability investigation**, with implementation approval and explicit
compatibility gates before claiming support. The passing scalar demo shows a
limited portable component, not feasibility of every codec path.

Proposed follow-up: inventory unconditional x86 headers, intrinsic operations,
scalar alternatives, Apple platform dependencies and alignment assumptions;
design an isolated scalar/ARM backend outside immutable `upstream/`, consistent
with the zero-external-dependency rule. Establish deterministic transform and
bitstream vectors and decode tests (with known decoder limitations accounted
for) before considering SIMD acceleration. This phase does not implement that
port, introduce an intrinsic-emulation dependency, or start Metal/GPU work.

Only documentation and evidence logs change on this branch. Root/source/test
CMake, Windows CI, CPU/CUDA sources and `upstream/` remain unchanged. Windows and
CUDA were not executed on this Mac; no runtime regression claim is inferred
from the Mac test. There are no executable changes that affect those paths.
Benchmarks were disabled; no performance figures or improvements are claimed.

Verification before publication:

```sh
git status --porcelain upstream/
git diff main...HEAD -- upstream/
git diff --check
```

All three checks are empty. Per Issue #1's blocked-result acceptance path, this
documentation is published on its own branch with an issue comment describing
the blockers and proposed follow-up; no draft PR or auto-merge is requested.
