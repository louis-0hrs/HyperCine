<!-- Copyright (c) 2026 HyperCine Contributors. Dual-licensed under the MIT License or Apache License, Version 2.0. See LICENSE-MIT and LICENSE-APACHE for details. -->

# Decode 3B-2A Issue #13: Windows host instrumentation and Compute Sanitizer evidence

Date: 2026-10-16
Issue: #13, "Decode 3B-2A Q1: CUDA sanitizer qualification and heap-corruption follow-up"
Branch: `decode-phase3b2a-issue13-windows-asan` (based on `decode-phase3b2a` at `1d78fc183ea28c4268345521e83fb2bca3226381`, the PR #12 head)
Scope: the Windows part of #13 — repeatable host heap-corruption reproduction/non-reproduction with host memory instrumentation, plus the missing Compute Sanitizer attach and kernel evidence.

No production file, encoder file or `upstream/` file was changed. The only source addition is a diagnostics-only Compute Sanitizer control binary (see section 4).

## 1. Environment

- Windows 11 Home 25H2, build 26200 (WDDM)
- NVIDIA GeForce RTX 4090 Laptop GPU, driver 610.62, 16,376 MiB
- CUDA Toolkit 12.8.61, Compute Sanitizer 2025.1.0.0 (build 35351055)
- MSVC 19.44.35229.0 (toolset 14.44.35207), CMake 3.31.6
- Host AddressSanitizer: `clang_rt.asan_dynamic-x86_64.dll` from toolset 14.44.35207

`benchmarks/results/decode_phase3b2a_issue13_windows_20261016/environment.txt` records the tool output for these versions.

## 2. Host ASan configuration

```
cmake -B build-asan -S . -G "Visual Studio 17 2022" -A x64 \
  -DHYPERCINE_ENABLE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89 \
  -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=OFF \
  -DCMAKE_CXX_FLAGS="/fsanitize=address /Zi /Dfseeko=_fseeki64" \
  -DCMAKE_CUDA_FLAGS="-Xcompiler=/fsanitize=address -Xcompiler=/Zi" \
  -DCMAKE_EXE_LINKER_FLAGS="/DEBUG"
```

Three configuration details matter and are not obvious:

1. The CUDA host pass must carry `/fsanitize=address` too. Instrumenting only the C++ translation units produces `LNK2038` `annotate_string`/`annotate_vector` conflicts between instrumented C++ objects and non-instrumented nvcc host objects inside the same static library.
2. `/DEBUG` is required, otherwise MSVC ASan reports module offsets instead of function names and line numbers.
3. The unmodified upstream `upstream/Example/mp4reader.cpp` needs `/Dfseeko=_fseeki64` in an ASan configuration. This is a build flag; `upstream/` remains untouched.

MSVC ASan on Windows does not support `detect_leaks` and does not honor `halt_on_error=0`; both are recorded as tool limits, not as passes.

## 3. Heap-corruption follow-up

The historical observation was one full CTest run exiting with heap corruption in `test_decode_phase3b0`, with isolated reruns passing.

| Target | Repetitions | Cases per run | Exit codes | ASan reports |
| --- | ---: | ---: | --- | ---: |
| `test_decode_phase3b0` | 20 | 297 | 0 in 20/20 | 0 |
| `test_decode_phase3b2a_temporal` | 5 | — | 0 in 5/5 | 0 |
| `test_decode_phase3b2r --reference-smoke` | 1 | — | 0 | 0 |
| `test_decode_phase3b2r --math` | 1 | — | 0 | 0 |
| `test_decode_phase3b2a_reconstruction --matrix-range` | 1 | 8 matrix/range cases | 0 | 0 |
| `test_decode_phase3b2a_reconstruction` (default) | 5 | — | 1 in 5/5 | 1 per run |
| `test_decode_phase3b2a_reconstruction --4k-smoke` | 1 | — | 1 | 1 |

**`test_decode_phase3b0`: not reproduced.** 5,940 case executions across 20 isolated instrumented repetitions produced no ASan report. Every run reported the same inventory (`attempts 297 encoded 274 profiles 7 fixtures 7 yuv10 1 interlaced 1`), and after removing the upstream-encoded byte counts all 20 logs hash identically. A subset of cases reports encoded byte counts that differ by up to 4 bytes between runs; that is upstream SDK probe metadata/rate-control variance in the probe, not HyperCine decoder output, and it matches the cross-process metadata behaviour already documented in `UPSTREAM.md`.

On Windows the probe cases that crash the Linux lane return upstream error code 10 (`CFHD_ERROR_UNEXPECTED`) — for example all `rg24_rgba_*` cases — instead of faulting. The Linux lane root-caused its `test_decode_phase3b0` failure to the unsupported `rg24 -> CFHD_ENCODED_FORMAT_RGBA_4444` pair; on MSVC Release the upstream `assert(0)` in that path is compiled out, so the same case returns an error.

## 4. Reproducible host ASan finding: upstream SSE2 read-ahead

The only reproducible host ASan finding in this session is in the **frozen upstream CPU decoder used as the coefficient oracle**, not in HyperCine production decode:

```
ERROR: AddressSanitizer: heap-buffer-overflow
READ of size 16, 0 bytes past the end of a 2,073,680-byte region
  #0 InvertTemporalQuant16s                     upstream/Codec/temporal.c:9606
  #1 TransformInverseTemporalQuant              upstream/Codec/wavelet.c:2355
  #2 ReconstructWaveletBand                     upstream/Codec/decoder.c:13238
  #3 DecodeSampleSubband                        upstream/Codec/decoder.c:12144
  #4 UpdateCodecState                           upstream/Codec/decoder.c:24070
  #5 DecodeSampleGroup                          upstream/Codec/decoder.c:11242
  #6 DecodeSample                               upstream/Codec/decoder.c:11083
  #7 CSampleDecoder::DecodeSample               upstream/DecoderSDK/SampleDecoder.cpp:1558
  #8 CFHD_DecodeSample                          upstream/DecoderSDK/CFHDDecoder.cpp:757
  #9 CaptureUpstreamFieldPlusBands              tests/test_decode_phase3b2a_reconstruction.cpp:135
 #10 main                                       tests/test_decode_phase3b2a_reconstruction.cpp:1136
allocation: AllocWaveletStack wavelet.c:478 -> AllocImage image.c:169 -> AllocAligned allocator.h:162
```

- Trigger: the 1920×1080 FIELDPLUS fixture `tests/fixtures/phase3b2a/motion_1080p_progressive.cfhd` decoded through `CFHD_DecodeSample` into a `CFHD_PIXEL_FORMAT_YU64` target, and the 4K smoke case as well. The same access site and size occur in 5/5 default-mode runs and in the 4K run.
- Mechanism: the SSE2 temporal inverse preloads the next iteration's lowpass/highpass coefficients with unconditional `_mm_load_si128` loads. On the last group of the last row the read-ahead passes the end of the plane allocation.
- Classification: `UPSTREAM_ORACLE_OUT_OF_ALLOCATION_READ`. It is a read, it is inside `upstream/`, and it is exactly the kind of access a CRT heap checker can report as heap corruption when the read leaves the mapped or checker-guarded region. It is therefore a plausible explanation for the single historical heap-corruption exit, not a proven one.
- The 64×64 fixture path (`--matrix-range`) stays inside its allocation and reports nothing, so the finding is size dependent.
- Upstream was not modified. The quirk is recorded in `UPSTREAM.md`; HyperCine SIMD backends must keep defined boundary behaviour and must not imitate this read-ahead.

Evidence: `benchmarks/results/decode_phase3b2a_issue13_windows_20261016/host-asan/` contains the symbolized reports, the run matrix and one full `test_decode_phase3b0` log.

## 5. Compute Sanitizer attach and instrumentation

Attach is resolved on this machine: instrumented runs execute on the RTX 4090 without the former `No attachable process found` failure.

Because Compute Sanitizer 2025.1.0.0 in this installation has no `--dump-kernel-launches`, a zero-error report alone would not show that device code was instrumented at all. `tests/sanitizer_launch_control.cu` is a diagnostics-only control binary added for exactly that purpose (CUDA-gated build target, no CTest registration, `--clean` is the safe mode and `--violate` writes one element past a device allocation and is meant to run only under the sanitizer):

| Control run | Result |
| --- | --- |
| `--clean` under memcheck | 0 errors |
| `--violate` under memcheck | `Invalid __global__ write of size 4 bytes` reported |
| `--violate` with `--kernel-name kns=sanitizer_control_out_of_bounds` | the same invalid write is reported |
| `--violate` with `--kernel-name kns=sanitizer_control_clean` | 0 errors — the filter provably gates which kernels are checked |

Decoder targets, all with `--error-exitcode 99`:

| Target | memcheck + leak check | initcheck | racecheck | synccheck |
| --- | --- | --- | --- | --- |
| `test_decode_phase3b2a_temporal.exe` | 0 errors, 0 leaked bytes in 0 allocations | 0 errors | 0 hazards (0 errors, 0 warnings) | 0 errors |
| `test_decode_phase3b2a_reconstruction.exe .` | 0 errors, 0 leaked bytes in 0 allocations | 0 errors | 0 hazards over the first 400 kernel launches | 0 errors over the first 400 kernel launches |
| `test_decode_phase3b2a_reconstruction.exe --matrix-range .` (64×64) | — | — | 0 hazards | 0 errors |
| `test_decode_phase3b0.exe` (297-case probe) | 0 errors, 0 leaked bytes in 0 allocations | — | — | — |
| `test_decode_phase3b2a_upstream_temporal.exe` | not applicable: the target terminates before the first instrumented CUDA API call | — | — | — |

The production reconstruction target verified its BGRA hashes, the 200/100-repeat determinism checks, the 400-request zero-allocation alternation and its resource accounting **inside the instrumented process**, so its zero-error result is not a vacuous "no kernel ran" result. The unbounded racecheck on that target did not finish within the session budget; the bounded runs above are the reported coverage.

## 6. Session result

- Full CUDA CTest on this branch: **44/44 PASS** in 773.46 s (Release, CUDA 12.8.61, RTX 4090 Laptop GPU).
- Windows host instrumentation part of #13: **PASS**. Attach is demonstrated, instrumentation is proven active with controls, all four tools plus leak checking report zero on the decoder targets within the documented coverage, and the historical heap corruption did not reproduce in 20 instrumented repetitions.
- The reproducible host finding is an upstream oracle read-ahead, documented rather than fixed, with a bounded effect claim: it is a read, it does not change HyperCine production decode, and the frozen reference hashes still reproduce in the non-ASan Release build.
- The #13 safety gate stays **HOLD** overall: PR #52 still has not integrated the native A2 test patch #80, and the resulting-parent native tests do not exist yet. No production promotion is recommended from this evidence.

## 7. Files

- `benchmarks/results/decode_phase3b2a_issue13_windows_20261016/decode_phase3b2a_issue13_windows.json` — machine-readable record
- `benchmarks/results/decode_phase3b2a_issue13_windows_20261016/host-asan/` — ASan reports, run matrix, runner script
- `benchmarks/results/decode_phase3b2a_issue13_windows_20261016/compute-sanitizer/` — sanitizer logs and target output
- `tests/sanitizer_launch_control.cu` — sanitizer instrumentation control (diagnostics only)

Committed logs are sanitized: host paths, user names and the machine name are redacted. Raw logs stay local.
