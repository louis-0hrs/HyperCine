<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Five macOS PR reviews and Metal cache truth fix

Reviewed existing PRs #129, #131, #132, #136 and #140 at the exact heads in
[report.json](report.json). All source reads and builds used those pinned
heads; existing owner worktrees stayed clean. Fresh build trees and raw logs
remain outside Git. These are five scoped review tasks, not completion of the
parent feature issues. No merge, release or dependency clearance is implied.

| PR | Review and fresh native result | Conclusion |
| --- | --- | --- |
| #129 / #48 | Integer-only arithmetic entry points, negative sizes, checked limits, report stability; 5 arithmetic and 6 runner unit methods passed. | No actionable finding within C0 mathematics; no extreme frame allocation or SDK compatibility claim. |
| #131 / #96 | All 145 privacy/hook tests passed, including 46 real installed-hook scenarios per SHA-1/SHA-256 format; 6 runner unit methods also passed. | Explicit init formats and 40/64-digit object checks cover both repository formats. Windows/Linux/Git 2.55 remain separate gates. |
| #132 / #115 | Required init/shutdown/device symbols, initialization and device failures, balanced library lifetime; actual extracted sampler and both writers passed 2/2 ASan/UBSan CTest. | No actionable finding within synthetic-provider scope; NVIDIA hardware and complete CUDA compilation remain untested. LeakSanitizer disabled. |
| #136 / #47 | Strict object parsing, duplicate-key rejection, exact numeric types and sanitized errors; fresh P7 CTest 3/3 passed including 24 real public FFmpeg-decoded frames. | No actionable finding in this verifier slice. FFmpeg is an independent oracle; native Mac decoder and SDK pixel parity are not established. |
| #140 / #7, #70 | Fresh complete standalone CTest 30/30 passed, but an inventory without every Metal test was accepted for enabled TRUE/YES/1 values. | Reproduced defect fixed in this child; the parent branch is untouched. |

## Finding and correction

The old runner tested `HYPERCINE_ENABLE_METAL == ON`. CMake also enables its
Metal branch for `TRUE`, `YES`, `1`, lowercase true constants and other
non-false variable values. Consequently a configuration with an enabled Metal
backend could lose all Metal CTest registrations and still satisfy the runner.
The corrected runner uses CMake variable truth semantics to derive mandatory
Metal tests. It preserves the existing CPU-only inventory for false values.

The new regression checks 23 values against the installed CMake interpreter,
then requires an incomplete inventory to be rejected for every enabled value.
This includes the case-sensitive `-NOTFOUND` suffix. Before the correction,
eleven subcases failed; afterward all 12 runner unit methods passed. Existing
privacy units passed 13/13. No encoder, shader or bitstream code changed.

## Reproduction

Run at each pinned PR source, with Python 3.8+, CMake 3.25+, Apple Clang and
external FFmpeg/ffprobe available. Use independent build directories.

```sh
# PR #129
python3 -B tests/geometry_extreme/test_audit.py
# PR #131 (145 privacy/hook methods, then 6 runner methods)
python3 -B -m unittest discover -s ci -p 'test_privacy*.py'
python3 -B ci/test_standalone_gate.py
# PR #136
cmake -S tests/macos_decode_portability -B build-p7
ctest --test-dir build-p7 -C Release --output-on-failure
```

For #132 configure the SDK-free standalone build with Debug,
`-DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'`
and `-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined`. Build target
`test_phase3a_telemetry`, then run its two tests with
`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1
ctest --test-dir build -C Debug -R '^phase3a_telemetry' --output-on-failure`.
These tests extract production source boundaries; they do not compile the
complete CUDA translation unit or load a real NVML provider.

For #140 and this child:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_TESTS=ON \
  -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --parallel 3
python3 -B ci/test_standalone_gate.py
python3 -B ci/run_standalone.py --build-dir build --system Darwin --machine arm64
```

Repeat in another fresh directory with `-DHYPERCINE_ENABLE_METAL:BOOL=TRUE`
to exercise the actual alternative cache spelling and required GPU tests.
Metal execution needs access to the Apple GPU. Actual child CPU/Metal counts
and timings are recorded in report.json. Test runtimes are not benchmarks.
Child complete CPU CTest passed 30/30 (75.97 s); complete native Metal CTest
passed 34/34 (109.89 s), both with zero skips. Removing each of the four
actual Metal registrations from the TRUE-configured inventory was rejected.
Original external SDK corpora were not configured; fixture/manifests must not
be represented as new SDK binary equality evidence. P4 acceptance and all
Metal/NEON/decoder/profile dependency gates remain unchanged.
