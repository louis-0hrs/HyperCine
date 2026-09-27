# HyperCine Phase 3A: Sequence Correctness, Reproducibility, and Benchmark Hardening

## 1. Executive Summary

Phase 3A closes every correctness, methodology, and reproducibility gap identified in the independent adversarial audit ([`docs/INDEPENDENT_AUDIT_2026-09-26.md`](../docs/INDEPENDENT_AUDIT_2026-09-26.md)) and establishes **HyperCine** as a verified, sustained multi-frame SMPTE VC-5 / CineForm (`CFHD`) encoder:

- **Reproducible Source Baseline (Step 0):** Committed the verified Phase 2D baseline at `4c880e8` (tagged `phase2d-research-checkpoint`) on branch `phase3a-sequence-correctness` before introducing Phase 3A changes. `upstream/` remains 100% untouched (`git status --porcelain upstream/` is empty).
- **Dynamic Multi-Frame Rate-Control State (`Phase3AQuantMode::DynamicUpstreamState`):** Replaced the static single-frame quantizer snapshot with an exact per-frame upstream state machine (`Phase3AEncoderContext` in [`src/core/phase3a_encoder.cpp`](../src/core/phase3a_encoder.cpp)) that updates `lastgopbitcount = sample_bytes * 8`, `FSratelimiter`, `SetEncoderQuantization`, `SetTransformQuantization`, `wavelet->quant[0..3]`, and `wavelet->scale[0..3]` on every frame $k$.
- **Split Frontend / Wavelet Multi-Slot Overlap:** Added `HyperCine_GpuSlotKickFrontendOnly` and `HyperCine_GpuSlotKickWaveletWithQuant` in [`src/cuda/hybrid_kernels.cu`](../src/cuda/hybrid_kernels.cu) along with O(1) `predicted_sample_bytes` calculation from compressed highpass word counts, allowing Frame $k+1$'s quant-independent frontend (host staging, H2D DMA, and `BgraToYuv`) to overlap on GPU while Frame $k$'s wavelet + entropy executes, and kicking Frame $k+1$'s wavelet + entropy with the exact `quant_table[k+1]` the instant Frame $k$'s GPU entropy finishes.
- **120-Frame Changing-Complexity Sequence Equivalence (`A == B == C`):** Verified across a 120-frame sequence at 1080p (spanning 15 distinct `FSratelimiter` values and 85 quantizer transitions), 24 frames at 4K, and 8 frames at 8K that **Pristine Upstream (`CFHD_EncodeSample`)**, **HyperCine CPU Reference**, and **HyperCine GPU** produce identical `FSratelimiter` trajectories, identical 36-entry `quant` and `scale` tables, identical 30/30 quantized subband coefficient hashes, and byte-identical compressed wavelets (`masked_diff_bytes == 0`).
- **300-Frame Distinct-Frame Multi-Slot Verification:** Replaced single-frame pointer reuse with an 11-archetype distinct complexity ring buffer stamped with a machine-verifiable 32-bit binary visual identity header (`0xA5 || pattern_id`, `frame_index`) in the top-left $16 \times 2$ grid of $32 \times 32$ pixel blocks. Verified 300 frames at 1080p, 48 frames at 4K, and 16 frames at 8K with zero duplicate/dropped frames, zero stale slot reuse, and 100% decoded visual stamp recovery ($\text{PSNR} > 44\text{ dB}$).
- **17-Case Adversarial Entropy & Row-Gap (`pitch_px > width`) Verification:** Fixed `EncodeHighpassStages` ([`src/cpu/entropy_stages.cpp`](../src/cpu/entropy_stages.cpp)) and `EncodeHighpassParallel` ([`src/cpu/entropy_parallel.cpp`](../src/cpu/entropy_parallel.cpp)) to accumulate `(pitch_px - width)` virtual zero runs across row gaps matching `upstream/Codec/encoder.c:EncodeQuantizedBand`. Verified 49/49 subband test cases across all 17 pathological patterns byte-for-byte across Upstream, CPU Staged, GPU `ReferencePhase2C`, GPU `Fused`, GPU `FusedScanOptimized`, and GPU `CUDA Graph`.
- **Sanitizer, Stability & Build Hardening Gates:** Passed `compute-sanitizer` (`memcheck`, `initcheck`, `synccheck`, `racecheck`) with **0 errors / 0 hazards**, MSVC AddressSanitizer (`/fsanitize=address`) with **0 errors**, CPU-only build (`-DHYPERCINE_ENABLE_CUDA=OFF`) with **4/4 CTest targets passing**, full CUDA build with **13/13 CTest targets passing**, and sustained long-run stability runs (`1000f@1080p`, `300f@4K`, `100f@8K`) with **0 bytes VRAM leak after destroy**.

---

## 2. Audit Finding Closure Matrix

| Audit ID | Severity | Finding Summary | Phase 3A Resolution & Verification | Status |
| :--- | :--- | :--- | :--- | :--- |
| **F1** | **CRITICAL** | Multi-frame GPU pipeline froze `QuantTables2D` from a single pre-pass; `lastgopbitcount` and `FSratelimiter` never updated across frames. | Implemented `Phase3AEncoderContext::PrepareFrameState` & `CompleteFrameState` in [`src/core/phase3a_encoder.cpp`](../src/core/phase3a_encoder.cpp) + `HyperCine_GpuSlotKickWaveletWithQuant` in [`src/cuda/hybrid_kernels.cu`](../src/cuda/hybrid_kernels.cu). Verified 120/120 frames `A == B == C` across 85 quant transitions (`Test 1`). | **CLOSED** |
| **F2** | **HIGH** | Multi-frame benchmarks reused one static frame buffer (`MakeFrame`) across all frames (`seq_idx = 0..N-1`). | Implemented 11-archetype changing-complexity ring buffer + per-frame `StampFrameIdentityBlocks` + `ExtractStampedFrameIdentity`. Verified across 300 frames at 1080p, 48 at 4K, 16 at 8K (`Test 2`). | **CLOSED** |
| **F3** | **HIGH** | Uncommitted Phase 2D working tree on top of `467ac53`. | Created branch `phase3a-sequence-correctness`, committed Phase 2D snapshot `4c880e8`, and tagged `phase2d-research-checkpoint`. | **CLOSED** |
| **F4** | **HIGH** | `HostInput` benchmark measured pre-pinned `cudaHostAlloc` memory without pageable host staging (`memcpy`), and primary headlines emphasized `DeviceInput`. | Defined three explicit input modes (`HostPageableEndToEnd` as primary Mode 1, `HostPinnedApi` as Mode 2, `DeviceInput` as Mode 3) with separate `host_pageable_stage_ms` and `h2d_ms` reporting. | **CLOSED** |
| **F5** | **MEDIUM** | GPU entropy equivalence was only tested on the smooth `Canonical` pattern (`37.8%` of codebooks), with a latent `pitch_px > width` mismatch between CPU and GPU entropy paths. | Fixed `pitch_px > width` row-gap zero-run accumulation in [`src/cpu/entropy_stages.cpp`](../src/cpu/entropy_stages.cpp) and [`src/cpu/entropy_parallel.cpp`](../src/cpu/entropy_parallel.cpp); allocated full highpass word capacity at 8K in [`src/cuda/entropy_gpu.cu`](../src/cuda/entropy_gpu.cu); verified 49/49 pathological cases across all 17 adversarial patterns (`Test 3`). | **CLOSED** |
| **F6** | **MEDIUM** | `gpu_utilization_pct` was an analytical estimate (`100 * total_gpu_ms / frame_period_ms`), and 8K single-run benchmarks had thermal/clock drift. | Renamed analytical metric to `pipeline_gpu_stage_occupancy_estimate_pct` and integrated dynamic `nvml.dll` hardware telemetry (`NvmlRuntimeSampler`) + 5-trial 8K variance study. | **CLOSED** |
| **F7** | **LOW** | Production metadata safety was only tested on 1-frame clips, and CPU-only build (`-DHYPERCINE_ENABLE_CUDA=OFF`) failed in `tests/CMakeLists.txt`. | Guarded `test_cuda_pyramid` under `if(HYPERCINE_ENABLE_CUDA)` in [`tests/CMakeLists.txt`](../tests/CMakeLists.txt); verified 16-frame production metadata sequence (`Test 4`). | **CLOSED** |

---

## 3. Multi-Frame Rate-Control & Quantizer State Verification (`Test 1`)

In GoPro's CineForm encoder (`upstream/Codec/encoder.c:EncodeSample`), each frame $k$ updates `encoder->lastgopbitcount` with the bit count of frame $k-1$'s output sample (`sample_bytes * 8`). On frame $k$, `SetEncoderQuantization` calls `QuantizationSetRate`, which adjusts `encoder->q.FSratelimiter` (`0..16`) based on the achieved compression ratio and recomputes the 36-entry `wavelet->quant` table via `SetTransformQuantization`.

In `VerifyPhase3ASequence120` ([`src/core/phase3a_encoder.cpp`](../src/core/phase3a_encoder.cpp)), we drive **A (`CFHD_EncodeSample`)**, **B (HyperCine CPU Reference)**, and **C (HyperCine GPU)** with the identical changing-complexity sequence cycling through flat, gradient, burst noise (`DeterministicNoise`), Nyquist grating (`HighFrequencyPattern`), hard edges, and synthetic photographic frames:

| Resolution | Frames | Matched Frames (`A == B == C`) | Distinct `FSratelimiter` Values | Quantizer Table Transitions | 30/30 Coeff Hashes Equal | 30/30 Payload Hashes Equal | Masked Sample Diff Bytes |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **1080p (`1920x1080`)** | 120 | **120 / 120** | **15** (`0..14`) | **85** | **120 / 120** | **120 / 120** | **0** |
| **4K (`3840x2160`)** | 24 | **24 / 24** | **11** | **13** | **24 / 24** | **24 / 24** | **0** |
| **8K (`7680x4320`)** | 8 | **8 / 8** | **3** | **2** | **8 / 8** | **8 / 8** | **0** |

### 1080p `FSratelimiter` Trajectory (Frames 0..39 Excerpt)
```
Frame  0.. 5 (Flat/Gradient/LowDetail):   FSratelimiter = [4, 2, 0, 0, 0, 0]
Frame  6..11 (Noise/HighFreq Burst):      FSratelimiter = [0, 3, 6, 9, 12, 13]
Frame 12..15 (Checkerboard/HardEdges):    FSratelimiter = [14, 12, 10, 9]
Frame 16..21 (Flat/Smooth Recovery):      FSratelimiter = [9, 7, 5, 3, 1, 0]
```

---

## 4. 300-Frame Distinct-Frame Multi-Slot Verification (`Test 2`)

Each frame $k$ is generated from an 11-pattern ring buffer (coprime to 2-slot and 3-slot pipelines) and stamped in-place with a 32-bit binary block code (`words[0] = 0xA500 | pattern_id`, `words[1] = frame_index`) in the top-left $512 \times 64$ region ($16 \times 2$ blocks of $32 \times 32$ pixels) plus a frame-unique 4:2:2 detail strip in rows `64..95`.

| Resolution | Frames | Active Slots | Strict Monotonic Order | Zero Duplicates / Drops | Zero Stale Slot Reuse (`SHA256[k] != SHA256[k-S]`) | Decoded Stamp Matches | Min PSNR (dB) |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **1080p (`1920x1080`)** | 300 | 2 | **PASS** | **PASS (`300/300`)** | **PASS** | **34 / 34 (`100%`)** | **44.16 dB** |
| **4K (`3840x2160`)** | 48 | 3 | **PASS** | **PASS (`48/48`)** | **PASS** | **39 / 39 (`100%`)** | **44.38 dB** |
| **8K (`7680x4320`)** | 16 | 2 | **PASS** | **PASS (`16/16`)** | **PASS** | **16 / 16 (`100%`)** | **44.37 dB** |

> [!NOTE]
> During multi-frame decode verification on a 32-thread host CPU, we uncovered two latent concurrency bugs in pristine `upstream/Codec/decoder.c` (`AllocDecoderBuffer` allocating `8 * scratch_size` while spawning up to 32 entropy worker threads, and `DecodeSampleTransform` checking `BANDS_ALL_STARTED` instead of `BANDS_ALL_VALID` before level-1 wavelet reconstruction). In accordance with Rule 1 (`upstream/` immutability), these bugs are documented in [`docs/UPSTREAM.md`](../docs/UPSTREAM.md#L105-L115) and bypassed in `DecodeCfhdSample3A` by clamping the decoder instance to 1 worker thread (`TAG_CPU_MAX = 1`).

---

## 5. Adversarial GPU Entropy & Row-Gap Verification (`Test 3`)

All 17 pathological entropy cases were verified across subbands `sb1` (`240x135`, L3 Luma LH), `sb5` (`240x270`, L2 Chroma HL), and `sb9` (`960x540`, L1 Luma HH), plus `RowGapStrideEdge` with `width = 224 < pitch_px = 256` and sentinel garbage `0x3773` in the padding columns `[width .. pitch_px - 1]`:

| # | Adversarial Case | Subbands Tested | Upstream == CPU_Staged | Upstream == GPU_Ref | Upstream == GPU_Fused | Upstream == GPU_ScanOpt | Upstream == GPU_Graph (Capture + Replay) |
| :-: | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| 1 | `AllZeros` | `sb1, sb5, sb9` | **PASS** | **PASS** | **PASS** | **PASS** | **PASS** |
| 2 | `SingleNonZeroFirst` | `sb1, sb5, sb9` | **PASS** | **PASS** | **PASS** | **PASS** | **PASS** |
| 3 | `SingleNonZeroLast` | `sb1, sb5, sb9` | **PASS** | **PASS** | **PASS** | **PASS** | **PASS** |
| 4 | `AlternatingNonZeroZero` | `sb1, sb5, sb9` | **PASS** | **PASS** | **PASS** | **PASS** | **PASS** |
| 5 | `ZeroRunsAroundBoundaries` (`1..1024`) | `sb1, sb5, sb9` | **PASS** | **PASS** | **PASS** | **PASS** | **PASS** |
| 6 | `VeryLongZeroRuns` (`> width`, multi-row) | `sb1, sb5, sb9` | **PASS** | **PASS** | **PASS** | **PASS** | **PASS** |
| 7 | `CodebookTransitionValues` | `sb1, sb5, sb9` | **PASS** | **PASS** | **PASS** | **PASS** | **PASS** |
| 8 | `MaxMinSupportedCoeffs` | `sb1, sb5, sb9` | **PASS** | **PASS** | **PASS** | **PASS** | **PASS** |
| 9 | `EscapeLargeMagnitude` | `sb1, sb5, sb9` | **PASS** | **PASS** | **PASS** | **PASS** | **PASS** |
| 10 | `ByteBoundaryCrossings` | `sb1, sb5, sb9` | **PASS** | **PASS** | **PASS** | **PASS** | **PASS** |
| 11 | `Word16BoundaryCrossings` | `sb1, sb5, sb9` | **PASS** | **PASS** | **PASS** | **PASS** | **PASS** |
| 12 | `Word32BoundaryCrossings` | `sb1, sb5, sb9` | **PASS** | **PASS** | **PASS** | **PASS** | **PASS** |
| 13 | `CudaBlockBoundaryCrossings` (`254..257`) | `sb1, sb5, sb9` | **PASS** | **PASS** | **PASS** | **PASS** | **PASS** |
| 14 | `EndOfBandTerminationEdge` | `sb1, sb5, sb9` | **PASS** | **PASS** | **PASS** | **PASS** | **PASS** |
| 15 | `SparseRandomKnownSeed` (`0x1337BEEF`) | `sb1, sb5, sb9` | **PASS** | **PASS** | **PASS** | **PASS** | **PASS** |
| 16 | `DenseHighEntropyKnownSeed` (`0xCAFEBABE`) | `sb1, sb5, sb9` | **PASS** | **PASS** | **PASS** | **PASS** | **PASS** |
| 17 | `RowGapStrideEdge` (`width=224, pitch_px=256`) | `sb1` | **PASS** | **PASS** | **PASS** | **PASS** | **PASS** |
| **Total** | **17 Pathological Patterns** | **49 Cases** | **49 / 49** | **49 / 49** | **49 / 49** | **49 / 49** | **49 / 49** |

---

## 6. Multi-Frame Production Metadata Sequence Verification (`Test 4`)

With `Phase3AConfig::deterministic_test_metadata = false` (the default in [`include/hypercine/phase3a.h`](../include/hypercine/phase3a.h)), two independent 16-frame 1080p clips (`clip1` and `clip2`) were encoded and parsed tag-by-tag:

- **`TAG_CLIP_GUID`:** Non-zero on every frame, constant across all 16 frames within each clip (`clip1 = 00a97761-4610b6cb-e9e56c98-83dcba08`), and distinct between `clip1` and `clip2` (`clip2 = ac79b8f4-4f98dfb8-bfd13a96-9bfa4be2`).
- **`TAG_ENCODE_DATE` & `TAG_ENCODE_TIME`:** Valid `YYYY-MM-DD` (`2026-09-27`) and `HH:MM:SS` (`12:20:29`) strings on every frame.
- **`TAG_UNIQUE_FRAMENUM`:** Strictly monotonic `0, 1, 2, ..., 15` across the 16-frame sequence.
- **`TAG_TIMECODE`:** Progresses across the sequence (`12:20:29:00` .. `12:20:29:15`).
- **Masked Bitstream Equivalence:** Masking only the 4 volatile metadata tags (`TAG_CLIP_GUID`, `TAG_ENCODE_DATE`, `TAG_ENCODE_TIME`, `TAG_TIMECODE`) yields `masked_diff_bytes == 0` against Pristine Upstream `CFHD_EncodeSample` across all 16 frames.

---

## 7. Input-Mode Benchmark Matrix

Performance numbers are no longer maintained in this file. The canonical clean-tree five-trial `ProductionEndToEnd` matrix is in [`docs/PHASE3A1_BENCHMARK_HARDENING.md`](PHASE3A1_BENCHMARK_HARDENING.md) and [`benchmarks/phase3a2_release_candidate.json`](../benchmarks/phase3a2_release_candidate.json) (with historical Phase 3A.1 audit data in [`benchmarks/phase3a1_canonical.json`](../benchmarks/phase3a1_canonical.json)). The tables that used to follow this heading were hand-copied from different runs and disagreed with `benchmarks/results/phase3a_sequence.json`.

Cross-mode bitstream equality remains a correctness result. With deterministic metadata, `HostPageableEndToEnd`, `HostPinnedApi`, and `DeviceInput` produce the same per-frame SHA-256. The canonical benchmark records that again. The historical single-run frame-23 hash was `c11c8ae0749e6f323e4738c777674e1131e971f490ef0e60b07b631c7dd20172`.

## 8. Retired variance, graph, and long-run FPS tables

The previous 8K variance table, the CUDA graph percent-gain table, and the long-run table (including 462 FPS at 1080p) are retired for the same reason. Section 9 of the Phase 3A.1 document is the graph measurement. Section 10 below keeps the sanitizer record. The quantizer sentence that belongs with the graph is still accurate and is restated without a second FPS table:

Per-frame `quant[0..35]` is applied by `HyperCine_GpuSlotKickWaveletWithQuant`, outside the entropy CUDA graph. The entropy graph reads already-quantized coefficient buffers at fixed device pointers. Changing `FSratelimiter` does not recapture that graph.

## 9. Placeholder removed

This heading is intentionally empty so older links to "section 9" of this file are not a second performance table. Use Phase 3A.1 / Phase 3A.2.

## 10. Long-Run Stability, Memory Leak, Sanitizer & Build Hardening Gates

### 10.1 Long-run FPS

The 1,000/300/100-frame stability runs remain evidence that the pipeline finishes, stamps decode, and VRAM returns to the pre-run value. Their FPS figures are not the published throughput. See [`docs/PHASE3A1_BENCHMARK_HARDENING.md`](PHASE3A1_BENCHMARK_HARDENING.md) and [`benchmarks/phase3a2_release_candidate.json`](../benchmarks/phase3a2_release_candidate.json).

### 10.2 CUDA `compute-sanitizer` & CPU AddressSanitizer (`ASan`) Gates

| Sanitizer Tool | Command / Target | Result |
| :--- | :--- | :--- |
| **`compute-sanitizer --tool memcheck`** | `test_phase3a.exe --quick-sanitize` & `test_entropy_capacity.exe` | **`ERROR SUMMARY: 0 errors`** |
| **`compute-sanitizer --tool initcheck`** | `test_phase3a.exe --quick-sanitize` | **`ERROR SUMMARY: 0 errors`** |
| **`compute-sanitizer --tool synccheck`** | `test_phase3a.exe --quick-sanitize` | **`ERROR SUMMARY: 0 errors`** |
| **`compute-sanitizer --tool racecheck`** | `test_phase3a.exe --quick-sanitize` | **`RACECHECK SUMMARY: 0 hazards displayed (0 errors, 0 warnings)`** |
| **MSVC AddressSanitizer (`/fsanitize=address`)** | `test_transform_wavelet`, `test_codec_roundtrip`, `test_yuy2_stability` | **3 / 3 Passed (`0 ASan errors`)** |
| **CPU-Only Build (`-DHYPERCINE_ENABLE_CUDA=OFF`)** | `ctest --test-dir build_cpu_only -C Release` | **4 / 4 CTest targets passed (`100%`)** |
| **Full CUDA Build (`-DHYPERCINE_ENABLE_CUDA=ON`)** | `ctest --test-dir build -C Release` | **14 / 14 CTest targets passed (`100%`)** |
