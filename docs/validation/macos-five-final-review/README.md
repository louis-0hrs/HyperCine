<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Independent review of five macOS prerequisites

Native macOS ARM64, 2026-10-18; AppleClang 21, CMake 3.31.5,
Python 3.9.6, FFmpeg n8.0, Apple Git 2.50.1. Each exact source head was
reviewed in an isolated worktree. These are five bounded review tasks,
not completion of the parent encoder/decoder/security features.

| PR | Exact source head | Result |
| --- | --- | --- |
| #143 | `2d0b396ba323eacd40dfefbd7665339f95c4d73b` | No actionable finding in the p10 range/storage fix. Full standalone CTest with `PYTHONOPTIMIZE=1`: 29/29 PASS, 83.89 s. Fourteen optimized validation methods and 19 CI helper methods PASS. Checks cover actual/reference samples, all three planes, storage length, little endian, invalid ranges and retained quality threshold. |
| #142 | `eb9fe2ed101f7e470022b8632abd155df90e99e0` | No actionable finding in the bounded RGB444 p12 source boundary. Full Release standalone CTest: 30/30 PASS, 81.12 s. Separate native ARM64 ASan/UBSan RGB-only CTest: 2/2 PASS, 0.60 s; 12,562,712 storage checks and 50 rejection/state checks. Reviewed aligned owned storage, all-channel extent validation before reads, checked arithmetic, active-pixel range, deterministic padding and replacement exception safety. This emits no RGB CFHD packet. |
| #128 | `ae39cce531a2cfa5f14036f4c08fd595992b6b79` | P2 finding: a valid JSON integer outside the floating-point range in `psnr_db` raises uncaught `OverflowError`, producing a traceback containing local source paths instead of sanitized `INVALID_EVIDENCE`. This child fixes measurement validation and adds direct API and actual-process tests for positive/negative 401-digit integers. Sixteen methods pass normally and with Python optimization; existing 19 CI helper methods pass. Full child standalone CTest is recorded in `report.json`. |
| #125 | `aa0c61373e34d945bb084a6150d65e76534fb930` | No actionable finding in the original-object/ref/tag combination. Fresh native privacy discovery: 99/99 PASS, 53.156 s, including 40 scanner, 46 installed-hook and 13 actual-workflow methods. Reviewed replacement disabling before CI range selection and every direct scanner mode, retained remote/tag protections and generic diagnostics. This is native POSIX/synthetic-remote coverage, not native Windows/Linux qualification or historical disclosure remediation. |
| #137 | `6af34d01e42d68717c00482d079d67b4ac016afd` | No actionable finding in the documentation-only report. Thirty-three independent source/arithmetic consistency checks PASS: pinned commit existence and README agreement, exact two-document diff, five resident buffer payload totals, padded input and old/new overlap. Inspected actual Metal `Prepare`, `AllocateSurface`, `Buffer` and replacement assignment. Historical test timings are not independently recertified by these checks; no GPU allocation or execution. |

## Reproduced decoder evidence defect

JSON permits integers larger than the floating-point range. `math.isfinite`
implicitly converts such an integer and raises `OverflowError`. The CLI's
existing generic exception boundary does not catch it. The new regression
fails on the parent (two uncaught errors) and verifies the repaired CLI exits 2,
prints `INVALID_EVIDENCE` with `release_blocked`, emits no stderr, and does not
print the evidence path. The helper now rejects nonnumeric and unrepresentable
measurements before both PSNR and threshold comparisons. Encoder mismatch
precedence and the >35 dB threshold remain intact. No decoder runtime or
measurement provenance is supplied by this classifier.

## Reproduce

Use each pinned PR source for its review. Configure FFmpeg/ffprobe locally;
never publish their absolute paths. The full CTest command is:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_BENCHMARKS=OFF \
  -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON \
  -DHYPERCINE_FFMPEG_EXECUTABLE="$HYPERCINE_FFMPEG" \
  -DHYPERCINE_FFPROBE_EXECUTABLE="$HYPERCINE_FFPROBE"
cmake --build build --parallel 4
ctest --test-dir build -C Release --output-on-failure

# #143's additional optimization gate
PYTHONOPTIMIZE=1 ctest --test-dir build -C Release --output-on-failure
python3 -O tests/test_validation_contracts.py

# #128 and this child
python3 tests/reference_decode/test_contract.py
python3 -O tests/reference_decode/test_contract.py

# #125, isolated local synthetic repositories; real installed hooks
python3 -m unittest discover -s ci -p 'test_privacy*.py'

# #137's pinned documentation/source/arithmetic checks (Git objects required)
python3 docs/validation/macos-five-final-review/check_report.py
```

For #142's additional scoped sanitizer run configure a separate build with
`-fsanitize=address,undefined -fno-omit-frame-pointer` for C++ and
`-fsanitize=address,undefined` for the executable linker, build
`test_rgb444_frame`, then run `ASAN_OPTIONS=detect_leaks=0 ctest --test-dir
build-asan -C Release -R rgb444 --output-on-failure`. Only the RGB C++ test is
qualified by that additional run; no Python/FFmpeg instrumentation or leak
qualification is claimed.

All three full standalone runs use actual external decode plus checked-in
16/120-frame manifest tests. No external private Windows corpus is configured.
No native NVIDIA/CUDA, six-profile packet parity, RGB packet, NEON implementation,
P4/P5 acceptance, production decoder, hardware speedup or automatic merge is
implied. Existing owner worktrees and frozen `upstream/` remain unchanged.
