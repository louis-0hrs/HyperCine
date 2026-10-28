<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# macOS validation gate completeness

Five scoped prerequisite tasks, based on main
`e87ef0258ba37f5c78f1557c59d9c7331df5521c`. These changes harden the
standalone validation runner; they do not complete the parent feature issues.

| GitHub task | Concrete change | Acceptance |
| --- | --- | --- |
| #7 Metal parity qualification | Require backend selection and entropy exactness; when Metal is enabled, require its device, autonomous manifest, 120-frame sequence and external decode registrations. | Removing any required registration fails before CTest execution. |
| #23 original Windows binary comparisons | Derive mandatory actual Tier A and sequence tests from configured corpus roots, including corresponding Metal comparisons. | Configured roots cannot silently degrade to manifest-only validation. No external corpus is assumed present. |
| #46 shared quality-state prerequisite | Build and register the existing shared quality-state unit suite in standalone mode; require it in the runner. | Common six-profile mathematics is tested without adding a Mac profile implementation or new tables. |
| #70 native ARM64 qualification prerequisite | Compare the declared macOS target with explicit CMake architecture, in addition to interpreter architecture. | Missing, x86-only, universal and arm64e target settings fail an arm64 job. No NEON kernel or dispatch change. |
| #54 validation-report integrity prerequisite | Invalidate old PASS summary, registration inventory and JUnit before preflight. | A rejected job cannot upload old success evidence; unrelated diagnostic files remain. This is not a privacy-scanner replacement. |

Main's runner was directly reproduced accepting inventories missing either
`entropy_bitexact` or `filmscan3_backend_selection`; the corrected runner
rejects both. Eleven guardrail unit tests exercise every baseline omission,
Metal omissions, configured corpora, target settings, stale evidence, result
status and frozen fixture checkout. The guardrail suite is now itself a
mandatory CTest registration.

The actual configured inventories contain 30 CPU and 34 Metal tests. Removing
each of the four actual Metal registrations from CTest JSON was rejected by
the corrected runner, independently of the synthetic unit inventories.

## Native results

macOS 26.5.2, Apple M2 Pro ARM64, AppleClang 21.0.0.21000101,
CMake 3.31.5, Python 3.9.6, existing external FFmpeg n8.0 validation build.
Representative Release, sanitizer and Metal executables identify as Mach-O
ARM64. [Sanitized machine-readable evidence](report.json) pins source hashes.

| Suite | Result | CTest elapsed |
| --- | --- | --- |
| Release full standalone gate | 30/30 PASS, zero skips | 79.85 s |
| ASan + UBSan full standalone gate | 30/30 PASS, zero skips | 168.22 s |
| Native Metal full standalone gate | 34/34 PASS, zero skips | 106.47 s |
| Runner guardrail units | 11/11 PASS | not benchmarked |
| Existing privacy guard units | 13/13 PASS | not benchmarked |

The initial sandbox Metal suite produced 30 passes and four failures because
no Metal device was visible. A direct device test outside the sandbox detected
Apple M2 Pro and passed; the full 34-test gate was then rerun outside the
sandbox and passed. This limitation was not converted into a skip or success.
Timings are test runtimes, not codec performance comparisons. No sanitizer
findings were reported; LeakSanitizer was disabled as described below.

## Reproduction

Use CMake 3.25+, Apple Clang, Python 3.8+ and external FFmpeg/ffprobe with CFHD
and rawvideo support in PATH. All commands run from the repository root.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_TESTS=ON \
  -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --parallel 3
python3 ci/test_standalone_gate.py
python3 ci/run_standalone.py --system Darwin --machine arm64
```

The runner invokes `ctest --test-dir build -C Release --output-on-failure`
with `--no-tests=error` and JUnit output. Use a separate build directory for
Metal (`-DHYPERCINE_ENABLE_METAL=ON`) and run outside a sandbox that hides GPU
access. Use another directory for ASan/UBSan with RelWithDebInfo and
`-DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'`
and `-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined`.
Run its gate with `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`
and `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. Apple ARM64 leak
checking is not claimed. Pass each directory through `--build-dir`.

## Limits

Actual original-corpus availability is not required for these runner
regressions and was not inferred from manifests. No new 4K SDK parity,
Metal 4K, six-profile Mac adapter, NEON acceleration, native Mac decoder,
Windows/Linux execution, performance improvement or production promotion is
claimed. Parent issues #44/#45/#46/#47/#70 retain their existing review gates.
Core encoder code and immutable `upstream/` are unchanged. Build trees and raw
logs remain local; only sanitized counts and toolchain evidence are published.
