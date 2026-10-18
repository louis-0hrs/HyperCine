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

## Full case matrix (widened after the clean minimal run)

Every tool was re-run, with `--error-exitcode 99` and its own `--log-file`, over three
targets that together cover the issue's matrix:

1. `test_decode_phase3b1_acceptance --sanitizer` — frozen 64×64 and 1080p
   progressive/interlaced/GOP-1/GOP-2, a mixed GOP-1+FIELDPLUS corpus, slot reuse,
   1/2/4/auto slot modes, two and four concurrent decoder instances, consumer-stream
   `StreamWait` ordering, held/delayed release, failed-ticket independence, drain, and
   the malformed/truncated/oversized/band-missing rejection cases;
2. `test_decode_phase3b2a_reconstruction` — canonical 64×64 fieldplus frames 0/1 plus
the four committed fixtures
   (`temporal_asymmetric_64_progressive`, `motion_64_progressive`,
   `chroma_temporal_64_progressive`, `motion_1080p_progressive`), BT.601/BT.709 ×
   CG/VS matrix/range, 200/100-repeat determinism, 400-request zero-allocation
   alternation, and the frame-index-2 `InvalidRequest` rejection;
3. `test_decode_phase3b2a_temporal` — the minimal temporal kernel, kept for continuity.

| Tool | acceptance `--sanitizer` | reconstruction | temporal |
| --- | --- | --- | --- |
| `memcheck --leak-check full` | 0 errors / 0 leaks | 0 errors / 0 leaks | 0 errors / 0 leaks |
| `initcheck` | 0 errors | 0 errors | 0 errors |
| `racecheck` | 0 hazards / 0 warnings | 0 hazards / 0 warnings | 0 hazards / 0 warnings |
| `synccheck` | 0 errors | 0 errors | 0 errors |

All 12 tool×target runs exited 0. Summaries were read from the sanitizer log files, not
inferred from the test exit code. No suppressions, no `--launch-skip`/`--launch-count`.
This is GPU-instrumented qualification of the FIELDPLUS decoder, not a golden-bitstream
equality proof (the corpus has no phase3b2a intermediate dumps).

Per-run sanitizer logs (local paths neutralised) are committed under
`benchmarks/results/decode_phase3b2a_linux_sanitizer_logs/` with `summary.tsv`; the
full-suite log is in
`benchmarks/results/decode_phase3b0_linux_heap_corruption_logs/ctest_full_suite.txt`.

## Outcome

`DECODER_SAFETY_PASS` **for the FIELDPLUS decoder GPU gates**: all four tools genuinely
instrumented on the A2 across the minimal kernel **and** the full matrix, with zero
errors/hazards/leaks throughout.

The separate host heap-corruption item is resolved with evidence (not silently closed):
it is **reproducible and deterministic on Linux**, and it is a crash inside the
**upstream encoder's unsupported-format path**, not the FIELDPLUS decoder — see
`docs/DECODE_PHASE3B0_LINUX_HEAP_CORRUPTION.md` and
`benchmarks/results/decode_phase3b0_linux_heap_corruption.json`. In short: the upstream
`SampleEncoder` guards unsupported input→encoded conversions with `assert(0)`
(`SampleEncoder.cpp:260`); Release/`NDEBUG` builds compile that out, and the probe's
`rg24_rgba` case then SIGSEGVs during `CFHD_EncodeSample`. Not a 3B-2A defect, and not
claimed to be the exact single Windows observation.

Also not claimed:

- real 1080p/4K GOP-2 **golden** samples were only exercised as the committed 1080p
  fixture and generated 4K probe, not against Windows golden intermediate dumps.
- `upstream/` unchanged, nothing merged, no decoder-source change beyond the six
  portability fixes above.

## Reference data

The NAS CIFS space is reachable from this host and the Windows golden corpus under
`HyperCine/golden/windows/qualities/2026-10-15/` is verified (1760/1760 files, index
SHA-256 `2343a5efae40590de81cc87b275fce382cc59565f3ae4689f21cc83f48218205`), so the
earlier `BLOCKED_REFERENCE` state no longer applies to that corpus. It does not contain
phase3b2a decoder intermediate dumps, which is why this run is a sanitizer/safety proof
rather than a golden parity proof.
