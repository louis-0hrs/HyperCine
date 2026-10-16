<!-- Copyright (c) 2026 HyperCine Contributors. Dual-licensed under the MIT License or Apache License, Version 2.0. See LICENSE-MIT and LICENSE-APACHE for details. -->

# Issue #13 Windows instrumentation evidence

Raw evidence for the Windows part of issue #13. The narrative report is
[`docs/DECODE_PHASE3B2A_ISSUE13_WINDOWS.md`](../../../docs/DECODE_PHASE3B2A_ISSUE13_WINDOWS.md);
the machine-readable record is [`decode_phase3b2a_issue13_windows.json`](decode_phase3b2a_issue13_windows.json).
Committed logs are sanitized: host paths, user names and the machine name are redacted.

## host-asan

Built with MSVC AddressSanitizer, CUDA host pass included:

```
cmake -B build-asan -S . -G "Visual Studio 17 2022" -A x64 \
  -DHYPERCINE_ENABLE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89 \
  -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=OFF \
  -DCMAKE_CXX_FLAGS="/fsanitize=address /Zi /Dfseeko=_fseeki64" \
  -DCMAKE_CUDA_FLAGS="-Xcompiler=/fsanitize=address -Xcompiler=/Zi" \
  -DCMAKE_EXE_LINKER_FLAGS="/DEBUG"
```

| File | Content |
| --- | --- |
| `run-asan-matrix.sh` | the runner (paths parameterized) |
| `run-matrix.txt` | per-run exit code, duration and ASan report count |
| `phase3b0-asan-run01.log` | one full `test_decode_phase3b0` log (297 cases) |
| `reconstruction-heap-buffer-overflow.log` | symbolized report for the 1080p FIELDPLUS YU64 capture, same site in 5/5 runs |
| `reconstruction-4k-smoke-heap-buffer-overflow.log` | symbolized report for the 4K case, same site and size |

Result: 20/20 `test_decode_phase3b0` runs clean (heap corruption not reproduced); one reproducible
finding in the upstream oracle (`InvertTemporalQuant16s`, `upstream/Codec/temporal.c:9606`,
16-byte read past the plane allocation); 64x64, temporal, and 3B-2R paths clean.

## compute-sanitizer

All runs used `--error-exitcode 99` and `--launch-timeout 0`.

| File | Content |
| --- | --- |
| `control-*.log` | instrumentation controls from `tests/sanitizer_launch_control.cu` |
| `temporal-*.log` | four tools on the minimal CUDA temporal target |
| `reconstruction-memcheck.log`, `-target-output.txt`, `reconstruction-initcheck.log` | full production reconstruction target with leak checking |
| `reconstruction-racecheck-bounded-400-launches.log`, `reconstruction-synccheck-bounded-400-launches.log` | production target, first 400 kernel launches |
| `reconstruction-64-matrix-*.log` | 64x64 `--matrix-range` production decode |
| `phase3b0-probe-memcheck.log`, `-target-output.txt` | 297-case probe under memcheck with leak checking |
| `upstream-temporal-memcheck.log` | target terminates before the first instrumented API call |

Result: attach resolved; instrumentation proven active by the controls; memcheck plus leak
checking, initcheck, racecheck and synccheck report zero findings on the decoder targets within
the coverage documented above.

## environment.txt

Tool versions and GPU/driver information for the runs.
