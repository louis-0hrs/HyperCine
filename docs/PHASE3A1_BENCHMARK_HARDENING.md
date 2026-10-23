# Historical Phase 3A.1–3A.2a benchmark hardening and provenance

This is a **historical engineering record** of the `ProductionEndToEnd_v2` methodology, Phase 3A.2a sequence/provenance corrections, and earlier slot-scaling studies. The original measurements, hashes and audit explanations below remain unchanged.

> [!NOTE]
> **Current Phase 3A.2c headline:** use [`benchmarks/phase3a2c_release_candidate.json`](../benchmarks/phase3a2c_release_candidate.json) and the [current benchmark guide](../benchmarks/results/README.md). The `3A.2a` tables below were canonical **for that historical phase**, but are no longer the README headline source.

- **Historical Clean-Tree Release Candidate Record (Phase 3A.2a — superseded):** [`benchmarks/phase3a2a_release_candidate.json`](../benchmarks/phase3a2a_release_candidate.json) — generated on clean source commit `fba948edf98b72045af150b1a918fd0d8a643c61` (`"publication": "CLEAN"`, `"canonical": true`, `"git_status_clean": true`, `"upstream_status_clean": true`, `"benchmark_contract": "ProductionEndToEnd_v2"`, `"benchmark_equivalence_verified": true`, `"native_architecture_match": true`). Those numbers remain part of the Phase 3A.2a audit record; the README's current Phase 3A.2c headline uses the later dataset linked above.
- **Preserved Historical Release Candidate Record (Phase 3A.2):** [`benchmarks/phase3a2_release_candidate.json`](../benchmarks/phase3a2_release_candidate.json) — generated on commit `4e8037841dcba3c31a6f8d31371869e26f6fbd1b` (tagged `phase3a2-release-candidate`). Preserved untouched as historical audit evidence.
- **Phase 3A.1 Audit & Diagnostic Companions:** [`benchmarks/phase3a1_canonical.json`](../benchmarks/phase3a1_canonical.json) and the fresh-process order-study companions (`benchmarks/phase3a1_fresh_*.json`) were captured during the Phase 3A.1 diagnostic pass before working-tree cleanup and are marked `"publication": "DIRTY"`. `upstream/` was never modified in any pass.

---

## 1. Phase 3A.2a Corrections (`ProductionEndToEnd_v2`)

Final audit of `benchmarks/phase3a2_release_candidate.json` identified two benchmark/provenance issues. Phase 3A.2a resolved both without altering any codec algorithm, wavelet transform, or CUDA entropy kernel:

### 1.1 CPU-vs-GPU Sequence Alignment & Sample-Size Parity
* **Root Cause in Phase 3A.2:** In the original `benchmark_phase3a1.cpp`, `RunUpstream()` hardcoded `4` warmup frames (`pool[0..3]`) and then **restarted** its timed loop at `i = 0` (`pool[0 .. N-1 % unique]`). Meanwhile, `RunGpu()` used `warmup = max(slot_count, 2)` (`2` at 1080p, `3` at 4K UHD, `2` at 8K UHD) and continued its timed loop at `pool[W .. W+N-1 % unique]`. Because CineForm's `FSratelimiter` rate controller updates `lastgopbitcount` after every frame, restarting CPU at `0` after 4 warmup frames while continuing GPU at `W` caused:
  1. Different `FSratelimiter` / `lastgopbitcount` states entering the timed window (`pool[0..3]` for CPU vs `pool[0..2]` for GPU at 4K).
  2. Different trailing frames in the 4K timed window (`N = 300 = 8 * 36 + 12`: trailing 12 frames were `pool[0..11]` for CPU vs `pool[3..14]` for GPU), which caused the 4K mean sample size discrepancy (`6,640,048` B on CPU vs `6,717,335` B on GPU).
* **Phase 3A.2a Fix (`ProductionEndToEnd_v2`):** Both `UpstreamCpuEncodeSample` and all HyperCine GPU modes now use one common warmup count `W = 4` (`kCommonWarmupFrames = 4 >= slot_count`), encoding warmup frames `0..3` (`pool[0..3]`) and timed frames `4 .. 4 + N - 1` (`pool[(4 + i) % unique_cycle]`) with **first measured logical frame index = `4`** and no sequence restart after warmup.
* **Apples-to-Apples Stateful Equivalence Gate:** `benchmark_phase3a1` now executes an automated equivalence gate (`ValidateEquivalenceGate`) on the shared pool (`W = 4`, first measured logical frame `4`, `24` measured frames at 1080p/4K and `12` at 8K) verifying 100% frame-by-frame agreement between `UpstreamCpuEncodeSample`, `HostPageableEndToEnd`, `HostPinnedApi`, and `DeviceInput` across:
  - `logical_frame_index` (`4 .. 4 + N - 1`) and `pattern`
  - `prev_encoded_bytes` and `lastgopbitcount`
  - `FSratelimiter` trajectory and 36-entry `quant[]` / `scale[]` tables
  - per-frame `sample_bytes` and `masked_sample_sha256` (`CPU/upstream masked sample == HyperCine masked sample` on every frame)
  - unmasked `sample_sha256` across all 3 GPU modes in deterministic verification mode, plus `CFHD_DecodeSample` frame stamp and PSNR ($> 35\text{ dB}$)
  - exact equality of full-sequence headline `mean_sample_bytes`, `min_sample_bytes`, `max_sample_bytes`, and `distinct_fs_rate_limiters` across all 4 modes (`1,757,179` B at 1080p; `6,761,383` B at 4K UHD; `33,988,680` B at 8K UHD).

### 1.2 CUDA Architecture Provenance (`sm_89` vs `CMAKE_CUDA_ARCHITECTURES = 52`)
* **Root Cause in Phase 3A.2:** In root [`CMakeLists.txt`](../CMakeLists.txt), `if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)` was placed *after* `enable_language(CUDA)`. In CMake 3.18+, `enable_language(CUDA)` initializes `CMAKE_CUDA_ARCHITECTURES` in `CMakeCache.txt` to nvcc's default (`52` on CUDA 12.8) when not already defined, making the subsequent `if(NOT DEFINED ...)` dead code. Meanwhile, `src/CMakeLists.txt` had `set_property(TARGET hypercine_wavelet PROPERTY CUDA_ARCHITECTURES 89)`, so `hypercine_wavelet` was actually compiled for `sm_89`, while `benchmarks/CMakeLists.txt` recorded the global `CMAKE_CUDA_ARCHITECTURES` cache variable (`52`).
* **Phase 3A.2a Fix:** Root `CMakeLists.txt` now sets default `CMAKE_CUDA_ARCHITECTURES` *before* `enable_language(CUDA)`, `src/CMakeLists.txt` inherits `CMAKE_CUDA_ARCHITECTURES` without a hardcoded override, and the canonical release build (`build-release-sm89`) was configured from scratch with `-DCMAKE_CUDA_ARCHITECTURES=89`. `cuobjdump --list-elf` and `cuobjdump --list-ptx` confirm `benchmark_phase3a1.exe` embeds `sm_89` cubin (`benchmark_phase3a1.1..3.sm_89.cubin`) and `compute_89` PTX (`benchmark_phase3a1.1..3.sm_89.ptx`), and the JSON records `"cuda_architecture": "sm_89"`, `"cmake_cuda_architectures": "89"`, `"target_cuda_architectures": "89"`, `"native_architecture_match": true`.

---

## 2. Phase 3A.2a Canonical Headline Matrix (`benchmarks/phase3a2a_release_candidate.json`)

Generated on clean source commit `fba948edf98b72045af150b1a918fd0d8a643c61` (5 trials per mode, `ProductionEndToEnd_v2`, common warmup `W = 4`, first measured logical frame `4`, BGRA8 $\to$ `CFHD_ENCODED_FORMAT_YUV_422`, FilmScan 3, GOP=1, `DynamicUpstreamState`, `DefaultAuto` slot policy, CUDA Graph ON for GPU modes):

| Resolution | Input Mode | Median FPS | Mean FPS | Stdev (FPS / %) | Min–Max FPS | Mean Latency (ms) | Median Latency (ms) | P95 / P99 Latency (ms) | Raw Input (GB/s) | Output (MB/s) | Mean Sample Bytes (Min–Max) | Speedup vs Upstream CPU | Graph | Slots (`DefaultAuto`) | Mean GPU Clock (MHz) | Mean Power (W) | Max Temp (°C) | Variance Flag |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: | :---: | ---: | ---: | ---: | :--- |
| **1080p** (`1920x1080`) | `UpstreamCpuEncodeSample` | **58.25** | 57.42 | 1.50 (2.61%) | 54.57–58.72 | 17.43 | 13.30 | 36.85 / 42.30 | 0.483 | 102.4 | 1,757,179 (180,440–5,494,332) | 1.00× | — | 1 thread | — | — | — | |
| **1080p** (`1920x1080`) | **`HostPageableEndToEnd`** | **448.61** | 441.23 | 25.97 (5.89%) | 401.18–472.37 | 3.52 | 3.30 | 4.51 / 7.28 | 3.721 | 788.3 | 1,757,179 (180,440–5,494,332) | **7.70×** | on | 2 | 1827 | 44.0 | 61 | stdev 5.9% |
| **1080p** (`1920x1080`) | `HostPinnedApi` | **542.73** | 531.53 | 28.23 (5.31%) | 476.63–554.77 | 2.74 | 2.54 | 3.91 / 5.98 | 4.502 | 953.7 | 1,757,179 (180,440–5,494,332) | **9.32×** | on | 2 | 1911 | 47.8 | 62 | stdev 5.3% |
| **1080p** (`1920x1080`) | `DeviceInput` *(GPU-resident)* | **806.15** | 775.13 | 56.87 (7.34%) | 669.63–827.36 | 2.24 | 2.05 | 3.60 / 5.49 | 6.687 | 1416.6 | 1,757,179 (180,440–5,494,332) | **13.84×** | on | 2 | 1796 | 48.9 | 61 | stdev 7.3% |
| **4K UHD** (`3840x2160`) | `UpstreamCpuEncodeSample` | **15.93** | 15.88 | 0.13 (0.79%) | 15.69–16.00 | 62.99 | 47.39 | 136.67 / 148.04 | 0.528 | 107.7 | 6,761,383 (631,824–22,236,848) | 1.00× | — | 1 thread | — | — | — | |
| **4K UHD** (`3840x2160`) | **`HostPageableEndToEnd`** | **237.21** | 228.10 | 16.24 (7.12%) | 196.74–239.84 | 11.94 | 10.86 | 17.20 / 36.67 | 7.870 | 1603.9 | 6,761,383 (631,824–22,236,848) | **14.89×** | on | 3 | 1787 | 56.7 | 61 | stdev 7.1% |
| **4K UHD** (`3840x2160`) | `HostPinnedApi` | **291.51** | 282.39 | 19.60 (6.94%) | 243.26–293.49 | 9.40 | 8.18 | 13.73 / 29.28 | 9.671 | 1971.0 | 6,761,383 (631,824–22,236,848) | **18.30×** | on | 3 | 1864 | 56.1 | 62 | stdev 6.9% |
| **4K UHD** (`3840x2160`) | `DeviceInput` *(GPU-resident)* | **399.84** | 377.62 | 41.32 (10.94%) | 295.84–404.62 | 6.98 | 5.38 | 12.90 / 37.59 | 13.266 | 2703.5 | 6,761,383 (631,824–22,236,848) | **25.10×** | on | 3 | 1642 | 51.0 | 63 | stdev 10.9% |
| **8K UHD** (`7680x4320`) | `UpstreamCpuEncodeSample` | **3.54** | 3.53 | 0.03 (0.99%) | 3.47–3.56 | 283.52 | 216.88 | 557.58 / 589.54 | 0.470 | 120.4 | 33,988,680 (2,435,408–89,196,680) | 1.00× | — | 1 thread | — | — | — | |
| **8K UHD** (`7680x4320`) | **`HostPageableEndToEnd`** | **37.67** | 37.47 | 0.99 (2.63%) | 35.94–38.73 | 38.90 | 38.19 | 52.77 / 71.29 | 4.999 | 1280.3 | 33,988,680 (2,435,408–89,196,680) | **10.63×** | on | 2 | 1951 | 56.2 | 58 | |
| **8K UHD** (`7680x4320`) | `HostPinnedApi` | **45.69** | 45.71 | 0.14 (0.30%) | 45.54–45.94 | 30.29 | 28.72 | 46.98 / 62.01 | 6.063 | 1552.8 | 33,988,680 (2,435,408–89,196,680) | **12.90×** | on | 2 | 1938 | 58.3 | 59 | |
| **8K UHD** (`7680x4320`) | `DeviceInput` *(GPU-resident)* | **94.46** | 94.21 | 0.37 (0.39%) | 93.74–94.60 | 17.33 | 16.26 | 33.75 / 52.90 | 12.535 | 3210.4 | 33,988,680 (2,435,408–89,196,680) | **26.66×** | on | 2 | 1847 | 77.8 | 61 | |

### 2.1 Comparison: Phase 3A.2 (Old) vs. Phase 3A.2a (Corrected Canonical)

| Resolution | Input Mode | Phase 3A.2 Median FPS | Phase 3A.2a Median FPS | FPS Delta (%) | Phase 3A.2 Mean Sample Bytes | Phase 3A.2a Mean Sample Bytes | Attribution of Difference |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | :--- |
| **1080p** | `UpstreamCpuEncodeSample` | 76.98 | 58.25 | -24.33% | 1,757,174 | 1,757,179 | Shared laptop CPU/GPU thermal envelope after 4-mode equivalence gate (consistent with Phase 3A.1's 63.18 FPS) + aligned sequence `4..1003` |
| **1080p** | `HostPageableEndToEnd` | 457.53 | 448.61 | -1.95% | 1,757,177 | 1,757,179 | Normal laptop boost-clock variance (`1840` vs `1827` MHz; stdev 5.9%) + warmup `W=4` vs `W=2` |
| **1080p** | `HostPinnedApi` | 561.31 | 542.73 | -3.31% | 1,757,177 | 1,757,179 | Normal laptop boost-clock variance (stdev 5.3%) + warmup `W=4` vs `W=2` |
| **1080p** | `DeviceInput` | 863.82 | 806.15 | -6.68% | 1,757,177 | 1,757,179 | Normal laptop boost-clock variance (`SwPowerCap`/`GpuIdle` transitions; stdev 7.3%) |
| **4K UHD** | `UpstreamCpuEncodeSample` | 19.65 | 15.93 | -18.93% | 6,640,048 | 6,761,383 | Aligned sequence `4..303` (+1.83% higher mean compressed bytes than `0..299`) + laptop CPU thermal state |
| **4K UHD** | `HostPageableEndToEnd` | 227.07 | 237.21 | +4.47% | 6,717,335 | 6,761,383 | Normal laptop boost-clock variance (`1778` vs `1787` MHz) + aligned sequence `4..303` (`W=4` vs `W=3`) |
| **4K UHD** | `HostPinnedApi` | 294.09 | 291.51 | -0.88% | 6,717,335 | 6,761,383 | Within 1% run-to-run repeatability + aligned sequence `4..303` |
| **4K UHD** | `DeviceInput` | 385.06 | 399.84 | +3.84% | 6,717,335 | 6,761,383 | Normal laptop boost-clock variance + aligned sequence `4..303` |
| **8K UHD** | `UpstreamCpuEncodeSample` | 4.36 | 3.54 | -18.81% | 33,992,443 | 33,988,680 | Laptop CPU thermal state after 1080p/4K/8K equivalence runs (matches Phase 3A.1's `3.597` FPS) + aligned sequence `4..123` |
| **8K UHD** | `HostPageableEndToEnd` | 38.46 | 37.67 | -2.05% | 33,992,290 | 33,988,680 | Normal run variance (stdev 2.6%) + warmup `W=4` vs `W=2` |
| **8K UHD** | `HostPinnedApi` | 45.86 | 45.69 | -0.37% | 33,992,290 | 33,988,680 | Within 0.4% run-to-run repeatability |
| **8K UHD** | `DeviceInput` | 92.46 | 94.46 | +2.16% | 33,992,290 | 33,988,680 | Slightly higher mean GPU clock (`1847` vs `1772` MHz) |

### 2.2 `DefaultAuto` Policy Review vs. Manual Slot Scaling Study (`UserOverride`)

In `SelectPhase2DFrameSlots` ([`src/core/phase2d_encoder.cpp`](../src/core/phase2d_encoder.cpp)):
* **`DefaultAuto` (Canonical Balanced/Default Runtime Policy):**
  - Computes `per_slot_vram_bytes = 20 * px + 4,718,592` and `session_workspace_vram_bytes = 12 * px + 33,554,432`.
  - Computes `vram_max_slots = clamp(((free_vram - (session_workspace_vram_bytes + 256 MiB)) * 3 / 4) / per_slot_vram_bytes, 1, 8)` (`8` on the 16 GB RTX 4090 Laptop GPU).
  - Selects hardcoded resolution-bucket targets from Phase 2D empirical tuning (`target_slots = 2` for $\le 1080\text{p}$, `target_slots = 3` for $\le 4\text{K UHD}$, `target_slots = 2` for $> 4\text{K UHD}$ / $8\text{K UHD}$) and `FrameConsolidated` D2H under `Phase2DD2HPolicy::Throughput`. It does not dynamically optimize slot counts at runtime:
    - **1080p:** `2` slots (`46,190,592` B per slot, `58,437,632` B workspace, `150,818,816` B total estimated VRAM, `144,703,488` B measured VRAM used, `FrameConsolidated` D2H).
    - **4K UHD:** `3` slots (`170,606,592` B per slot, `133,087,232` B workspace, `644,907,008` B total estimated VRAM, `805,306,368` B measured VRAM used, `FrameConsolidated` D2H).
    - **8K UHD:** `2` slots (`668,270,592` B per slot, `431,685,632` B workspace, `1,768,226,816` B total estimated VRAM, `2,101,346,304` B measured VRAM used, `FrameConsolidated` D2H).
* **`UserOverride` (Explicit Tuning / Experimentation Policy):**
  - *The canonical benchmark uses the encoder's `DefaultAuto` slot policy. The slot-scaling study intentionally overrides that policy to measure throughput/latency/VRAM trade-offs and is not the public headline configuration.*

| Resolution | Slots (`UserOverride`) | Matches `DefaultAuto`? | Median FPS (3 trials) | Mean Latency (ms) | Median Latency (ms) | P95 Latency (ms) | Estimated Total VRAM (Bytes / MiB) | Measured VRAM Used (Bytes / MiB) |
| :--- | :---: | :---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **1080p** | 1 | no | 329.96 | 2.86 | 2.38 | 3.94 | 104,628,224 (99.78 MiB) | 73,400,320 (70.00 MiB) |
| **1080p** | **2** | **yes (`DefaultAuto`)** | **473.57** | **3.30** | **3.13** | **4.34** | **150,818,816 (143.83 MiB)** | **144,703,488 (138.00 MiB)** |
| **1080p** | 3 | no | 634.16 | 4.30 | 4.13 | 5.48 | 197,009,408 (187.88 MiB) | 216,006,656 (206.00 MiB) |
| **1080p** | 4 | no | 687.01 | 5.38 | 5.36 | 7.30 | 243,200,000 (231.93 MiB) | 287,309,824 (274.00 MiB) |
| **4K UHD** | 1 | no | 120.88 | 8.19 | 6.88 | 12.98 | 303,693,824 (289.63 MiB) | 268,435,456 (256.00 MiB) |
| **4K UHD** | 2 | no | 152.80 | 9.87 | 9.27 | 13.87 | 474,300,416 (452.33 MiB) | 536,870,912 (512.00 MiB) |
| **4K UHD** | **3** | **yes (`DefaultAuto`)** | **230.81** | **11.88** | **10.72** | **18.05** | **644,907,008 (615.03 MiB)** | **805,306,368 (768.00 MiB)** |
| **4K UHD** | 4 | no | 251.93 | 15.56 | 14.15 | 23.77 | 815,513,600 (777.73 MiB) | 1,073,741,824 (1,024.00 MiB) |
| **8K UHD** | 1 | no | 30.42 | 33.37 | 32.45 | 52.72 | 1,099,956,224 (1,049.00 MiB) | 1,050,673,152 (1,002.00 MiB) |
| **8K UHD** | **2** | **yes (`DefaultAuto`)** | **36.94** | **40.20** | **39.47** | **55.08** | **1,768,226,816 (1,686.31 MiB)** | **2,101,346,304 (2,004.00 MiB)** |
| **8K UHD** | 3 | no | 52.18 | 52.36 | 47.82 | 110.20 | 2,436,497,408 (2,323.63 MiB) | 3,152,019,456 (3,006.00 MiB) |
| **8K UHD** | 4 | no | 60.98 | 64.36 | 62.17 | 119.52 | 3,104,768,000 (2,960.94 MiB) | 4,202,692,608 (4,008.00 MiB) |

---

## 3. Historical Phase 3A.2 Clean-Tree Headline Matrix (`benchmarks/phase3a2_release_candidate.json`)

Generated on commit `4e8037841dcba3c31a6f8d31371869e26f6fbd1b` prior to the Phase 3A.2a sequence-alignment and CMake architecture provenance fixes:

| Resolution | Input Mode | Median FPS | Mean FPS | Stdev (FPS / %) | Min–Max FPS | Mean Latency (ms) | Median Latency (ms) | P95 / P99 Latency (ms) | Raw Input (GB/s) | Output (MB/s) | Speedup vs Upstream CPU | Graph | Slots | Mean GPU Clock (MHz) | Mean Power (W) | Max Temp (°C) | Variance Flag |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: | :---: | ---: | ---: | ---: | :--- |
| **1080p** (`1920x1080`) | `UpstreamCpuEncodeSample` | **76.98** | 77.03 | 0.47 (0.61%) | 76.40–77.59 | 12.98 | 9.26 | 28.17 / 29.43 | 0.639 | 135.3 | 1.00× | — | 1 thread | — | — | — | |
| **1080p** (`1920x1080`) | **`HostPageableEndToEnd`** | **457.53** | 443.35 | 30.03 (6.77%) | 391.99–470.47 | 3.42 | 3.20 | 4.16 / 8.14 | 3.795 | 804.0 | **5.94×** | on | 2 | 1840 | 45.2 | 62 | stdev 6.8% |
| **1080p** (`1920x1080`) | `HostPinnedApi` | **561.31** | 546.87 | 31.02 (5.67%) | 485.01–564.97 | 2.54 | 2.39 | 3.32 / 5.32 | 4.656 | 986.3 | **7.29×** | on | 2 | 1903 | 48.3 | 62 | stdev 5.7% |
| **1080p** (`1920x1080`) | `DeviceInput` | **863.82** | 808.72 | 83.56 (10.33%) | 694.40–883.58 | 2.10 | 1.90 | 3.27 / 6.05 | 7.165 | 1517.9 | **11.22×** | on | 2 | 1732 | 47.9 | 62 | stdev 10.3% |
| **4K UHD** (`3840x2160`) | `UpstreamCpuEncodeSample` | **19.65** | 19.64 | 0.08 (0.42%) | 19.49–19.73 | 50.91 | 34.48 | 113.76 / 117.50 | 0.652 | 130.5 | 1.00× | — | 1 thread | — | — | — | |
| **4K UHD** (`3840x2160`) | **`HostPageableEndToEnd`** | **227.07** | 223.29 | 5.73 (2.57%) | 214.41–228.85 | 12.30 | 11.09 | 17.90 / 29.11 | 7.534 | 1525.3 | **11.56×** | on | 3 | 1778 | 56.4 | 64 | |
| **4K UHD** (`3840x2160`) | `HostPinnedApi` | **294.09** | 288.10 | 14.74 (5.12%) | 258.82–298.28 | 9.21 | 8.04 | 13.70 / 27.28 | 9.757 | 1975.5 | **14.97×** | on | 3 | 1855 | 58.1 | 63 | stdev 5.1% |
| **4K UHD** (`3840x2160`) | `DeviceInput` | **385.06** | 374.84 | 34.53 (9.21%) | 315.57–409.91 | 7.01 | 5.54 | 12.39 / 24.57 | 12.775 | 2586.6 | **19.60×** | on | 3 | 1672 | 56.4 | 64 | stdev 9.2% |
| **8K UHD** (`7680x4320`) | `UpstreamCpuEncodeSample` | **4.36** | 4.36 | 0.01 (0.29%) | 4.35–4.38 | 229.24 | 173.25 | 451.61 / 463.74 | 0.579 | 148.3 | 1.00× | — | 1 thread | — | — | — | |
| **8K UHD** (`7680x4320`) | **`HostPageableEndToEnd`** | **38.46** | 38.80 | 0.60 (1.55%) | 38.06–39.60 | 37.20 | 35.20 | 50.67 / 71.82 | 5.105 | 1307.5 | **8.81×** | on | 2 | 1950 | 57.7 | 62 | |
| **8K UHD** (`7680x4320`) | `HostPinnedApi` | **45.86** | 45.73 | 0.51 (1.11%) | 45.01–46.33 | 29.32 | 28.17 | 44.62 / 64.05 | 6.086 | 1558.9 | **10.51×** | on | 2 | 1960 | 58.6 | 63 | |
| **8K UHD** (`7680x4320`) | `DeviceInput` | **92.46** | 92.68 | 3.19 (3.45%) | 88.24–96.92 | 17.38 | 16.34 | 33.53 / 51.18 | 12.270 | 3142.9 | **21.19×** | on | 2 | 1772 | 80.7 | 64 | |

---

## 4. Historical Phase 3A.1 Diagnostic Audit Record

The three published HostPageableEndToEnd tables in earlier Phase 3A documents were the same encoder path measured under different GPU clocks and different run lengths. They were not three different timing boundaries.

On the checked-in `benchmarks/results/phase3a_sequence.json`, the 60-frame 1080p HostPageable run that printed about 154 FPS held a flat 450 MHz graphics clock and an 810 MHz memory clock for the whole window (about 0.39 s, 13.8 W, `GpuIdle`). The 1,000-frame run that printed 481.6 FPS in that same file, and 462 FPS in the hand-written doc, held 1995 MHz and about an 8 GHz memory clock. H2D on those two records was 2.55 ms versus 0.63 ms. Mean bitstream size was 1.65 MB versus 1.66 MB, so the frames were not a different test. The doc's 169 FPS table was a third short run, copied by hand, and it does not match the JSON.

A fresh process on this machine, after 8K and 4K had already brought the GPU to 1995 MHz, measured 481.7 FPS for 1,000 frames of 1080p HostPageableEndToEnd. That reproduces the old 481 JSON figure. It is a hot single run. The five-trial median in a cold-to-warm session is 440.7 FPS (Phase 3A.1) / 448.61 FPS (Phase 3A.2a). The headline is the median, not 462 and not 481.

4K is not faster than 1080p when both are at boost. Median HostPageable FPS is 448.61 at 1080p and 237.21 at 4K. The old "4K beats 1080p" table compared an idle-clock 1080p run with a boosted 4K run.

8K HostPageable median is 37.67–38.46 FPS, which matches the old ~38–39 FPS. 8K DeviceInput median is 94.46 FPS. The old ~120–127 FPS device figure was not reproduced.

CUDA graph on versus off, both at 1995 MHz, is about +95% at 1080p, +38% at 4K, and +3% at 8K. The old +88% / +56% / +2% story had the right shape. The 4K gain at matched clocks is +38%, not +56%. Hashes match with the graph off and on.

### What was wrong in the pre-3A.1 benchmark

1. **One name, several clocks.** `HostPageableEndToEnd` in the sequence summary, the hand-written matrix, and the long run all called `RunPhase3A` with graph on, dynamic quant, and auto slots. The 60-frame window never left idle clocks. The long run started after the 8K variance study had already heated the GPU.
2. **Hand-copied tables.** `docs/PHASE3A_SEQUENCE_CORRECTNESS.md` did not match `phase3a_sequence.json`. Those tables are removed. This file is the only performance table.
3. **Verification lied when it was off.** `verify_sequence == false` still set `decode_ok`, `stamp_verify_ok`, and PSNR to 42 dB. Skipped verification is now `verification_performed: false` and does not count as a pass.
4. **The timer did not say what it was.** `sustained_fps` is completed frames over the post-warmup wall clock. It includes pageable staging, H2D, encode, D2H, and CPU assembly. It is not a submission rate. With 2 slots, `1000 / mean_latency` is not the FPS.
5. **No explicit drain.** The loop did wait for D2H and assembly before stopping, but it never called `cudaDeviceSynchronize`. The canonical timer now does.
6. **Overflow looked like "not ready."** `PollGpuEntropyFrameD2H` returned false both when the GPU was still running and when the packed word count exceeded the buffer. The second case spins forever in a poll loop. It now returns `Failed`, and the packer refuses to write past the cap.
7. **8K device and 1080p graph percentages were single-condition numbers.** They are replaced by the paired, clock-matched runs below.

### Canonical timing: ProductionEndToEnd / ProductionEndToEnd_v2

`ProductionEndToEnd` (`ProductionEndToEnd_v2` with aligned `W = 4` warmup) is the contract, not a fourth input mode. Every headline FPS number uses it.

The wall clock starts when the first measured frame (`logical_frame_index = 4`) is accepted, after the 4-frame warmup. Warmup is at least the slot count, so the one-time CUDA graph capture (one capture per slot, on the first use of that slot) finishes before the clock starts. The clock stops after the last measured sample has been assembled on the host and `cudaDeviceSynchronize` has returned. A frame counts only when its compressed sample is available to the caller.

Included, when that input mode has them:

- pageable-to-pinned staging
- H2D
- GPU color conversion and wavelet, including quantization inside the wavelet kernels
- GPU entropy
- D2H of the compressed result and the lowpass
- packet assembly and metadata insertion
- the rate-control update that runs before the next wavelet launch
- the final device drain

Excluded, and reported separately when they were measured:

- encoder creation and CUDA context creation
- input-frame generation
- one-time graph capture (`graph_capture_max`, `warmup_excluded_ms`)
- filesystem output

`sequence_fps` is measured frames divided by that wall clock. `steady_state_fps` drops the first and last `slot_count` completions and is labeled separately. It is not the headline.

### Input modes

| Mode | What the caller already did | What the timer still includes |
| :--- | :--- | :--- |
| **HostPageableEndToEnd** | Nothing. Ordinary pageable BGRA. | Staging, H2D, encode, result on the host. This is the primary public benchmark. |
| **HostPinnedApi** | Pinned the buffer. That allocation is outside the timer. | H2D, encode, result on the host. |
| **DeviceInput** | Placed BGRA in device memory. | Encode and the compressed result. This is an integration rate for a caller that already has the frame on the GPU. It is not the file-transcode rate. It is never the only number in the table. |

### Stage times on the median-FPS trial (Phase 3A.1 Reference Breakdown)

These are mean durations of each stage. They overlap. Their sum is not the frame latency and not the frame period. The period column is the wall-clock frame period.

| Stage | 1080p HostPageable | 4K HostPageable | 8K HostPageable | 8K DeviceInput |
| :--- | ---: | ---: | ---: | ---: |
| Sequence FPS | 440.7 | 227.2 | 38.16 | 94.0 |
| Frame period ms | 2.27 | 4.40 | 26.21 | 10.64 |
| Pageable staging ms | 0.50 | 1.73 | 6.54 | 0 |
| H2D ms | 0.65 | 2.58 | 9.95 | 0 |
| Color ms | 0.072 | 0.25 | 0.58 | 0.51 |
| Wavelet, including quant ms | 0.30 | 0.46 | 1.12 | 1.13 |
| Entropy ms | 0.53 | 1.58 | 7.59 | 7.66 |
| Compressed D2H ms | 0.14 | 0.49 | 2.50 | 2.51 |
| Host assembly ms | 0.32 | 1.26 | 4.03 | 3.90 |
| Rate-control update ms | 0.011 | 0.018 | 0.015 | 0.015 |
| Sum of GPU stage averages ms | 1.70 | 5.36 | 21.74 | 11.82 |
| H2D GB/s | 12.7 | 12.8 | 13.3 | 0 |
| Final `cudaDeviceSynchronize` ms | 0.009 | 0.014 | 0.010 | (same drain) |

### CUDA graph

Graph off and graph on used the same frames, the same slot count, and the same streams. Trials alternated off then on so the on-run was not always the warmer one.

| Resolution | Frames | Off median FPS (3A.2a) | On median FPS (3A.2a) | Median delta | Captures |
| :--- | ---: | ---: | ---: | ---: | ---: |
| 1080p | 400 | 235.03 | 472.06 | +100.8% | 2 |
| 4K UHD | 120 | 162.30 | 233.98 | +44.2% | 3 |
| 8K UHD | 48 | 37.45 | 36.33 | -3.0% (within thermal noise; +3.2% in 3A.1) | 2 |

Capture count equals the slot count. Capture time is not inside `sequence_fps`. Separate identity runs with deterministic metadata hashed every frame and confirmed 100% byte identity between Graph OFF and Graph ON across 1080p (`16` frames), 4K UHD (`16` frames), and 8K UHD (`8` frames).

### Quantization and the CUDA graph

1. `FSratelimiter` is computed on the CPU. `PrepareFrameState` calls upstream `SetEncoderQuantization` and `SetTransformQuantization`. The previous frame's sample size is committed by `CompleteFrameState` as `lastgopbitcount = sample_bytes * 8`, which is what `QuantizationSetQuality` reads on the next frame.
2. The 36 quant values and the 36 scale values are generated on the CPU before the wavelet launch.
3. `HyperCine_GpuSlotKickWaveletWithQuant` copies those 36 ints into `slot->run_quant` and launches the wavelet kernels with them. That launch is not inside a CUDA graph.
4. The wavelet kernels are what consume `quant[]`. They write quantized `int16` subband buffers.
5. The entropy graph reads those buffers. Its cache key is the session, band count, stream count, D2H mode, kernel mode, LPT flag, and each band's device pointer, width, height, and pitch. Quant tables are not in the key.
6. Codebooks are uploaded before capture. They are fixed for the subband. A new codebook upload while graphs are enabled synchronizes that stream first.
7. Dynamic parameters across frames are the quant table (host-side, applied at the wavelet launch) and the coefficient values in the fixed device buffers. Pointers inside the graph stay fixed.
8. An `FSratelimiter` change does not recapture the graph. Recapture happens when the key changes: resolution, pointers, stream count, or kernel mode.

### Entropy output bound & Asynchronous errors

- Each highpass subband of `N = height * pitch_px` virtual coefficients has at most `ceil(31*(N+1)/32) <= N+1` 32-bit VLC words and is allocated with `N + 4096` words (`test_entropy_capacity`).
- `GpuStageStatus` is `Pending`, `Complete`, or `Failed`. Any capacity overflow or asynchronous CUDA error transitions the slot to a sticky `Failed` state and terminates the encode loop cleanly rather than spinning indefinitely.
