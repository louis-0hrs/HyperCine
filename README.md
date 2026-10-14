# HyperCine

**HyperCine** is an independent, GPU-accelerated CineForm-compatible (`CFHD` / SMPTE VC-5) video encoder and research baseline derived from the open-source [GoPro CineForm SDK](https://github.com/gopro/cineform-sdk).

> **Project Status & Independence Notice:**  
> HyperCine is an **independent derivative engineering and research project**. It is **not** an official GoPro product and is not affiliated with or endorsed by GoPro, Inc. The repository preserves the upstream GoPro CineForm SDK (`upstream/`, commit `11574d0295771edccadd17af14af74a539f924c7`) completely unmodified as a frozen reference oracle and builds a modern C++17 / NVIDIA CUDA encoder pipeline alongside it.

---

## 1. Overview & Technical Scope

HyperCine accelerates the CineForm intra-frame wavelet encoding pipeline on NVIDIA CUDA GPUs while maintaining strict bitstream equivalence with the pristine upstream GoPro CineForm SDK reference encoder (`CFHD_EncodeSample`).

### Supported Scope (HyperCine v0.1.0-alpha)
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

#### Methodology (`ProductionEndToEnd_v2`)
All headline numbers are generated from a clean git working tree by [`benchmarks/benchmark_phase3a1.cpp`](benchmarks/benchmark_phase3a1.cpp) and stored in [`benchmarks/phase3a2c_release_candidate.json`](benchmarks/phase3a2c_release_candidate.json) (benchmarked source commit `57ca3f37be4f017af8a72592e042b2fe285416a3`, `"publication": "CLEAN"`, `"canonical": true`, `"benchmark_tree_clean": true`, `"upstream_tree_clean": true`, `"benchmark_equivalence_verified": true`, `"native_architecture_match": true`).

> **Provenance Note (Source vs. Release Revision):** The benchmark executable was built natively for sm_89 and executed from source commit `57ca3f37be4f017af8a72592e042b2fe285416a3` on a 100% clean working tree with benchmark output written outside the repository (`%TEMP%\phase3a2c_release_candidate.json`). The generated benchmark artifact and updated documentation were then committed without changing production source, CUDA kernels, tests, benchmark source, or build logic. The annotated release-candidate tag `phase3a2c-release-candidate` points to the resulting documentation/artifact commit.

* **Timed Boundary (`ProductionEndToEnd`):** Starts when the first measured frame (`logical_frame_index = 4`) is submitted after the common warmup prefix (`warmup_frames = 4 >= slot_count`, so one-time CUDA Graph capture is excluded and reported separately). For `HostPageableEndToEnd`, the timed window includes ordinary pageable source $\to$ pageable-to-pinned staging $\to$ H2D DMA $\to$ GPU frontend $\to$ wavelet + dynamic quantization $\to$ GPU entropy $\to$ compressed D2H DMA $\to$ host `CFHD` sample assembly $\to$ final worker drain and `cudaDeviceSynchronize()`. Encoder creation, CUDA context creation, input-frame generation, one-time CUDA Graph capture, and filesystem output are excluded.
* **Aligned Warmup & Sequence Definition (`ProductionEndToEnd_v2`):** Both `UpstreamCpuEncodeSample` and all three HyperCine GPU modes execute the exact same `W = 4` warmup frames (`pool[0..3]`) followed without reset or restart by `N` timed frames starting at logical sequence index `4` (`pool[(4 + i) % unique_cycle]`: `N = 1000` over a `96`-frame cycle at 1080p, `N = 300` over a `36`-frame cycle at 4K UHD, and `N = 120` over a `12`-frame cycle at 8K UHD). Each configuration runs **5 trials** with per-trial GPU input-mode permutation; the **5-trial median** is the canonical headline figure.
* **Apples-to-Apples Stateful Equivalence & Output-Size Fairness Gate:** Before headline trials are marked canonical, `benchmark_phase3a1` verifies on the shared sequence (`W = 4`, first measured logical frame `4`) that `UpstreamCpuEncodeSample`, `HostPageableEndToEnd`, `HostPinnedApi`, and `DeviceInput` produce identical per-frame logical indices, patterns, `prev_encoded_bytes`, `lastgopbitcount`, `FSratelimiter` trajectories, 36-entry `quant[]` / `scale[]` tables, per-frame sample byte counts, and masked `CFHD` sample SHA-256 digests (`24/24` at 1080p, `24/24` at 4K UHD, `12/12` at 8K UHD), and that full-sequence headline `mean_sample_bytes`, `min_sample_bytes`, `max_sample_bytes`, and `distinct_fs_rate_limiters` match 100% across all four modes.
* **CPU Baseline Definition:** Baseline 1 (`UpstreamCpuEncodeSample`) is the pristine upstream `CFHD_EncodeSample` compiled from unmodified `upstream/` with its default `_THREADED_ENCODER=0` (single-threaded encoder) and MSVC Release `/O2` optimization. As documented in [`docs/UPSTREAM.md`](docs/UPSTREAM.md), upstream's optional `CFHDCodecThreaded` (`_THREADED_ENCODER=1`) target is marked `EXCLUDE_FROM_ALL` upstream and does not compile due to missing legacy symbols. Neither upstream nor HyperCine forces `/arch:AVX2` in CMake.

### Canonical 5-Trial Results (`benchmarks/phase3a2c_release_candidate.json`)

**Test System:** AMD Ryzen 9 7940HS (16 logical processors, 33.5 GB RAM), NVIDIA GeForce RTX 4090 Laptop GPU (runtime `sm_89`, built natively with `-DCMAKE_CUDA_ARCHITECTURES=89` $\to$ cubin `sm_89` + PTX `compute_89`, 16,375 MiB VRAM, NVIDIA Driver `610.62`, CUDA `12.8.61`), Windows 10.0 Build 26200 x64, MSVC `19.44.35228.0` (`Release /O2`), AC power online.

| Resolution | Input Mode | Median FPS | Mean FPS | Stdev (FPS / %) | Min–Max FPS | Mean Latency (ms) | Median Latency (ms) | P95 / P99 Latency (ms) | Raw Input (GB/s) | Output (MB/s) | Mean Sample Bytes (Min–Max) | Speedup vs Upstream CPU | Graph | Slots (`DefaultAuto`) | Mean GPU Clock (MHz) | Mean Power (W) | Max Temp (°C) |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: | :---: | ---: | ---: | ---: |
| **1080p** (`1920x1080`) | `UpstreamCpuEncodeSample` | **63.27** | 65.53 | 4.93 (7.52%) | 60.26–74.39 | 15.34 | 11.58 | 33.53 / 36.55 | 0.525 | 111.2 | 1,757,179 (180,440–5,494,332) | 1.00× | — | 1 thread | — | — | — |
| **1080p** (`1920x1080`) | **`HostPageableEndToEnd`** | **450.65** | 442.29 | 17.00 (3.84%) | 414.08–461.93 | 3.55 | 3.34 | 4.75 / 7.70 | 3.738 | 791.9 | 1,757,179 (180,440–5,494,332) | **7.12×** | on | 2 | 1822 | 44.6 | 61 |
| **1080p** (`1920x1080`) | `HostPinnedApi` | **551.37** | 540.28 | 24.70 (4.57%) | 491.31–556.73 | 2.66 | 2.48 | 3.75 / 6.22 | 4.573 | 968.9 | 1,757,179 (180,440–5,494,332) | **8.71×** | on | 2 | 1899 | 48.7 | 61 |
| **1080p** (`1920x1080`) | `DeviceInput` *(GPU-resident)* | **792.82** | 749.39 | 66.80 (8.91%) | 649.74–819.99 | 2.31 | 2.07 | 3.81 / 6.82 | 6.576 | 1393.1 | 1,757,179 (180,440–5,494,332) | **12.53×** | on | 2 | 1702 | 47.7 | 62 |
| **4K UHD** (`3840x2160`) | `UpstreamCpuEncodeSample` | **17.52** | 17.91 | 1.28 (7.17%) | 16.46–19.94 | 56.13 | 40.81 | 124.92 / 137.64 | 0.581 | 118.4 | 6,761,383 (631,824–22,236,848) | 1.00× | — | 1 thread | — | — | — |
| **4K UHD** (`3840x2160`) | **`HostPageableEndToEnd`** | **222.56** | 219.14 | 14.61 (6.67%) | 200.89–234.74 | 12.43 | 10.91 | 17.70 / 50.95 | 7.384 | 1504.8 | 6,761,383 (631,824–22,236,848) | **12.71×** | on | 3 | 1786 | 50.3 | 63 |
| **4K UHD** (`3840x2160`) | `HostPinnedApi` | **290.27** | 269.37 | 42.40 (15.74%) | 184.75–293.96 | 10.16 | 9.21 | 15.60 / 30.22 | 9.631 | 1962.6 | 6,761,383 (631,824–22,236,848) | **16.57×** | on | 3 | 1901 | 56.0 | 63 |
| **4K UHD** (`3840x2160`) | `DeviceInput` *(GPU-resident)* | **393.60** | 380.38 | 23.71 (6.23%) | 341.95–405.29 | 6.86 | 5.49 | 12.32 / 24.29 | 13.059 | 2661.3 | 6,761,383 (631,824–22,236,848) | **22.47×** | on | 3 | 1642 | 54.6 | 64 |
| **8K UHD** (`7680x4320`) | `UpstreamCpuEncodeSample` | **3.61** | 3.60 | 0.04 (1.17%) | 3.55–3.66 | 277.89 | 207.05 | 567.99 / 592.38 | 0.479 | 122.7 | 33,988,680 (2,435,408–89,196,680) | 1.00× | — | 1 thread | — | — | — |
| **8K UHD** (`7680x4320`) | **`HostPageableEndToEnd`** | **36.82** | 37.03 | 0.67 (1.80%) | 36.18–38.01 | 39.36 | 38.97 | 53.83 / 84.04 | 4.887 | 1251.5 | 33,988,680 (2,435,408–89,196,680) | **10.20×** | on | 2 | 1953 | 54.4 | 60 |
| **8K UHD** (`7680x4320`) | `HostPinnedApi` | **45.74** | 45.44 | 0.76 (1.68%) | 43.92–45.94 | 29.95 | 27.77 | 45.39 / 72.32 | 6.070 | 1554.7 | 33,988,680 (2,435,408–89,196,680) | **12.67×** | on | 2 | 1976 | 56.9 | 60 |
| **8K UHD** (`7680x4320`) | `DeviceInput` *(GPU-resident)* | **93.22** | 93.80 | 0.92 (0.98%) | 93.01–95.43 | 17.53 | 17.05 | 33.60 / 54.78 | 12.372 | 3168.5 | 33,988,680 (2,435,408–89,196,680) | **25.82×** | on | 2 | 1813 | 76.6 | 62 |

#### Baseline Comparison Interpretation
On the tested RTX 4090 Laptop GPU system, HyperCine `HostPageableEndToEnd` (`ProductionEndToEnd_v2`) achieved **7.12×** (`450.65 FPS` vs `63.27 FPS` at 1080p), **12.71×** (`222.56 FPS` vs `17.52 FPS` at 4K UHD), and **10.20×** (`36.82 FPS` vs `3.61 FPS` at 8K UHD) the median throughput of the pristine single-threaded GoPro CineForm SDK encoder under the documented benchmark configuration. Caller-pinned host memory (`HostPinnedApi`) reached **8.71×** / **16.57×** / **12.67×** (`551.37` / `290.27` / `45.74` FPS), and GPU-resident `DeviceInput` (in-VRAM integration throughput, not file transcode) reached **12.53×** / **22.47×** / **25.82×** (`792.82` / `393.60` / `93.22` FPS).

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
| **1080p** (`400` frames) | 1 | no | 385.66 | 2.58 | 2.37 | 3.77 | 104.63 MB (99.78 MiB) | 73.40 MB (70.00 MiB) |
| **1080p** (`400` frames) | **2** | **yes (`DefaultAuto`)** | **464.59** | **3.39** | **3.21** | **4.53** | **150.82 MB (143.83 MiB)** | **144.70 MB (138.00 MiB)** |
| **1080p** (`400` frames) | 3 | no | 612.91 | 4.44 | 4.20 | 5.98 | 197.01 MB (187.88 MiB) | 216.01 MB (206.00 MiB) |
| **1080p** (`400` frames) | 4 | no | 651.31 | 5.53 | 5.49 | 7.79 | 243.20 MB (231.93 MiB) | 287.31 MB (274.00 MiB) |
| **4K UHD** (`120` frames) | 1 | no | 122.23 | 8.16 | 7.08 | 12.90 | 303.69 MB (289.63 MiB) | 268.44 MB (256.00 MiB) |
| **4K UHD** (`120` frames) | 2 | no | 155.71 | 9.71 | 8.78 | 13.67 | 474.30 MB (452.33 MiB) | 536.87 MB (512.00 MiB) |
| **4K UHD** (`120` frames) | **3** | **yes (`DefaultAuto`)** | **231.32** | **11.79** | **10.62** | **18.83** | **644.91 MB (615.03 MiB)** | **805.31 MB (768.00 MiB)** |
| **4K UHD** (`120` frames) | 4 | no | 234.80 | 16.61 | 14.95 | 28.46 | 815.51 MB (777.73 MiB) | 1,073.74 MB (1,024.00 MiB) |
| **8K UHD** (`48` frames) | 1 | no | 29.37 | 33.70 | 32.03 | 50.20 | 1,099.96 MB (1,049.00 MiB) | 1,050.67 MB (1,002.00 MiB) |
| **8K UHD** (`48` frames) | **2** | **yes (`DefaultAuto`)** | **37.25** | **39.38** | **37.69** | **56.25** | **1,768.23 MB (1,686.31 MiB)** | **2,101.35 MB (2,004.00 MiB)** |
| **8K UHD** (`48` frames) | 3 | no | 53.70 | 50.33 | 48.63 | 97.75 | 2,436.50 MB (2,323.63 MiB) | 3,152.02 MB (3,006.00 MiB) |
| **8K UHD** (`48` frames) | 4 | no | 60.85 | 62.94 | 61.03 | 113.59 | 3,104.77 MB (2,960.94 MiB) | 4,202.69 MB (4,008.00 MiB) |

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

### Option C: Experimental SDK-free Apple Silicon encoder

The separate fixed 128x128 FS3-Q2 CPU encoder builds natively on arm64 without
compiling or linking the SDK or CUDA. It emits real CFHD samples validated with
an independent FFmpeg decoder. See [the standalone report](docs/MACOS_STANDALONE_ENCODER.md)
for profile limits, RGB quality results and pending Windows/Adobe/Resolve gates.

```sh
cmake -S . -B build-standalone -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_BENCHMARKS=OFF
cmake --build build-standalone --config Release --parallel
ctest --test-dir build-standalone -C Release --output-on-failure
./build-standalone/hypercine_encode_smoke gradient gradient.cfhd
```

### Running CUDA Sanitizer Verification
```powershell
compute-sanitizer --tool memcheck .\build-release-sm89\tests\Release\test_entropy_capacity.exe
compute-sanitizer --tool memcheck .\build-release-sm89\tests\Release\test_phase3a.exe --quick-sanitize
```
*(Note: On Windows, `compute-sanitizer` uses a local `127.0.0.1` loopback TCP socket for IPC with the target binary, which may trigger a one-time Windows Firewall prompt; no external network access is used.)*

### Running the Canonical Benchmark Harness
`benchmark_phase3a1.exe` enforces strict working-tree cleanliness (`git status --porcelain` and `git status --porcelain upstream/` must be 100% empty) and requires canonical output to be written to a path outside the repository (e.g., `%TEMP%\phase3a2c_release_candidate.json`) before being copied in:

```powershell
# Canonical 5-trial benchmark suite (equivalence + headline + slots + graph) on a clean working tree:
.\build-release-sm89\benchmarks\Release\benchmark_phase3a1.exe --suite=all --trials=5 --out="$env:TEMP\phase3a2c_release_candidate.json"
Copy-Item "$env:TEMP\phase3a2c_release_candidate.json" "benchmarks\phase3a2c_release_candidate.json"

# Exploratory run on a modified working tree (marked "publication": "DIRTY"):
.\build-release-sm89\benchmarks\Release\benchmark_phase3a1.exe --allow-dirty --suite=headline --trials=1 --out="$env:TEMP\phase3a1_tmp.json"
```

---

## 5. Current Limitations

To ensure transparent technical evaluation, please note the following scope boundaries of the Phase 3A.2c release candidate:

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

Original CineForm SDK (`upstream/`): Copyright (c) 2017 GoPro, Inc. (dual-licensed under MIT or Apache-2.0; see [`upstream/LICENSE.txt`](upstream/LICENSE.txt) and [`upstream/LICENSE-APACHE.txt`](upstream/LICENSE-APACHE)).

HyperCine additions: Copyright (c) 2026 HyperCine Contributors.
