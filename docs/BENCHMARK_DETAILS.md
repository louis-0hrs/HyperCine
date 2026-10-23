# HyperCine — Detailed benchmark results

[← README overview](../README.md#performance)

This document preserves the Phase 3A.2c `ProductionEndToEnd_v2` benchmark record, including its provenance, equivalence checks and non-headline slot-scaling study. The original exceptionally wide tables are separated into readable views **without changing any recorded values**. These results describe the benchmarked release candidate, not a new test run.

## Methodology (`ProductionEndToEnd_v2`)
All headline numbers are generated from a clean git working tree by [`benchmarks/benchmark_phase3a1.cpp`](../benchmarks/benchmark_phase3a1.cpp) and stored in [`benchmarks/phase3a2c_release_candidate.json`](../benchmarks/phase3a2c_release_candidate.json) (benchmarked source commit `57ca3f37be4f017af8a72592e042b2fe285416a3`, `"publication": "CLEAN"`, `"canonical": true`, `"benchmark_tree_clean": true`, `"upstream_tree_clean": true`, `"benchmark_equivalence_verified": true`, `"native_architecture_match": true`).

> **Provenance Note (Source vs. Release Revision):** The benchmark executable was built natively for sm_89 and executed from source commit `57ca3f37be4f017af8a72592e042b2fe285416a3` on a 100% clean working tree with benchmark output written outside the repository (`%TEMP%\phase3a2c_release_candidate.json`). The generated benchmark artifact and updated documentation were then committed without changing production source, CUDA kernels, tests, benchmark source, or build logic. The annotated release-candidate tag `phase3a2c-release-candidate` points to the resulting documentation/artifact commit.

* **Timed Boundary (`ProductionEndToEnd`):** Starts when the first measured frame (`logical_frame_index = 4`) is submitted after the common warmup prefix (`warmup_frames = 4 >= slot_count`, so one-time CUDA Graph capture is excluded and reported separately). For `HostPageableEndToEnd`, the timed window includes ordinary pageable source $\to$ pageable-to-pinned staging $\to$ H2D DMA $\to$ GPU frontend $\to$ wavelet + dynamic quantization $\to$ GPU entropy $\to$ compressed D2H DMA $\to$ host `CFHD` sample assembly $\to$ final worker drain and `cudaDeviceSynchronize()`. Encoder creation, CUDA context creation, input-frame generation, one-time CUDA Graph capture, and filesystem output are excluded.
* **Aligned Warmup & Sequence Definition (`ProductionEndToEnd_v2`):** Both `UpstreamCpuEncodeSample` and all three HyperCine GPU modes execute the exact same `W = 4` warmup frames (`pool[0..3]`) followed without reset or restart by `N` timed frames starting at logical sequence index `4` (`pool[(4 + i) % unique_cycle]`: `N = 1000` over a `96`-frame cycle at 1080p, `N = 300` over a `36`-frame cycle at 4K UHD, and `N = 120` over a `12`-frame cycle at 8K UHD). Each configuration runs **5 trials** with per-trial GPU input-mode permutation; the **5-trial median** is the canonical headline figure.
* **Apples-to-Apples Stateful Equivalence & Output-Size Fairness Gate:** Before headline trials are marked canonical, `benchmark_phase3a1` verifies on the shared sequence (`W = 4`, first measured logical frame `4`) that `UpstreamCpuEncodeSample`, `HostPageableEndToEnd`, `HostPinnedApi`, and `DeviceInput` produce identical per-frame logical indices, patterns, `prev_encoded_bytes`, `lastgopbitcount`, `FSratelimiter` trajectories, 36-entry `quant[]` / `scale[]` tables, per-frame sample byte counts, and masked `CFHD` sample SHA-256 digests (`24/24` at 1080p, `24/24` at 4K UHD, `12/12` at 8K UHD), and that full-sequence headline `mean_sample_bytes`, `min_sample_bytes`, `max_sample_bytes`, and `distinct_fs_rate_limiters` match 100% across all four modes.
* **CPU Baseline Definition:** Baseline 1 (`UpstreamCpuEncodeSample`) is the pristine upstream `CFHD_EncodeSample` compiled from unmodified `upstream/` with its default `_THREADED_ENCODER=0` (single-threaded encoder) and MSVC Release `/O2` optimization. As documented in [`docs/UPSTREAM.md`](UPSTREAM.md), upstream's optional `CFHDCodecThreaded` (`_THREADED_ENCODER=1`) target is marked `EXCLUDE_FROM_ALL` upstream and does not compile due to missing legacy symbols. Neither upstream nor HyperCine forces `/arch:AVX2` in CMake.

## Canonical five-trial results (`benchmarks/phase3a2c_release_candidate.json`)

**Test System:** AMD Ryzen 9 7940HS (16 logical processors, 33.5 GB RAM), NVIDIA GeForce RTX 4090 Laptop GPU (runtime `sm_89`, built natively with `-DCMAKE_CUDA_ARCHITECTURES=89` $\to$ cubin `sm_89` + PTX `compute_89`, 16,375 MiB VRAM, NVIDIA Driver `610.62`, CUDA `12.8.61`), Windows 10.0 Build 26200 x64, MSVC `19.44.35228.0` (`Release /O2`), AC power online.

#### Throughput

| Resolution | Input mode | Median FPS | Mean FPS | FPS stdev (%) | FPS range | Speedup vs CPU |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: |
| **1080p** (`1920x1080`) | `UpstreamCpuEncodeSample` | **63.27** | 65.53 | 4.93 (7.52%) | 60.26–74.39 | 1.00× |
| **1080p** (`1920x1080`) | **`HostPageableEndToEnd`** | **450.65** | 442.29 | 17.00 (3.84%) | 414.08–461.93 | **7.12×** |
| **1080p** (`1920x1080`) | `HostPinnedApi` | **551.37** | 540.28 | 24.70 (4.57%) | 491.31–556.73 | **8.71×** |
| **1080p** (`1920x1080`) | `DeviceInput` *(GPU-resident)* | **792.82** | 749.39 | 66.80 (8.91%) | 649.74–819.99 | **12.53×** |
| **4K UHD** (`3840x2160`) | `UpstreamCpuEncodeSample` | **17.52** | 17.91 | 1.28 (7.17%) | 16.46–19.94 | 1.00× |
| **4K UHD** (`3840x2160`) | **`HostPageableEndToEnd`** | **222.56** | 219.14 | 14.61 (6.67%) | 200.89–234.74 | **12.71×** |
| **4K UHD** (`3840x2160`) | `HostPinnedApi` | **290.27** | 269.37 | 42.40 (15.74%) | 184.75–293.96 | **16.57×** |
| **4K UHD** (`3840x2160`) | `DeviceInput` *(GPU-resident)* | **393.60** | 380.38 | 23.71 (6.23%) | 341.95–405.29 | **22.47×** |
| **8K UHD** (`7680x4320`) | `UpstreamCpuEncodeSample` | **3.61** | 3.60 | 0.04 (1.17%) | 3.55–3.66 | 1.00× |
| **8K UHD** (`7680x4320`) | **`HostPageableEndToEnd`** | **36.82** | 37.03 | 0.67 (1.80%) | 36.18–38.01 | **10.20×** |
| **8K UHD** (`7680x4320`) | `HostPinnedApi` | **45.74** | 45.44 | 0.76 (1.68%) | 43.92–45.94 | **12.67×** |
| **8K UHD** (`7680x4320`) | `DeviceInput` *(GPU-resident)* | **93.22** | 93.80 | 0.92 (0.98%) | 93.01–95.43 | **25.82×** |

#### Latency

| Resolution | Input mode | Mean (ms) | Median (ms) | P95 / P99 (ms) |
| :--- | :--- | ---: | ---: | ---: |
| **1080p** (`1920x1080`) | `UpstreamCpuEncodeSample` | 15.34 | 11.58 | 33.53 / 36.55 |
| **1080p** (`1920x1080`) | **`HostPageableEndToEnd`** | 3.55 | 3.34 | 4.75 / 7.70 |
| **1080p** (`1920x1080`) | `HostPinnedApi` | 2.66 | 2.48 | 3.75 / 6.22 |
| **1080p** (`1920x1080`) | `DeviceInput` *(GPU-resident)* | 2.31 | 2.07 | 3.81 / 6.82 |
| **4K UHD** (`3840x2160`) | `UpstreamCpuEncodeSample` | 56.13 | 40.81 | 124.92 / 137.64 |
| **4K UHD** (`3840x2160`) | **`HostPageableEndToEnd`** | 12.43 | 10.91 | 17.70 / 50.95 |
| **4K UHD** (`3840x2160`) | `HostPinnedApi` | 10.16 | 9.21 | 15.60 / 30.22 |
| **4K UHD** (`3840x2160`) | `DeviceInput` *(GPU-resident)* | 6.86 | 5.49 | 12.32 / 24.29 |
| **8K UHD** (`7680x4320`) | `UpstreamCpuEncodeSample` | 277.89 | 207.05 | 567.99 / 592.38 |
| **8K UHD** (`7680x4320`) | **`HostPageableEndToEnd`** | 39.36 | 38.97 | 53.83 / 84.04 |
| **8K UHD** (`7680x4320`) | `HostPinnedApi` | 29.95 | 27.77 | 45.39 / 72.32 |
| **8K UHD** (`7680x4320`) | `DeviceInput` *(GPU-resident)* | 17.53 | 17.05 | 33.60 / 54.78 |

#### Transfer rate and sample size

| Resolution | Input mode | Raw input (GB/s) | Output (MB/s) | Mean sample bytes (range) |
| :--- | :--- | ---: | ---: | ---: |
| **1080p** (`1920x1080`) | `UpstreamCpuEncodeSample` | 0.525 | 111.2 | 1,757,179 (180,440–5,494,332) |
| **1080p** (`1920x1080`) | **`HostPageableEndToEnd`** | 3.738 | 791.9 | 1,757,179 (180,440–5,494,332) |
| **1080p** (`1920x1080`) | `HostPinnedApi` | 4.573 | 968.9 | 1,757,179 (180,440–5,494,332) |
| **1080p** (`1920x1080`) | `DeviceInput` *(GPU-resident)* | 6.576 | 1393.1 | 1,757,179 (180,440–5,494,332) |
| **4K UHD** (`3840x2160`) | `UpstreamCpuEncodeSample` | 0.581 | 118.4 | 6,761,383 (631,824–22,236,848) |
| **4K UHD** (`3840x2160`) | **`HostPageableEndToEnd`** | 7.384 | 1504.8 | 6,761,383 (631,824–22,236,848) |
| **4K UHD** (`3840x2160`) | `HostPinnedApi` | 9.631 | 1962.6 | 6,761,383 (631,824–22,236,848) |
| **4K UHD** (`3840x2160`) | `DeviceInput` *(GPU-resident)* | 13.059 | 2661.3 | 6,761,383 (631,824–22,236,848) |
| **8K UHD** (`7680x4320`) | `UpstreamCpuEncodeSample` | 0.479 | 122.7 | 33,988,680 (2,435,408–89,196,680) |
| **8K UHD** (`7680x4320`) | **`HostPageableEndToEnd`** | 4.887 | 1251.5 | 33,988,680 (2,435,408–89,196,680) |
| **8K UHD** (`7680x4320`) | `HostPinnedApi` | 6.070 | 1554.7 | 33,988,680 (2,435,408–89,196,680) |
| **8K UHD** (`7680x4320`) | `DeviceInput` *(GPU-resident)* | 12.372 | 3168.5 | 33,988,680 (2,435,408–89,196,680) |

#### Graph, slots and GPU telemetry

| Resolution | Input mode | CUDA Graph | Slots | Clock (MHz) | Power (W) | Max temp (°C) |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: |
| **1080p** (`1920x1080`) | `UpstreamCpuEncodeSample` | — | 1 thread | — | — | — |
| **1080p** (`1920x1080`) | **`HostPageableEndToEnd`** | on | 2 | 1822 | 44.6 | 61 |
| **1080p** (`1920x1080`) | `HostPinnedApi` | on | 2 | 1899 | 48.7 | 61 |
| **1080p** (`1920x1080`) | `DeviceInput` *(GPU-resident)* | on | 2 | 1702 | 47.7 | 62 |
| **4K UHD** (`3840x2160`) | `UpstreamCpuEncodeSample` | — | 1 thread | — | — | — |
| **4K UHD** (`3840x2160`) | **`HostPageableEndToEnd`** | on | 3 | 1786 | 50.3 | 63 |
| **4K UHD** (`3840x2160`) | `HostPinnedApi` | on | 3 | 1901 | 56.0 | 63 |
| **4K UHD** (`3840x2160`) | `DeviceInput` *(GPU-resident)* | on | 3 | 1642 | 54.6 | 64 |
| **8K UHD** (`7680x4320`) | `UpstreamCpuEncodeSample` | — | 1 thread | — | — | — |
| **8K UHD** (`7680x4320`) | **`HostPageableEndToEnd`** | on | 2 | 1953 | 54.4 | 60 |
| **8K UHD** (`7680x4320`) | `HostPinnedApi` | on | 2 | 1976 | 56.9 | 60 |
| **8K UHD** (`7680x4320`) | `DeviceInput` *(GPU-resident)* | on | 2 | 1813 | 76.6 | 62 |

## Baseline interpretation
On the tested RTX 4090 Laptop GPU system, HyperCine `HostPageableEndToEnd` (`ProductionEndToEnd_v2`) achieved **7.12×** (`450.65 FPS` vs `63.27 FPS` at 1080p), **12.71×** (`222.56 FPS` vs `17.52 FPS` at 4K UHD), and **10.20×** (`36.82 FPS` vs `3.61 FPS` at 8K UHD) the median throughput of the pristine single-threaded GoPro CineForm SDK encoder under the documented benchmark configuration. Caller-pinned host memory (`HostPinnedApi`) reached **8.71×** / **16.57×** / **12.67×** (`551.37` / `290.27` / `45.74` FPS), and GPU-resident `DeviceInput` (in-VRAM integration throughput, not file transcode) reached **12.53×** / **22.47×** / **25.82×** (`792.82` / `393.60` / `93.22` FPS).

## Runtime slot policy versus manual slot scaling

HyperCine distinguishes between two slot-selection policies in `SelectPhase2DFrameSlots` ([`src/core/phase2d_encoder.cpp`](../src/core/phase2d_encoder.cpp)):
* **`DefaultAuto` (Canonical Balanced/Default Runtime Policy):** Used by default in `RunPhase3A` and for all canonical headline results above. It computes `per_slot_vram_bytes = 20 * px + 4,718,592` and `session_workspace_vram_bytes = 12 * px + 33,554,432`, caps `vram_max_slots` to 75% of available free VRAM after reserving workspace + 256 MiB, and selects a hardcoded resolution-bucket target (`2` slots for $\le 1080\text{p}$, `3` slots for $\le 4\text{K UHD}$, `2` slots for $> 4\text{K UHD}$ / $8\text{K UHD}$) with `FrameConsolidated` D2H under `Phase2DD2HPolicy::Throughput`. It does not dynamically tune slot count per GPU model at runtime:
  * **1080p (`DefaultAuto`):** `2` slots (`46,190,592` B [44.05 MiB] estimated per slot, `58,437,632` B [55.73 MiB] workspace, `150,818,816` B [143.83 MiB] total estimated VRAM, `144,703,488` B [138.00 MiB] measured VRAM used, `FrameConsolidated` D2H).
  * **4K UHD (`DefaultAuto`):** `3` slots (`170,606,592` B [162.70 MiB] estimated per slot, `133,087,232` B [126.92 MiB] workspace, `644,907,008` B [615.03 MiB] total estimated VRAM, `805,306,368` B [768.00 MiB] measured VRAM used, `FrameConsolidated` D2H).
  * **8K UHD (`DefaultAuto`):** `2` slots (`668,270,592` B [637.31 MiB] estimated per slot, `431,685,632` B [411.69 MiB] workspace, `1,768,226,816` B [1,686.31 MiB] total estimated VRAM, `2,101,346,304` B [2,004.00 MiB] measured VRAM used, `FrameConsolidated` D2H).
* **`UserOverride` (Explicit Tuning / Experimentation Policy):** Allows callers to specify an explicit slot count (`1..4`).

### Manual Slot Scaling / Throughput Tuning Study (`UserOverride` — Non-Headline)
> *The canonical benchmark uses the encoder's `DefaultAuto` slot policy. The slot-scaling study intentionally overrides that policy (`Phase2DSlotPolicy::UserOverride`) on `HostPageableEndToEnd` to measure throughput/latency/VRAM trade-offs and is not the public headline configuration.*

#### Manual slot count: throughput and tail latency

| Resolution | Slots | Is DefaultAuto? | Median FPS | Mean latency (ms) | P95 latency (ms) |
| :--- | :--- | ---: | ---: | ---: | ---: |
| **1080p** (`400` frames) | 1 | no | 385.66 | 2.58 | 3.77 |
| **1080p** (`400` frames) | **2** | **yes (`DefaultAuto`)** | **464.59** | **3.39** | **4.53** |
| **1080p** (`400` frames) | 3 | no | 612.91 | 4.44 | 5.98 |
| **1080p** (`400` frames) | 4 | no | 651.31 | 5.53 | 7.79 |
| **4K UHD** (`120` frames) | 1 | no | 122.23 | 8.16 | 12.90 |
| **4K UHD** (`120` frames) | 2 | no | 155.71 | 9.71 | 13.67 |
| **4K UHD** (`120` frames) | **3** | **yes (`DefaultAuto`)** | **231.32** | **11.79** | **18.83** |
| **4K UHD** (`120` frames) | 4 | no | 234.80 | 16.61 | 28.46 |
| **8K UHD** (`48` frames) | 1 | no | 29.37 | 33.70 | 50.20 |
| **8K UHD** (`48` frames) | **2** | **yes (`DefaultAuto`)** | **37.25** | **39.38** | **56.25** |
| **8K UHD** (`48` frames) | 3 | no | 53.70 | 50.33 | 97.75 |
| **8K UHD** (`48` frames) | 4 | no | 60.85 | 62.94 | 113.59 |

#### Manual slot count: median latency and VRAM

| Resolution | Slots | Median latency (ms) | Estimated total VRAM | Measured VRAM used (median) |
| :--- | :--- | ---: | ---: | ---: |
| **1080p** (`400` frames) | 1 | 2.37 | 104.63 MB (99.78 MiB) | 73.40 MB (70.00 MiB) |
| **1080p** (`400` frames) | **2** | **3.21** | **150.82 MB (143.83 MiB)** | **144.70 MB (138.00 MiB)** |
| **1080p** (`400` frames) | 3 | 4.20 | 197.01 MB (187.88 MiB) | 216.01 MB (206.00 MiB) |
| **1080p** (`400` frames) | 4 | 5.49 | 243.20 MB (231.93 MiB) | 287.31 MB (274.00 MiB) |
| **4K UHD** (`120` frames) | 1 | 7.08 | 303.69 MB (289.63 MiB) | 268.44 MB (256.00 MiB) |
| **4K UHD** (`120` frames) | 2 | 8.78 | 474.30 MB (452.33 MiB) | 536.87 MB (512.00 MiB) |
| **4K UHD** (`120` frames) | **3** | **10.62** | **644.91 MB (615.03 MiB)** | **805.31 MB (768.00 MiB)** |
| **4K UHD** (`120` frames) | 4 | 14.95 | 815.51 MB (777.73 MiB) | 1,073.74 MB (1,024.00 MiB) |
| **8K UHD** (`48` frames) | 1 | 32.03 | 1,099.96 MB (1,049.00 MiB) | 1,050.67 MB (1,002.00 MiB) |
| **8K UHD** (`48` frames) | **2** | **37.69** | **1,768.23 MB (1,686.31 MiB)** | **2,101.35 MB (2,004.00 MiB)** |
| **8K UHD** (`48` frames) | 3 | 48.63 | 2,436.50 MB (2,323.63 MiB) | 3,152.02 MB (3,006.00 MiB) |
| **8K UHD** (`48` frames) | 4 | 61.03 | 3,104.77 MB (2,960.94 MiB) | 4,202.69 MB (4,008.00 MiB) |

Full per-stage timing, CUDA Graph off-vs-on deltas, and historical Phase 3A.2 vs. Phase 3A.2a comparisons are documented in [`docs/PHASE3A1_BENCHMARK_HARDENING.md`](PHASE3A1_BENCHMARK_HARDENING.md).

[← README overview](../README.md#performance)
