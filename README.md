# HyperCine

**HyperCine** is an independent, GPU-accelerated CineForm-compatible (`CFHD` / SMPTE VC-5) video encoder and research baseline derived from the open-source [GoPro CineForm SDK](https://github.com/gopro/cineform-sdk).

> **Project Status & Independence Notice:**  
> HyperCine is an **independent derivative engineering and research project**. It is **not** an official GoPro product and is not affiliated with or endorsed by GoPro, Inc. The repository preserves the upstream GoPro CineForm SDK (`upstream/`, commit `11574d0295771edccadd17af14af74a539f924c7`) completely unmodified as a frozen reference oracle and builds a modern C++17 / NVIDIA CUDA encoder pipeline alongside it.

---

## 1. Overview & Technical Scope

HyperCine accelerates the CineForm intra-frame wavelet encoding pipeline on NVIDIA CUDA GPUs while maintaining strict bitstream equivalence with the pristine upstream GoPro CineForm SDK reference encoder (`CFHD_EncodeSample`).

### Supported Scope (Phase 3A.2a Release Candidate)
* **Primary Accelerated Platform:** Windows 10/11 x64 with NVIDIA CUDA GPUs (verified on native `sm_89` / GeForce RTX 4090 Laptop GPU built with `-DCMAKE_CUDA_ARCHITECTURES=89`; configurable via `CMAKE_CUDA_ARCHITECTURES`).
* **CPU-Only Reference Build:** Portable ISO C++17 CPU-only path (`-DHYPERCINE_ENABLE_CUDA=OFF`) compiling the unmodified upstream SDK and CPU reference wrappers. Locally executed and verified on Windows x64 (MSVC 19.44) and configured for GitHub Actions CPU-only CI ([`.github/workflows/ci-cpu.yml`](.github/workflows/ci-cpu.yml) on `windows-latest`). Intended to build on platforms supported by the upstream SDK, though Linux and macOS builds have not yet been executed or verified.
* **Accelerated Pixel & Compression Profile:**
  * **Source Format:** 8-bit `BGRA` (`CFHD_PIXEL_FORMAT_BGRA`, progressive, `GOP = 1`).
  * **Encoded Format:** 12-bit internal `YUV 4:2:2` (`CFHD_ENCODED_FORMAT_YUV_422`, 3 wavelet levels, 10 subbands per channel / 30 subbands total).
  * **Quality Profile:** `FilmScan 3` (`CFHD_ENCODING_QUALITY_FILMSCAN3`).
  * **Rate-Control State Machine:** Sequence-stateful dynamic quantization (`Phase3AQuantMode::DynamicUpstreamState`) matching upstream `QuantizationSetQuality` / `FSratelimiter` transitions on every frame $k$ from frame $k-1$'s encoded bit count (`lastgopbitcount = sample_bytes * 8`).

---

## 2. Architecture Summary

1. **Immutable Upstream Reference (`upstream/`):**  
   The entire `upstream/` directory is frozen at baseline commit `11574d0295771edccadd17af14af74a539f924c7`. No files inside `upstream/` are modified. All compiler flags, warnings, and upstream decoder/threading workarounds are handled in HyperCine's CMake and wrapper layers (documented in [`docs/UPSTREAM.md`](docs/UPSTREAM.md)).
2. **Three Explicit Input Memory Contracts:**
   * **`HostPageableEndToEnd` (Primary Public Benchmark):** Accepts ordinary pageable host `BGRA8` buffers. The timed window includes pageable-to-pinned staging (`memcpy`), asynchronous H2D DMA, GPU color conversion, GPU wavelet + quantization, GPU entropy coding, compressed D2H DMA, and host `CFHD` sample assembly.
   * **`HostPinnedApi`:** Accepts caller-pinned host `BGRA8` buffers (`cudaMallocHost`), skipping pageable staging while still timing H2D DMA, GPU encode, D2H DMA, and host sample assembly.
   * **`DeviceInput` (GPU-Resident Integration Throughput — Non-File-Transcode):** Accepts `BGRA8` frames already residing in GPU device memory (`cudaMalloc`), measuring GPU encode + compressed D2H + host sample assembly. *Note: `DeviceInput` measures GPU-resident integration throughput (e.g., after in-VRAM color grading or compositing) and must never be presented as file-transcode throughput.*
3. **Split Frontend / Wavelet Multi-Slot Overlap & CUDA Graph Execution:**
   * **Frontend Stage:** Asynchronous H2D DMA and `BgraToYuv` (8-bit `BGRA` $\to$ 12-bit internal `YUV 4:2:2`) run on frame $k+1$ in an overlapping slot while frame $k$ completes wavelet + entropy.
   * **Wavelet + Dynamic Quantization Stage (`HyperCine_GpuSlotKickWaveletWithQuant`):** Computes the 3-level 2-6 biorthogonal wavelet pyramid and applies frame $k$'s 36-entry `quant[]` table directly inside the wavelet kernels. This launch executes **outside** the entropy CUDA Graph so per-frame `FSratelimiter` updates never trigger CUDA Graph recapture or node parameter patching.
   * **GPU Entropy Stage (`FusedScanOptimized` + `cudaGraphExec_t`):** Encodes all 27 highpass subbands across 4 concurrent CUDA streams (Longest Processing Time schedule) using a 9-kernel-per-band fused run-length + VLC + radix-1024 prefix-scan pipeline plus 2 frame-payload consolidation kernels (`245` kernel nodes total), captured once per slot into a cached `cudaGraphExec_t` and replayed via a single `cudaGraphLaunch` per frame.
   * **Proven Entropy Word Capacity & Sticky Error Propagation:** Each highpass subband of $N = \text{height} \times \text{pitch\_px}$ coefficients is analytically bounded by $\lceil 31(N+1)/32 \rceil \le N+1$ 32-bit VLC words and allocated with $N + 4096$ words (see [`docs/ENTROPY.md`](docs/ENTROPY.md)). Any kernel capacity violation or asynchronous CUDA error transitions `GpuStageStatus` (`Pending` / `Complete` / `Failed`) to a sticky `Failed` state rather than spinning indefinitely.

---

## 3. Canonical Benchmark Summary (`ProductionEndToEnd_v2`)

### Methodology (`ProductionEndToEnd_v2`)
All headline numbers are generated from a clean git working tree by [`benchmarks/benchmark_phase3a1.cpp`](benchmarks/benchmark_phase3a1.cpp) and stored in [`benchmarks/phase3a2a_release_candidate.json`](benchmarks/phase3a2a_release_candidate.json) (benchmarked source commit `fba948edf98b72045af150b1a918fd0d8a643c61`, `"publication": "CLEAN"`, `"canonical": true`, `"git_status_clean": true`, `"upstream_status_clean": true`, `"benchmark_equivalence_verified": true`, `"native_architecture_match": true`).

* **Timed Boundary (`ProductionEndToEnd`):** Starts when the first measured frame (`logical_frame_index = 4`) is submitted after the common warmup prefix (`warmup_frames = 4 >= slot_count`, so one-time CUDA Graph capture is excluded and reported separately). For `HostPageableEndToEnd`, the timed window includes ordinary pageable source $\to$ pageable-to-pinned staging $\to$ H2D DMA $\to$ GPU frontend $\to$ wavelet + dynamic quantization $\to$ GPU entropy $\to$ compressed D2H DMA $\to$ host `CFHD` sample assembly $\to$ final worker drain and `cudaDeviceSynchronize()`. Encoder creation, CUDA context creation, input-frame generation, one-time CUDA Graph capture, and filesystem output are excluded.
* **Aligned Warmup & Sequence Definition (`ProductionEndToEnd_v2`):** Both `UpstreamCpuEncodeSample` and all three HyperCine GPU modes execute the exact same `W = 4` warmup frames (`pool[0..3]`) followed without reset or restart by `N` timed frames starting at logical sequence index `4` (`pool[(4 + i) % unique_cycle]`: `N = 1000` over a `96`-frame cycle at 1080p, `N = 300` over a `36`-frame cycle at 4K UHD, and `N = 120` over a `12`-frame cycle at 8K UHD). Each configuration runs **5 trials** with per-trial GPU input-mode permutation; the **5-trial median** is the canonical headline figure.
* **Apples-to-Apples Stateful Equivalence & Output-Size Fairness Gate:** Before headline trials are marked canonical, `benchmark_phase3a1` verifies on the shared sequence (`W = 4`, first measured logical frame `4`) that `UpstreamCpuEncodeSample`, `HostPageableEndToEnd`, `HostPinnedApi`, and `DeviceInput` produce identical per-frame logical indices, patterns, `prev_encoded_bytes`, `lastgopbitcount`, `FSratelimiter` trajectories, 36-entry `quant[]` / `scale[]` tables, per-frame sample byte counts, and masked `CFHD` sample SHA-256 digests (`24/24` at 1080p, `24/24` at 4K UHD, `12/12` at 8K UHD), and that full-sequence headline `mean_sample_bytes`, `min_sample_bytes`, `max_sample_bytes`, and `distinct_fs_rate_limiters` match 100% across all four modes.
* **CPU Baseline Definition:** Baseline 1 (`UpstreamCpuEncodeSample`) is the pristine upstream `CFHD_EncodeSample` compiled from unmodified `upstream/` with its default `_THREADED_ENCODER=0` (single-threaded encoder) and MSVC Release `/O2` optimization. As documented in [`docs/UPSTREAM.md`](docs/UPSTREAM.md), upstream's optional `CFHDCodecThreaded` (`_THREADED_ENCODER=1`) target is marked `EXCLUDE_FROM_ALL` upstream and does not compile due to missing legacy symbols. Neither upstream nor HyperCine forces `/arch:AVX2` in CMake.

### Canonical 5-Trial Results (`benchmarks/phase3a2a_release_candidate.json`)

**Test System:** AMD Ryzen 9 7940HS (16 logical processors, 33.5 GB RAM), NVIDIA GeForce RTX 4090 Laptop GPU (runtime `sm_89`, built natively with `-DCMAKE_CUDA_ARCHITECTURES=89` $\to$ cubin `sm_89` + PTX `compute_89`, 16,375 MiB VRAM, NVIDIA Driver `610.62`, CUDA `12.8.61`), Windows 10.0 Build 26200 x64, MSVC `19.44.35228.0` (`Release /O2`), AC power online.

| Resolution | Input Mode | Median FPS | Mean FPS | Stdev (FPS / %) | Min–Max FPS | Mean Latency (ms) | Median Latency (ms) | P95 / P99 Latency (ms) | Raw Input (GB/s) | Output (MB/s) | Mean Sample Bytes (Min–Max) | Speedup vs Upstream CPU | Graph | Slots (`DefaultAuto`) | Mean GPU Clock (MHz) | Mean Power (W) | Max Temp (°C) |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: | :---: | ---: | ---: | ---: |
| **1080p** (`1920x1080`) | `UpstreamCpuEncodeSample` | **58.25** | 57.42 | 1.50 (2.61%) | 54.57–58.72 | 17.43 | 13.30 | 36.85 / 42.30 | 0.483 | 102.4 | 1,757,179 (180,440–5,494,332) | 1.00× | — | 1 thread | — | — | — |
| **1080p** (`1920x1080`) | **`HostPageableEndToEnd`** | **448.61** | 441.23 | 25.97 (5.89%) | 401.18–472.37 | 3.52 | 3.30 | 4.51 / 7.28 | 3.721 | 788.3 | 1,757,179 (180,440–5,494,332) | **7.70×** | on | 2 | 1827 | 44.0 | 61 |
| **1080p** (`1920x1080`) | `HostPinnedApi` | **542.73** | 531.53 | 28.23 (5.31%) | 476.63–554.77 | 2.74 | 2.54 | 3.91 / 5.98 | 4.502 | 953.7 | 1,757,179 (180,440–5,494,332) | **9.32×** | on | 2 | 1911 | 47.8 | 62 |
| **1080p** (`1920x1080`) | `DeviceInput` *(GPU-resident)* | **806.15** | 775.13 | 56.87 (7.34%) | 669.63–827.36 | 2.24 | 2.05 | 3.60 / 5.49 | 6.687 | 1416.6 | 1,757,179 (180,440–5,494,332) | **13.84×** | on | 2 | 1796 | 48.9 | 61 |
| **4K UHD** (`3840x2160`) | `UpstreamCpuEncodeSample` | **15.93** | 15.88 | 0.13 (0.79%) | 15.69–16.00 | 62.99 | 47.39 | 136.67 / 148.04 | 0.528 | 107.7 | 6,761,383 (631,824–22,236,848) | 1.00× | — | 1 thread | — | — | — |
| **4K UHD** (`3840x2160`) | **`HostPageableEndToEnd`** | **237.21** | 228.10 | 16.24 (7.12%) | 196.74–239.84 | 11.94 | 10.86 | 17.20 / 36.67 | 7.870 | 1603.9 | 6,761,383 (631,824–22,236,848) | **14.89×** | on | 3 | 1787 | 56.7 | 61 |
| **4K UHD** (`3840x2160`) | `HostPinnedApi` | **291.51** | 282.39 | 19.60 (6.94%) | 243.26–293.49 | 9.40 | 8.18 | 13.73 / 29.28 | 9.671 | 1971.0 | 6,761,383 (631,824–22,236,848) | **18.30×** | on | 3 | 1864 | 56.1 | 62 |
| **4K UHD** (`3840x2160`) | `DeviceInput` *(GPU-resident)* | **399.84** | 377.62 | 41.32 (10.94%) | 295.84–404.62 | 6.98 | 5.38 | 12.90 / 37.59 | 13.266 | 2703.5 | 6,761,383 (631,824–22,236,848) | **25.10×** | on | 3 | 1642 | 51.0 | 63 |
| **8K UHD** (`7680x4320`) | `UpstreamCpuEncodeSample` | **3.54** | 3.53 | 0.03 (0.99%) | 3.47–3.56 | 283.52 | 216.88 | 557.58 / 589.54 | 0.470 | 120.4 | 33,988,680 (2,435,408–89,196,680) | 1.00× | — | 1 thread | — | — | — |
| **8K UHD** (`7680x4320`) | **`HostPageableEndToEnd`** | **37.67** | 37.47 | 0.99 (2.63%) | 35.94–38.73 | 38.90 | 38.19 | 52.77 / 71.29 | 4.999 | 1280.3 | 33,988,680 (2,435,408–89,196,680) | **10.63×** | on | 2 | 1951 | 56.2 | 58 |
| **8K UHD** (`7680x4320`) | `HostPinnedApi` | **45.69** | 45.71 | 0.14 (0.30%) | 45.54–45.94 | 30.29 | 28.72 | 46.98 / 62.01 | 6.063 | 1552.8 | 33,988,680 (2,435,408–89,196,680) | **12.90×** | on | 2 | 1938 | 58.3 | 59 |
| **8K UHD** (`7680x4320`) | `DeviceInput` *(GPU-resident)* | **94.46** | 94.21 | 0.37 (0.39%) | 93.74–94.60 | 17.33 | 16.26 | 33.75 / 52.90 | 12.535 | 3210.4 | 33,988,680 (2,435,408–89,196,680) | **26.66×** | on | 2 | 1847 | 77.8 | 61 |

#### Baseline Comparison Interpretation
On the tested RTX 4090 Laptop GPU system, HyperCine `HostPageableEndToEnd` (`ProductionEndToEnd_v2`) achieved **7.70×** (`448.61 FPS` vs `58.25 FPS` at 1080p), **14.89×** (`237.21 FPS` vs `15.93 FPS` at 4K UHD), and **10.63×** (`37.67 FPS` vs `3.54 FPS` at 8K UHD) the median throughput of the pristine single-threaded GoPro CineForm SDK encoder under the documented benchmark configuration. Caller-pinned host memory (`HostPinnedApi`) reached **9.32×** / **18.30×** / **12.90×**, and GPU-resident `DeviceInput` (in-VRAM integration throughput, not file transcode) reached **13.84×** / **25.10×** / **26.66×** (`806.15` / `399.84` / `94.46` FPS).

### `DefaultAuto` Runtime Slot Policy vs. Manual Slot Scaling Study

HyperCine distinguishes between two slot-selection policies in `SelectPhase2DFrameSlots` ([`src/core/phase2d_encoder.cpp`](src/core/phase2d_encoder.cpp)):
* **`DefaultAuto` (Canonical Balanced/Default Runtime Policy):** Used by default in `RunPhase3A` and for all canonical headline results above. It computes `per_slot_vram_bytes = 20 * px + 4,718,592` and `session_workspace_vram_bytes = 12 * px + 33,554,432`, caps `vram_max_slots` to 75% of available free VRAM after reserving workspace + 256 MiB, and selects a hardcoded resolution-bucket target (`2` slots for $\le 1080\text{p}$, `3` slots for $\le 4\text{K UHD}$, `2` slots for $> 4\text{K UHD}$ / $8\text{K UHD}$) with `FrameConsolidated` D2H under `Phase2DD2HPolicy::Throughput`. It does not dynamically tune slot count per GPU model at runtime:
  * **1080p (`DefaultAuto`):** `2` slots (`46,190,592` B [44.05 MiB] estimated per slot, `58,437,632` B [55.73 MiB] workspace, `150,818,816` B [143.83 MiB] total estimated VRAM, `144,703,488` B [138.00 MiB] measured VRAM used, `FrameConsolidated` D2H).
  * **4K UHD (`DefaultAuto`):** `3` slots (`170,606,592` B [162.70 MiB] estimated per slot, `133,087,232` B [126.92 MiB] workspace, `644,907,008` B [615.03 MiB] total estimated VRAM, `805,306,368` B [768.00 MiB] measured VRAM used, `FrameConsolidated` D2H).
  * **8K UHD (`DefaultAuto`):** `2` slots (`668,270,592` B [637.31 MiB] estimated per slot, `431,685,632` B [411.69 MiB] workspace, `1,768,226,816` B [1,686.31 MiB] total estimated VRAM, `2,101,346,304` B [2,004.00 MiB] measured VRAM used, `FrameConsolidated` D2H).
* **`UserOverride` (Explicit Tuning / Experimentation Policy):** Allows callers to specify an explicit slot count (`1..4`).

#### Manual Slot Scaling / Throughput Tuning Study (`UserOverride` — Non-Headline)
> *The canonical benchmark uses the encoder's `DefaultAuto` slot policy. The slot-scaling study intentionally overrides that policy (`Phase2DSlotPolicy::UserOverride`) on `HostPageableEndToEnd` to measure throughput/latency/VRAM trade-offs and is not the public headline configuration.*

| Resolution | Slots (`UserOverride`) | Matches `DefaultAuto`? | Median FPS (3 trials) | Mean Latency (ms) | Median Latency (ms) | P95 Latency (ms) | Estimated Total VRAM | Measured VRAM Used (Median) |
| :--- | :---: | :---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **1080p** (`400` frames) | 1 | no | 329.96 | 2.86 | 2.38 | 3.94 | 104.63 MB (99.78 MiB) | 73.40 MB (70.00 MiB) |
| **1080p** (`400` frames) | **2** | **yes (`DefaultAuto`)** | **473.57** | **3.30** | **3.13** | **4.34** | **150.82 MB (143.83 MiB)** | **144.70 MB (138.00 MiB)** |
| **1080p** (`400` frames) | 3 | no | 634.16 | 4.30 | 4.13 | 5.48 | 197.01 MB (187.88 MiB) | 216.01 MB (206.00 MiB) |
| **1080p** (`400` frames) | 4 | no | 687.01 | 5.38 | 5.36 | 7.30 | 243.20 MB (231.93 MiB) | 287.31 MB (274.00 MiB) |
| **4K UHD** (`120` frames) | 1 | no | 120.88 | 8.19 | 6.88 | 12.98 | 303.69 MB (289.63 MiB) | 268.44 MB (256.00 MiB) |
| **4K UHD** (`120` frames) | 2 | no | 152.80 | 9.87 | 9.27 | 13.87 | 474.30 MB (452.33 MiB) | 536.87 MB (512.00 MiB) |
| **4K UHD** (`120` frames) | **3** | **yes (`DefaultAuto`)** | **230.81** | **11.88** | **10.72** | **18.05** | **644.91 MB (615.03 MiB)** | **805.31 MB (768.00 MiB)** |
| **4K UHD** (`120` frames) | 4 | no | 251.93 | 15.56 | 14.15 | 23.77 | 815.51 MB (777.73 MiB) | 1,073.74 MB (1,024.00 MiB) |
| **8K UHD** (`48` frames) | 1 | no | 30.42 | 33.37 | 32.45 | 52.72 | 1,099.96 MB (1,049.00 MiB) | 1,050.67 MB (1,002.00 MiB) |
| **8K UHD** (`48` frames) | **2** | **yes (`DefaultAuto`)** | **36.94** | **40.20** | **39.47** | **55.08** | **1,768.23 MB (1,686.31 MiB)** | **2,101.35 MB (2,004.00 MiB)** |
| **8K UHD** (`48` frames) | 3 | no | 52.18 | 52.36 | 47.82 | 110.20 | 2,436.50 MB (2,323.63 MiB) | 3,152.02 MB (3,006.00 MiB) |
| **8K UHD** (`48` frames) | 4 | no | 60.98 | 64.36 | 62.17 | 119.52 | 3,104.77 MB (2,960.94 MiB) | 4,202.69 MB (4,008.00 MiB) |

Full per-stage timing, CUDA Graph off-vs-on deltas, and historical Phase 3A.2 vs. Phase 3A.2a comparisons are documented in [`docs/PHASE3A1_BENCHMARK_HARDENING.md`](docs/PHASE3A1_BENCHMARK_HARDENING.md).

---

## 4. Build, Test & Benchmark Instructions

### Prerequisites
* **OS:** Windows 10/11 x64 (locally executed and verified; portable ISO C++17 CPU-only path is intended to build on upstream SDK platforms, though Linux and macOS builds have not yet been executed or verified)
* **Build System:** CMake 3.25+
* **C++ Compiler:** Visual Studio 2022 (MSVC 19.30+) with C++17 support
* **CUDA (for GPU acceleration):** NVIDIA CUDA Toolkit 12.x+ and a compatible NVIDIA GPU driver

### Option A: CPU-Only Reference Build (`HYPERCINE_ENABLE_CUDA=OFF`)
Builds the unmodified upstream GoPro CineForm SDK (`CFHDCodecStatic`) and HyperCine CPU reference wrappers without requiring the CUDA Toolkit:

```powershell
cmake -B build-cpu -S . -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=ON
cmake --build build-cpu --config Release
ctest --test-dir build-cpu -C Release --output-on-failure
```
*Expected result: 4 / 4 CTest suites pass (`test_transform_wavelet`, `test_codec_roundtrip`, `test_yuy2_stability`, `test_avi_sample`).*

### Option B: Full NVIDIA CUDA Release Build (`HYPERCINE_ENABLE_CUDA=ON`)
Builds all CPU and CUDA libraries, all 14 verification suites, and all benchmark harnesses (adjust `-DCMAKE_CUDA_ARCHITECTURES` to match your GPU, e.g., `89` for Ada Lovelace, `86` for Ampere):

```powershell
cmake -B build-release-sm89 -S . -DHYPERCINE_ENABLE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89 -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=ON
cmake --build build-release-sm89 --config Release
ctest --test-dir build-release-sm89 -C Release --output-on-failure
```
*Expected result: 14 / 14 CTest suites pass (`100%`), including `test_entropy_capacity` and `test_phase3a`.*

### Running CUDA Sanitizer Verification
```powershell
compute-sanitizer --tool memcheck .\build-release-sm89\tests\Release\test_entropy_capacity.exe
compute-sanitizer --tool memcheck .\build-release-sm89\tests\Release\test_phase3a.exe --quick-sanitize
```
*(Note: On Windows, `compute-sanitizer` uses a local `127.0.0.1` loopback TCP socket for IPC with the target binary, which may trigger a one-time Windows Firewall prompt; no external network access is used.)*

### Running the Canonical Benchmark Harness
`benchmark_phase3a1.exe` refuses to emit a canonical benchmark file if `git status --porcelain` or `git status --porcelain upstream/` is non-empty unless `--allow-dirty` is explicitly passed:

```powershell
# Canonical 5-trial benchmark suite (equivalence + headline + slots + graph) on a clean working tree:
.\build-release-sm89\benchmarks\Release\benchmark_phase3a1.exe --suite=all --trials=5 --out=benchmarks/phase3a2a_release_candidate.json

# Exploratory run on a modified working tree (marked "publication": "DIRTY"):
.\build-release-sm89\benchmarks\Release\benchmark_phase3a1.exe --allow-dirty --suite=headline --trials=1 --out=benchmarks/phase3a1_tmp.json
```

---

## 5. Current Limitations

To ensure transparent technical evaluation, please note the following scope boundaries of the Phase 3A.2a release candidate:

1. **Platform & Vendor Scope:** GPU acceleration currently targets **Windows x64 + NVIDIA CUDA** only. Vulkan (`src/vulkan/`) and Apple Metal (`src/metal/`) backends are planned for future phases and are not implemented in this release. The CPU-only reference path (`-DHYPERCINE_ENABLE_CUDA=OFF`) has been locally executed and verified on Windows x64 (MSVC 19.44) and is configured in [`.github/workflows/ci-cpu.yml`](.github/workflows/ci-cpu.yml) for `windows-latest`; Linux and macOS builds have not yet been executed or verified.
2. **Pixel Format & Compression Profile Scope:** The full GPU pipeline (`Phase3AEncoderContext`) currently accelerates 8-bit `BGRA` (`CFHD_PIXEL_FORMAT_BGRA`) $\to$ 12-bit internal `YUV 4:2:2` (`CFHD_ENCODED_FORMAT_YUV_422`) at `FilmScan 3` (`CFHD_ENCODING_QUALITY_FILMSCAN3`), progressive, `GOP = 1`. Other CineForm pixel formats (`v210`, `RG48`, `b64a`, `BYR4` Bayer RAW, `YUV 4:4:4`, alpha channels) fall back to the upstream CPU SDK.
3. **Encoder-Only GPU Acceleration:** GPU acceleration is implemented for **encoding**. Decoding in verification tests uses the upstream GoPro CineForm CPU decoder (`CFHD_DecodeSample` with `TAG_CPU_MAX = 1` to avoid upstream's multi-threaded decoder race conditions documented in [`docs/UPSTREAM.md`](docs/UPSTREAM.md)).
4. **Research/Engineering API Packaging:** The multi-frame GPU encoder is exposed via internal C++ headers (`include/hypercine/phase3a.h`, `RunPhase3A`) rather than a drop-in DirectShow/QuickTime/VFW system installer or commercial NLE plugin bundle.
5. **External NLE & FFmpeg Interoperability:** Bitstream compliance is verified by byte-for-byte masked equivalence (`masked_diff_bytes == 0`) against pristine upstream `CFHD_EncodeSample` and round-trip decoding through `CFHD_DecodeSample`. Automated CI testing inside Adobe Premiere Pro, DaVinci Resolve, or FFmpeg (`cfhd`) is not part of the repository test runner.
6. **Volatile Clip Metadata in Cross-Handle Comparisons:** In production mode (`deterministic_test_metadata = false`, the default), every encode writes live `TAG_CLIP_GUID`, `TAG_ENCODE_DATE`, `TAG_ENCODE_TIME`, and `TAG_TIMECODE` tags into the `CFHD` header. Cross-handle byte-identity tests mask only those 4 volatile metadata fields (`MaskCfhdVolatileMetadata`).
7. **Laptop GPU Clock & Thermal Variance:** On mobile GPUs (such as the tested RTX 4090 Laptop GPU), dynamic boost clocks and power/thermal caps (`SwPowerCap`, `SwThermalSlowdown`, or `GpuIdle` clock transitions between trials) introduce run-to-run variance (`~5–11%` stdev at 1080p/4K). All benchmark outputs therefore record 5-trial medians alongside full NVML clock, power, temperature, and throttle telemetry.

---

## 6. Documentation Map

* [`docs/PHASE3A1_BENCHMARK_HARDENING.md`](docs/PHASE3A1_BENCHMARK_HARDENING.md): Authoritative Phase 3A.1 / 3A.2 / 3A.2a `ProductionEndToEnd_v2` benchmark methodology, sequence-alignment & architecture provenance corrections, clean-tree headline tables, stage breakdowns, CUDA Graph off/on study, and slot scaling.
* [`docs/PHASE3A_SEQUENCE_CORRECTNESS.md`](docs/PHASE3A_SEQUENCE_CORRECTNESS.md): 120-frame changing-complexity rate-control equivalence (`FSratelimiter`), 300-frame multi-slot stamp verification, and 17-pattern adversarial entropy verification.
* [`docs/ENTROPY.md`](docs/ENTROPY.md): Run-length + VLC entropy architecture, analytical word capacity bound ($\lceil 31(N+1)/32 \rceil \le N+1$), and `GpuStageStatus` (`Pending` / `Complete` / `Failed`) error propagation contract.
* [`docs/CUDA_PHASE2D.md`](docs/CUDA_PHASE2D.md): Phase 2D `FSratelimiter` (`Frame1Initial` vs `SteadyState`) root-cause analysis and `2D-A -> 2D-B -> 2D-C` GPU entropy kernel fusion progression.
* [`docs/PHASE3B_PERFORMANCE_OPPORTUNITIES.md`](docs/PHASE3B_PERFORMANCE_OPPORTUNITIES.md): Deferred Phase 3B optimization opportunities identified during Phase 3A.1 benchmarking.
* [`docs/UPSTREAM.md`](docs/UPSTREAM.md): Upstream GoPro CineForm SDK provenance (`11574d0295771edccadd17af14af74a539f924c7`), immutability rules, and documented upstream quirks (`CFHDCodecThreaded` build failure and multi-threaded decoder races).
* [`docs/COMPATIBILITY.md`](docs/COMPATIBILITY.md): Format support matrix distinguishing upstream CPU SDK capabilities from the current HyperCine CUDA encoder scope.
* [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md): High-level CineForm 2-6 biorthogonal wavelet and pipeline overview.
* [`benchmarks/results/README.md`](benchmarks/results/README.md): Guide to canonical vs. historical benchmark JSON files.
* [`CONTRIBUTING.md`](CONTRIBUTING.md) & [`SECURITY.md`](SECURITY.md): Contribution rules, TDD gates, and security reporting policy.

---

## 7. License & Attribution

HyperCine is dual-licensed under the **MIT License** or the **Apache License, Version 2.0**, at your option:
* [`LICENSE-MIT`](LICENSE-MIT)
* [`LICENSE-APACHE`](LICENSE-APACHE)
* [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)

Original CineForm SDK (`upstream/`): Copyright (c) 2017 GoPro, Inc. (dual-licensed under MIT or Apache-2.0; see [`upstream/LICENSE.txt`](upstream/LICENSE.txt) and [`upstream/LICENSE-APACHE.txt`](upstream/LICENSE-APACHE.txt)).  
HyperCine additions: Copyright (c) 2026 HyperCine Contributors.
