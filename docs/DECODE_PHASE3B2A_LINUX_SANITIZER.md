# Decode Phase 3B-2A — Linux A2 sanitizer qualification (issue #38)

Scope: PR #12 progressive FIELDPLUS GOP-2 decoder branch, checked on real Linux CUDA
hardware. Windows could not instrument this branch at all (Compute Sanitizer
`No attachable process found`, RTX 4090 / WDDM / CUDA 12.8 / driver 610.62), so the
windows-side decoder result stayed unproven. This report covers the Linux side.

## Exact source under test

| Item | Value |
| --- | --- |
| Base | PR #12 head `1d78fc183ea28c4268345521e83fb2bca3226381` (`decode-phase3b2a`) |
| Isolation | own worktree + branch `codex/linux-decoder-sanitizer-p0`, PR12 branch not overwritten |
| Portability patches | only the minimal fixes below, no `upstream/` change, no #35 history merge |

## Host and toolchain

| Item | Value |
| --- | --- |
| GPU | NVIDIA A2, compute capability 8.6, driver 550.163.01 |
| CUDA | toolkit 12.4.131 (`/usr/bin/nvcc`) |
| Compute Sanitizer | 2024.1.1.0 (build 34043715) |
| Host | Debian 13, kernel 6.12.100+deb13-amd64, GCC 14.2.0 |
| Build | `cmake -S . -B build-linux-cuda -G Ninja -DCMAKE_BUILD_TYPE=Release -DHYPERCINE_BUILD_UPSTREAM=ON -DHYPERCINE_ENABLE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=86 -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=OFF` |

No driver, kernel, CUDA or system package was installed or modified.

## Portability fixes required (decoder-only, outside `upstream/`)

1. `cmake/linux_size_t_prefix.h` + root `CMakeLists.txt` — forced `-include` for C++
   translation units: `upstream/Common/CFHDAllocator.h` uses `size_t` without
   `<stddef.h>`.
2. Root `CMakeLists.txt` — `libuuid` as INTERFACE dependency of `CFHDCodecStatic`
   (`uuid_generate` from `upstream/EncoderSDK/SampleEncoder.cpp`).
3. `src/CMakeLists.txt` — `hypercine_wavelet` declares `CFHDCodecStatic` `PUBLIC`,
   so GNU ld sees the archive order it needs (MSVC re-scans, GNU ld does not).
4. `src/cuda/decode_phase2b_pipeline.cu` — `std::memcpy` used without `<cstring>`.
5. `tests/decoder_support.h` — `cudaMalloc(&p, ...)` with `T* p` relies on the implicit
   `T** -> void**` conversion, which is invalid C++ and rejected by GCC. Now an
   explicit `reinterpret_cast<void**>(&p)`.
6. `tests/test_phase2b.cpp` — `<cstring>` for `std::memcpy`.

Without these the Linux CUDA build fails outright, so a reported "pass" would have
been meaningless; the tools below ran after a real, complete build.

## Minimal-decoder run (real launches, instrumented)

`test_decode_phase3b2a_temporal` — the smallest CUDA decoder case, and the exact test
that could not be instrumented on Windows:

| Gate | Command | Exit | Result |
| --- | --- | --- | --- |
| plain | `./tests/test_decode_phase3b2a_temporal` | 0 | `progressive FIELDPLUS temporal inverse exact vectors passed` |
| `memcheck --leak-check full` | `compute-sanitizer --tool memcheck --leak-check full --error-exitcode 99 ...` | 0 | `LEAK SUMMARY: 0 bytes leaked in 0 allocations`, `ERROR SUMMARY: 0 errors` |
| `initcheck` | `compute-sanitizer --tool initcheck --error-exitcode 99 ...` | 0 | `ERROR SUMMARY: 0 errors` |
| `racecheck` | `compute-sanitizer --tool racecheck --error-exitcode 99 ...` | 0 | `RACECHECK SUMMARY: 0 hazards displayed (0 errors, 0 warnings)` |
| `synccheck` | `compute-sanitizer --tool synccheck --error-exitcode 99 ...` | 0 | `ERROR SUMMARY: 0 errors` |

Every tool was run with `--error-exitcode 99` and `--log-file`; summaries above were
read from the sanitizer log files, not inferred from the test binary exit alone. The
kernels really launched (the test asserts its exact vectors and reports the same
result under every tool). No suppression file and no `--launch-skip`/`--launch-count`
restriction was used to hide work.

`test_decode_phase3b2a_reconstruction <repo-root>` also passed:

- LL offset discrimination (BGRA missing +24 and signed-planar missing +10 both change
  128 coefficients)
- FIELDPLUS memory accounting at 1920x1080 with 2 slots: device total 170532360 B,
  immutable 150016 B, per-slot 85191172 B, pinned total 72518552 B, pageable capacity
  85510472 B, 152 CUDA allocation calls
- frame 0/1 BGRA hashes, 200/100-repeat determinism and zero-allocation 400-request
  alternation passed

## Outcome

`DECODER_SAFETY_PASS` **for the minimal decoder GPU gates**: all five tools genuinely
instrumented on the A2 with zero errors/hazards and zero leaks.

Still open, and not claimed:

- the full case matrix (GOP2 frames 0/1 alternation, 1/2/4 slots, invalid input, GOP1
  progressive/interlaced, mixed GOP1+GOP2, consumer-stream/wait ordering) was not run;
  the issue prescribes widening only after a clean minimal run.
- the single, never-reproduced Windows `test_decode_phase3b0` host heap corruption has
  **not** been reproduced here; a host ASan/UBSan build and repeated runs are still
  required before that item can be closed.
- real 1080p/4K GOP2 samples were not exercised in this minimal run.
- `upstream/` unchanged, nothing merged, no decoder-source change beyond the six
  portability fixes above.

## Reference data

The NAS CIFS space is reachable from this host and the Windows golden corpus under
`HyperCine/golden/windows/qualities/2026-10-15/` is verified (1760/1760 files, index
SHA-256 `2343a5efae40590de81cc87b275fce382cc59565f3ae4689f21cc83f48218205`), so the
earlier `BLOCKED_REFERENCE` state no longer applies to that corpus. It does not contain
phase3b2a decoder intermediate dumps, which is why this run is a sanitizer/safety proof
rather than a golden parity proof.
