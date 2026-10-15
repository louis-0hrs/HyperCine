<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Standalone CFHD CI gate (Issue #8)

The SDK-free encoder introduced in #3 / PR #4, extended in PR #22/#25/#26,
and independently decoded by the Windows SDK oracle in #5 / PR #9 needs its
own CI. The existing `CPU Reference CI` builds the legacy SDK configuration;
it does not replace this standalone gate.

The workflow is installed at `.github/workflows/ci-standalone.yml`. The
initial credential lacked `workflow` permission; the user renewed GitHub
login on 2026-10-15 and the scope was verified before activation.
Issue #8 remains open until the installed matrix actually passes on GitHub.
Windows/Linux standalone results and hosted Mac results are pending.

## Activation and review

The workflow runs on relevant pushes and pull requests. Manual dispatch is
available once the workflow is on the default branch. Check all four matrix
jobs and the separate existing Windows reference job before accepting the PR.
Do not close #8 based on local results alone. No automatic merge is configured.

The workflow uses `macos-15` for Apple Silicon, `windows-2022` for MSVC x64,
and `ubuntu-24.04` for Linux x64. These are single runner labels, not combined
label arrays. The Mac choice follows the current
[GitHub hosted-runner reference](https://docs.github.com/en/actions/reference/runners/github-hosted-runners).
Every job checks the actual OS and Python process architecture and fails on a
mismatch. macOS also explicitly sets `CMAKE_OSX_ARCHITECTURES=arm64`; compiler
identity/version appears in CMake output and in test provenance. Windows enters
the installed MSVC x64 developer environment and configures Ninja with `cl`.

## What is enforced

- Separate standalone configuration with upstream/CUDA disabled, tests enabled,
  and `HYPERCINE_REQUIRE_EXTERNAL_DECODE=ON`. Missing FFmpeg or ffprobe is a
  configure error. Both tools' actual versions are logged and commands must
  execute successfully. Core library dependencies remain unchanged.
- All **25 named tests** must be registered, including Q2 fixtures, geometry,
  signed coefficients, FS3 16-frame and **120-frame manifest comparisons**, real
  independent p10 FFmpeg decoding, and replay of the frozen Windows SDK oracle.
  The runner executes the entire configured CTest suite and checks its JUnit
  inventory; failures, missing results, duplicates and skips fail the gate.
- The 120-frame test generates actual Mac samples, computes autonomous tables
  and state, and compares input/coefficient/framed-band/normalized sample hashes
  against the versioned Windows manifest. This is **manifest-backed hash
  evidence**, not a fresh Windows SDK execution or a binary-to-binary comparison
  against the optional NAS corpus. NAS availability is not assumed in hosted CI.
- Compile database entries must contain no upstream or CUDA sources/includes.
  Working-tree upstream status/diff must be empty. The existing reference
  workflow and immutable `upstream/` files are unchanged.
- A separate macOS RelWithDebInfo job instruments encoder/test binaries with
  ASan + UBSan. External packaged FFmpeg and the Python interpreter are
  unsanitized. LeakSanitizer is disabled because this host's AppleClang arm64
  runtime rejects `detect_leaks=1`; leak checking is **not** claimed.
- CMake cache, compile database, registered tests, JUnit, CTest diagnostics,
  gate summary and decode metrics are retained for seven days, including on
  failure. There are no NAS credentials or corpus downloads in the workflow.

The current validators retain their original quality assertions and frozen
packet/decode hashes; this change does not relax codec acceptance criteria.
No performance or acceleration improvement is claimed.

## Local reproduction

Use CMake 3.25+, C++17, Python 3 and FFmpeg/ffprobe in PATH. On macOS add
`-DCMAKE_OSX_ARCHITECTURES=arm64`; on Windows run in an MSVC x64 developer shell
and select Ninja (`-G Ninja -DCMAKE_CXX_COMPILER=cl -DCMAKE_C_COMPILER=cl`).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_TESTS=ON \
  -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --parallel 3
python3 ci/test_standalone_gate.py
python3 ci/run_standalone.py --system Darwin --machine arm64
```

Use `--system Windows --machine x86_64` or `--system Linux --machine x86_64`
on those hosts. `run_standalone.py` invokes the required full
`ctest --test-dir build -C Release --output-on-failure`, with JUnit and
`--no-tests=error` added. Optional actual-corpus tests are also required to pass
if registered. A sanitizer build uses a separate directory and the flags in the
workflow; pass that directory via `--build-dir`.

The Python reference validators now receive the actual configured compiler
identity through CTest, rather than requiring a Unix `c++` executable on MSVC.
Direct invocations retain `c++ --version` as a fallback, and report missing
provenance explicitly if no compiler information is available.

## Observed local validation

Source base: `8118288` (main after PR #27). Native Apple M2 Pro arm64,
macOS 26.5.2, AppleClang 21.0.0.21000101, CMake 3.31.5, Python 3.9.6.
FFmpeg/ffprobe n8.0 are the existing unmodified standalone validation build
(`/private/tmp/hypercine-ffmpeg-validator`), configured without assembly or
network and with the CFHD/rawvideo decode and AVI/rawvideo validation paths.
The hosted workflow installs packaged FFmpeg instead; those results remain
pending and are not inferred from this local run.

- Release full gate: **25/25 passed**, zero skips, 78.80 seconds.
- ASan + UBSan full gate with `detect_leaks=0`: **25/25 passed**, zero skips,
  164.67 seconds, no sanitizer findings.
- Five guardrail unit tests pass: missing external tests, unit-only inventory,
  duplicate/missing results, skip/failure/error statuses, and complete passes.
- Required-decode configuration without FFmpeg/ffprobe fails with the expected
  `External decode gate requires both FFmpeg and ffprobe` error.
- Initial sanitizer run with `detect_leaks=1`: **4/25 passed**, 21 aborted/failed
  because the sanitizer runtime reports that leak detection is unsupported.
  This is retained as an environment failure, not a codec finding or a pass.
- Workflow YAML parses; embedded bash steps pass `bash -n`.
- Release and sanitizer executables identify as native Mach-O arm64.

Full local diagnostics are in the ignored `build/local-gate.log` and
`build-sanitized/local-gate.log`; the failed leak-option run is retained in
`build-sanitized/unsupported-leak-sanitizer.log`.
