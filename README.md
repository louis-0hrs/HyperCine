# HyperCine

**HyperCine** is an independent, GPU-accelerated CineForm-compatible (`CFHD` / SMPTE VC-5) video encoder and research baseline derived from the open-source [GoPro CineForm SDK](https://github.com/gopro/cineform-sdk).

> **Project Status & Independence Notice:**  
> HyperCine is an **independent derivative engineering and research project**. It is **not** an official GoPro product and is not affiliated with or endorsed by GoPro, Inc. The repository preserves the upstream GoPro CineForm SDK (`upstream/`, commit `11574d0295771edccadd17af14af74a539f924c7`) completely unmodified as a frozen reference oracle and builds a modern C++17 / NVIDIA CUDA encoder pipeline alongside it.

---

## 1. Overview & Technical Scope

HyperCine accelerates the CineForm intra-frame wavelet encoding pipeline on NVIDIA CUDA GPUs while maintaining strict bitstream equivalence with the pristine upstream GoPro CineForm SDK reference encoder (`CFHD_EncodeSample`).

### Supported Scope (Phase 3A.2 Release Candidate)
* **Primary Accelerated Platform:** Windows 10/11 x64 with NVIDIA CUDA GPUs (verified on `sm_89` / GeForce RTX 4090 Laptop GPU; configurable via `CMAKE_CUDA_ARCHITECTURES`).
* **CPU-Only Reference Build:** Portable ISO C++17 build mode (`-DHYPERCINE_ENABLE_CUDA=OFF`) compiling the unmodified upstream SDK and CPU reference wrappers (verified in GitHub Actions CI).
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
   * **`DeviceInput` (GPU-Resident Integration Mode):** Accepts `BGRA8` frames already residing in GPU device memory (`cudaMalloc`), measuring GPU encode + compressed D2H + host sample assembly. *Note: `DeviceInput` represents in-VRAM pipeline integration throughput (e.g., after GPU color grading or compositing) and must not be cited as file-transcode throughput.*
3. **Split Frontend / Wavelet Multi-Slot Overlap & CUDA Graph Execution:**
   * **Frontend Stage:** Asynchronous H2D DMA and `BgraToYuv` (8-bit `BGRA` $\to$ 12-bit internal `YUV 4:2:2`) run on frame $k+1$ in an overlapping slot while frame $k$ completes wavelet + entropy.
   * **Wavelet + Dynamic Quantization Stage (`HyperCine_GpuSlotKickWaveletWithQuant`):** Computes the 3-level 2-6 biorthogonal wavelet pyramid and applies frame $k$'s 36-entry `quant[]` table directly inside the wavelet kernels. This launch executes **outside** the entropy CUDA Graph so per-frame `FSratelimiter` updates never trigger CUDA Graph recapture or node parameter patching.
   * **GPU Entropy Stage (`FusedScanOptimized` + `cudaGraphExec_t`):** Encodes all 27 highpass subbands across 4 concurrent CUDA streams (Longest Processing Time schedule) using a 9-kernel-per-band fused run-length + VLC + radix-1024 prefix-scan pipeline plus 2 frame-payload consolidation kernels (`245` kernel nodes total), captured once per slot into a cached `cudaGraphExec_t` and replayed via a single `cudaGraphLaunch` per frame.
   * **Proven Entropy Word Capacity & Sticky Error Propagation:** Each highpass subband of $N = \text{height} \times \text{pitch\_px}$ coefficients is analytically bounded by $\lceil 31(N+1)/32 \rceil \le N+1$ 32-bit VLC words and allocated with $N + 4096$ words (see [`docs/ENTROPY.md`](docs/ENTROPY.md)). Any kernel capacity violation or asynchronous CUDA error transitions `GpuStageStatus` (`Pending` / `Complete` / `Failed`) to a sticky `Failed` state rather than spinning indefinitely.

---

## 3. Canonical Benchmark Summary (`ProductionEndToEnd`)

### Methodology (`ProductionEndToEnd`)
All headline numbers are generated from a clean git working tree by [`benchmarks/benchmark_phase3a1.cpp`](benchmarks/benchmark_phase3a1.cpp) and stored in [`benchmarks/phase3a2_release_candidate.json`](benchmarks/phase3a2_release_candidate.json) (commit `4e8037841dcba3c31a6f8d31371869e26f6fbd1b`, `"publication": "CLEAN"`, `"git_status_clean": true`).

* **Timed Boundary:** Starts when the first measured frame is submitted after warmup (`warmup_frames >= slot_count`, so one-time CUDA Graph capture is excluded and reported separately). Ends only after the final measured frame's `CFHD` bitstream has been assembled in host memory **and** `cudaDeviceSynchronize()` has returned.
* **Sequence Lengths & Multi-Trial Policy:** `1000` measured frames at 1080p (`96`-frame distinct complexity cycle), `300` measured frames at 4K UHD (`36`-frame distinct cycle), and `120` measured frames at 8K UHD (`12`-frame distinct cycle). Each configuration runs **5 trials** with per-trial input-mode permutation; the **5-trial median** is the canonical headline figure.
* **CPU Baseline Definition:** Baseline 1 (`UpstreamCpuEncodeSample`) is the pristine upstream `CFHD_EncodeSample` compiled from unmodified `upstream/` with its default `_THREADED_ENCODER=0` (single-threaded encoder) and MSVC Release `/O2` optimization. As documented in [`docs/UPSTREAM.md`](docs/UPSTREAM.md), upstream's optional `CFHDCodecThreaded` (`_THREADED_ENCODER=1`) target is marked `EXCLUDE_FROM_ALL` upstream and does not compile due to missing legacy symbols. Neither upstream nor HyperCine forces `/arch:AVX2` in CMake.

### Canonical 5-Trial Results (`benchmarks/phase3a2_release_candidate.json`)

**Test System:** AMD Ryzen 9 7940HS (16 logical processors, 33.5 GB RAM), NVIDIA GeForce RTX 4090 Laptop GPU (`sm_89`, 16,375 MiB VRAM, NVIDIA Driver `610.62`, CUDA `12.8.61`), Windows 10.0 Build 26200 x64, MSVC `19.44.35228.0` (`Release /O2`), AC power online.

| Resolution | Input Mode | Median FPS | Mean FPS | Stdev (FPS / %) | Min–Max FPS | Mean Latency (ms) | Median Latency (ms) | P95 / P99 Latency (ms) | Raw Input (GB/s) | Output (MB/s) | Speedup vs Upstream CPU | Graph | Slots | Mean GPU Clock (MHz) | Mean Power (W) | Max Temp (°C) |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: | :---: | ---: | ---: | ---: |
| **1080p** (`1920x1080`) | `UpstreamCpuEncodeSample` | **76.98** | 77.03 | 0.47 (0.61%) | 76.40–77.59 | 12.98 | 9.26 | 28.17 / 29.43 | 0.639 | 135.3 | 1.00× | — | 1 thread | — | — | — |
| **1080p** (`1920x1080`) | **`HostPageableEndToEnd`** | **457.53** | 443.35 | 30.03 (6.77%) | 391.99–470.47 | 3.42 | 3.20 | 4.16 / 8.14 | 3.795 | 804.0 | **5.94×** | on | 2 | 1840 | 45.2 | 62 |
| **1080p** (`1920x1080`) | `HostPinnedApi` | **561.31** | 546.87 | 31.02 (5.67%) | 485.01–564.97 | 2.54 | 2.39 | 3.32 / 5.32 | 4.656 | 986.3 | **7.29×** | on | 2 | 1903 | 48.3 | 62 |
| **1080p** (`1920x1080`) | `DeviceInput` *(GPU-resident)* | **863.82** | 808.72 | 83.56 (10.33%) | 694.40–883.58 | 2.10 | 1.90 | 3.27 / 6.05 | 7.165 | 1517.9 | **11.22×** | on | 2 | 1732 | 47.9 | 62 |
| **4K UHD** (`3840x2160`) | `UpstreamCpuEncodeSample` | **19.65** | 19.64 | 0.08 (0.42%) | 19.49–19.73 | 50.91 | 34.48 | 113.76 / 117.50 | 0.652 | 130.5 | 1.00× | — | 1 thread | — | — | — |
| **4K UHD** (`3840x2160`) | **`HostPageableEndToEnd`** | **227.07** | 223.29 | 5.73 (2.57%) | 214.41–228.85 | 12.30 | 11.09 | 17.90 / 29.11 | 7.534 | 1525.3 | **11.56×** | on | 3 | 1778 | 56.4 | 64 |
| **4K UHD** (`3840x2160`) | `HostPinnedApi` | **294.09** | 288.10 | 14.74 (5.12%) | 258.82–298.28 | 9.21 | 8.04 | 13.70 / 27.28 | 9.757 | 1975.5 | **14.97×** | on | 3 | 1855 | 58.1 | 63 |
| **4K UHD** (`3840x2160`) | `DeviceInput` *(GPU-resident)* | **385.06** | 374.84 | 34.53 (9.21%) | 315.57–409.91 | 7.01 | 5.54 | 12.39 / 24.57 | 12.775 | 2586.6 | **19.60×** | on | 3 | 1672 | 56.4 | 64 |
| **8K UHD** (`7680x4320`) | `UpstreamCpuEncodeSample` | **4.36** | 4.36 | 0.01 (0.29%) | 4.35–4.38 | 229.24 | 173.25 | 451.61 / 463.74 | 0.579 | 148.3 | 1.00× | — | 1 thread | — | — | — |
| **8K UHD** (`7680x4320`) | **`HostPageableEndToEnd`** | **38.46** | 38.80 | 0.60 (1.55%) | 38.06–39.60 | 37.20 | 35.20 | 50.67 / 71.82 | 5.105 | 1307.5 | **8.81×** | on | 2 | 1950 | 57.7 | 62 |
| **8K UHD** (`7680x4320`) | `HostPinnedApi` | **45.86** | 45.73 | 0.51 (1.11%) | 45.01–46.33 | 29.32 | 28.17 | 44.62 / 64.05 | 6.086 | 1558.9 | **10.51×** | on | 2 | 1960 | 58.6 | 63 |
| **8K UHD** (`7680x4320`) | `DeviceInput` *(GPU-resident)* | **92.46** | 92.68 | 3.19 (3.45%) | 88.24–96.92 | 17.38 | 16.34 | 33.53 / 51.18 | 12.270 | 3142.9 | **21.19×** | on | 2 | 1772 | 80.7 | 64 |

#### Baseline Comparison Interpretation
On the tested RTX 4090 Laptop GPU system, HyperCine `HostPageableEndToEnd` (`ProductionEndToEnd`) achieved **5.94×** (`457.53 FPS` vs `76.98 FPS` at 1080p), **11.56×** (`227.07 FPS` vs `19.65 FPS` at 4K UHD), and **8.81×** (`38.46 FPS` vs `4.36 FPS` at 8K UHD) the median throughput of the pristine single-threaded GoPro CineForm SDK encoder (`CFHD_EncodeSample` with `_THREADED_ENCODER=0`). Caller-pinned host memory (`HostPinnedApi`) reached **7.29×** / **14.97×** / **10.51×**, and GPU-resident `DeviceInput` reached **11.22×** / **19.60×** / **21.19×** (`863.82` / `385.06` / `92.46` FPS). Full per-stage timing, CUDA Graph off-vs-on deltas (`+95.4%` at 1080p, `+38.3%` at 4K, `+3.2%` at 8K at matched 1995 MHz clocks), and slot-scaling analyses are documented in [`docs/PHASE3A1_BENCHMARK_HARDENING.md`](docs/PHASE3A1_BENCHMARK_HARDENING.md).

---

## 4. Build, Test & Benchmark Instructions

### Prerequisites
* **OS:** Windows 10/11 x64 (Linux/macOS supported for CPU-only reference build)
* **Build System:** CMake 3.25+
* **C++ Compiler:** Visual Studio 2022 (MSVC 19.30+) with C++17 support
* **CUDA (for GPU acceleration):** NVIDIA CUDA Toolkit 12.x+ and a compatible NVIDIA GPU driver

### Option A: CPU-Only Reference Build (`HYPERCINE_ENABLE_CUDA=OFF`)
Builds the unmodified upstream GoPro CineForm SDK (`CFHDCodecStatic`) and HyperCine CPU reference wrappers without requiring the CUDA Toolkit:

```powershell
cmake -S . -B build_cpu -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=ON
cmake --build build_cpu --config Release
ctest --test-dir build_cpu -C Release --output-on-failure
```
*Expected result: 4 / 4 CTest suites pass (`test_transform_wavelet`, `test_codec_roundtrip`, `test_yuy2_stability`, `test_avi_sample`).*

### Option B: Full NVIDIA CUDA Release Build (`HYPERCINE_ENABLE_CUDA=ON`)
Builds all CPU and CUDA libraries, all 14 verification suites, and all benchmark harnesses (adjust `-DCMAKE_CUDA_ARCHITECTURES` to match your GPU, e.g., `89` for Ada Lovelace, `86` for Ampere):

```powershell
cmake -S . -B build -DHYPERCINE_ENABLE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89 -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```
*Expected result: 14 / 14 CTest suites pass (`100%`), including `test_entropy_capacity` and `test_phase3a`.*

### Running CUDA Sanitizer Verification
```powershell
compute-sanitizer --tool memcheck .\build\tests\Release\test_entropy_capacity.exe
compute-sanitizer --tool memcheck .\build\tests\Release\test_phase3a.exe --quick-sanitize
```
*(Note: On Windows, `compute-sanitizer` uses a local `127.0.0.1` loopback TCP socket for IPC with the target binary, which may trigger a one-time Windows Firewall prompt; no external network access is used.)*

### Running the Canonical Benchmark Harness
`benchmark_phase3a1.exe` refuses to emit a canonical benchmark file if `git status --porcelain` is non-empty unless `--allow-dirty` is explicitly passed:

```powershell
# Canonical 5-trial headline benchmark on a clean working tree:
.\build\benchmarks\Release\benchmark_phase3a1.exe --suite=headline --trials=5 --out=benchmarks/phase3a2_release_candidate.json

# Exploratory run on a modified working tree (marked "publication": "DIRTY"):
.\build\benchmarks\Release\benchmark_phase3a1.exe --allow-dirty --suite=headline --trials=1 --out=benchmarks/phase3a1_tmp.json
```

---

## 5. Current Limitations

To ensure transparent technical evaluation, please note the following scope boundaries of the Phase 3A.2 release candidate:

1. **Platform & Vendor Scope:** GPU acceleration currently targets **Windows x64 + NVIDIA CUDA** only. Vulkan (`src/vulkan/`) and Apple Metal (`src/metal/`) backends are planned for future phases and are not implemented in this release.
2. **Pixel Format & Compression Profile Scope:** The full GPU pipeline (`Phase3AEncoderContext`) currently accelerates 8-bit `BGRA` (`CFHD_PIXEL_FORMAT_BGRA`) $\to$ 12-bit internal `YUV 4:2:2` (`CFHD_ENCODED_FORMAT_YUV_422`) at `FilmScan 3` (`CFHD_ENCODING_QUALITY_FILMSCAN3`), progressive, `GOP = 1`. Other CineForm pixel formats (`v210`, `RG48`, `b64a`, `BYR4` Bayer RAW, `YUV 4:4:4`, alpha channels) fall back to the upstream CPU SDK.
3. **Encoder-Only GPU Acceleration:** GPU acceleration is implemented for **encoding**. Decoding in verification tests uses the upstream GoPro CineForm CPU decoder (`CFHD_DecodeSample` with `TAG_CPU_MAX = 1` to avoid upstream's multi-threaded decoder race conditions documented in [`docs/UPSTREAM.md`](docs/UPSTREAM.md)).
4. **Research/Engineering API Packaging:** The multi-frame GPU encoder is exposed via internal C++ headers (`include/hypercine/phase3a.h`, `RunPhase3A`) rather than a drop-in DirectShow/QuickTime/VFW system installer or commercial NLE plugin bundle.
5. **External NLE & FFmpeg Interoperability:** Bitstream compliance is verified by byte-for-byte masked equivalence (`masked_diff_bytes == 0`) against pristine upstream `CFHD_EncodeSample` and round-trip decoding through `CFHD_DecodeSample`. Automated CI testing inside Adobe Premiere Pro, DaVinci Resolve, or FFmpeg (`cfhd`) is not part of the repository test runner.
6. **Volatile Clip Metadata in Cross-Handle Comparisons:** In production mode (`deterministic_test_metadata = false`, the default), every encode writes live `TAG_CLIP_GUID`, `TAG_ENCODE_DATE`, `TAG_ENCODE_TIME`, and `TAG_TIMECODE` tags into the `CFHD` header. Cross-handle byte-identity tests mask only those 4 volatile metadata fields (`MaskCfhdVolatileMetadata`).
7. **Laptop GPU Clock & Thermal Variance:** On mobile GPUs (such as the tested RTX 4090 Laptop GPU), dynamic boost clocks and power/thermal caps (`SwPowerCap`, `SwThermalSlowdown`, or `GpuIdle` clock transitions between trials) introduce run-to-run variance (`~5–10%` stdev at 1080p/4K). All benchmark outputs therefore record 5-trial medians alongside full NVML clock, power, temperature, and throttle telemetry.

---

## 6. Documentation Map

* [`docs/PHASE3A1_BENCHMARK_HARDENING.md`](docs/PHASE3A1_BENCHMARK_HARDENING.md): Authoritative Phase 3A.1 / 3A.2 `ProductionEndToEnd` benchmark methodology, clean-tree headline tables, stage breakdowns, CUDA Graph off/on study, and slot scaling.
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
