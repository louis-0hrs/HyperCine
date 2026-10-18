# HyperCine

**GPU-accelerated CineForm encoding with upstream-compatible CFHD output.**

HyperCine is an independent C++17 / NVIDIA CUDA engineering and research project built alongside the open-source [GoPro CineForm SDK](https://github.com/gopro/cineform-sdk). It accelerates the CineForm (`CFHD` / SMPTE VC-5) intra-frame wavelet encoder and validates its bitstreams against the upstream reference.

**Stage:** `v0.1.0-alpha` · **CUDA target:** Windows x64 / NVIDIA · **Primary accelerated format:** BGRA8 → CFHD YUV 4:2:2 (10-bit codec precision), FilmScan 3

> [!IMPORTANT]
> HyperCine is **not an official GoPro product** and is not affiliated with or endorsed by GoPro, Inc. The reference SDK in [`upstream/`](upstream/) is frozen at commit `11574d0295771edccadd17af14af74a539f924c7` and remains unmodified. HyperCine's compiler integration, wrappers, and implementation live outside that directory.

[Performance](#performance) · [Technical scope](#technical-scope) · [Architecture](#architecture) · [Build and test](#build-and-test) · [Limitations](#limitations) · [Documentation](#documentation) · [License](#license-and-attribution)

## Performance

### Phase 3A.2c canonical results

**Median frames per second**, across five `ProductionEndToEnd_v2` trials. Parentheses show speedup relative to the original **single-threaded** GoPro SDK encoder.

| Resolution | Upstream CPU | Host pageable (GPU) | Host pinned (GPU) | Device input (GPU) |
| :--- | ---: | ---: | ---: | ---: |
| **1080p** (`1920x1080`) | **63.27** | **450.65** (**7.12×**) | **551.37** (**8.71×**) | **792.82** (**12.53×**) |
| **4K UHD** (`3840x2160`) | **17.52** | **222.56** (**12.71×**) | **290.27** (**16.57×**) | **393.60** (**22.47×**) |
| **8K UHD** (`7680x4320`) | **3.61** | **36.82** (**10.20×**) | **45.74** (**12.67×**) | **93.22** (**25.82×**) |

**The input paths are not interchangeable.** `HostPageableEndToEnd` is the primary host-to-host benchmark: its timing includes pageable-to-pinned staging, GPU transfers, CUDA processing, compressed D2H transfer, CFHD sample assembly, and synchronization. `HostPinnedApi` omits pageable staging. `DeviceInput` begins with frames already in GPU memory and **must not be presented as file-transcoding throughput**.

**Test platform:** AMD Ryzen 9 7940HS and GeForce RTX 4090 Laptop GPU (`sm_89`, 16,375 MiB VRAM), Windows x64, CUDA 12.8.61, MSVC Release. The measurement excludes encoder/context creation, input generation, one-time CUDA Graph capture, and filesystem output. A warmup-aligned stateful equivalence gate checks source-frame sequence, quantizer state, sample sizes, and masked CFHD hashes before canonical results are accepted.

**Explore the evidence:** [Canonical JSON](benchmarks/phase3a2c_release_candidate.json) · [Complete figures, latency and GPU telemetry](docs/BENCHMARK_DETAILS.md) · [Benchmark methodology and history](docs/PHASE3A1_BENCHMARK_HARDENING.md)

## Technical scope

**NVIDIA CUDA encoding.** The accelerated `Phase3AEncoderContext` processes progressive 8-bit `BGRA` (`CFHD_PIXEL_FORMAT_BGRA`) to CFHD `YUV 4:2:2` (`CFHD_ENCODED_FORMAT_YUV_422`) at **10-bit codec precision**, `FilmScan 3`, `GOP = 1`. Intermediate processing uses wider integer buffers; internal storage width is not the encoded bit depth. It uses a three-level wavelet and stateful dynamic quantization (`Phase3AQuantMode::DynamicUpstreamState`) to match upstream `QuantizationSetQuality` / `FSratelimiter` transitions.

**CPU-only reference.** `-DHYPERCINE_ENABLE_CUDA=OFF` compiles the frozen SDK and HyperCine's C++17 reference wrappers without the CUDA Toolkit. The documented reference configuration was verified on Windows x64 (MSVC 19.44) and configured for [CPU-only CI](.github/workflows/ci-cpu.yml). See the [format compatibility matrix](docs/COMPATIBILITY.md).

**Experimental Apple Silicon work.** A separate SDK-free CPU encoder supports fixed FS3-Q2 and an experimental stateful FilmScan 3 profile. It is **not** part of the CUDA performance comparison. See the [standalone encoder report](docs/MACOS_STANDALONE_ENCODER.md) and [FilmScan 3 validation](docs/MACOS_FILMSCAN3_CPU.md).

## Architecture

### Frozen upstream, separate implementation

The [GoPro SDK baseline](docs/UPSTREAM.md) is retained as an immutable oracle. Compiler settings, warnings, and workarounds are applied in HyperCine's build and wrapper layers rather than by patching `upstream/`. The overall design is outlined in [Architecture](docs/ARCHITECTURE.md).

### Three input memory contracts

**`HostPageableEndToEnd` — regular host buffers.** Includes pageable-to-pinned staging, H2D DMA, GPU encode, compressed D2H DMA, and host CFHD assembly. This is the primary public performance comparison.

**`HostPinnedApi` — caller-pinned host buffers.** Uses `cudaMallocHost` input and skips the staging copy, but still measures transfers and assembly.

**`DeviceInput` — GPU-resident integration.** Uses `cudaMalloc`-resident BGRA8 input for workflows such as in-VRAM grading or compositing. It measures GPU encode, compressed D2H, and host assembly—not storage read or file transcoding.

### Encoding pipeline

**Frontend and overlap.** Asynchronous H2D transfer and `BgraToYuv` prepare frame `k+1` while frame `k` completes wavelet and entropy work in another slot.

**Wavelet and rate control.** `HyperCine_GpuSlotKickWaveletWithQuant` performs the three-level 2-6 biorthogonal wavelet transform and applies the frame-specific 36-entry `quant[]` table within its kernels. This stage stays outside the entropy CUDA Graph, so per-frame `FSratelimiter` updates require no graph recapture.

**Entropy and CUDA Graphs.** `FusedScanOptimized` processes 27 highpass subbands across four streams with fused run-length, VLC and radix-1024 prefix-scan operations. Its cached `cudaGraphExec_t` replays the graph per frame.

**Error behavior.** Analytical entropy-word capacity bounds and sticky `GpuStageStatus` failure propagation prevent out-of-bounds write assumptions and indefinite waits. See the [entropy design and capacity proof](docs/ENTROPY.md).

## Build and test

**Prerequisites:** Windows 10/11 x64, CMake 3.25+, and Visual Studio 2022 / MSVC 19.30+ with C++17 support. GPU builds also need CUDA Toolkit 12.x+ and a compatible NVIDIA driver. The Apple Silicon standalone target uses a separate configuration.

### CPU-only reference build

Builds the upstream SDK (`CFHDCodecStatic`) and HyperCine reference wrappers without CUDA.

```powershell
cmake -B build-cpu -S . -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=ON
cmake --build build-cpu --config Release
ctest --test-dir build-cpu -C Release --output-on-failure
```

### NVIDIA CUDA release build

Choose `CMAKE_CUDA_ARCHITECTURES` for the installed GPU (`89` for Ada Lovelace, `86` for Ampere).

```powershell
cmake -B build-release-sm89 -S . -DHYPERCINE_ENABLE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89 -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=ON
cmake --build build-release-sm89 --config Release
ctest --test-dir build-release-sm89 -C Release --output-on-failure
```

### Experimental SDK-free Apple Silicon build

This target does not compile or link the GoPro SDK or CUDA. The fixed FS3-Q2 mode has independent FFmpeg decoder validation at 128×128, 256×256 and 1920×1080. The separate stateful FilmScan 3 profile targets 1920×1080 BGRA8 → YUV422 p10, GOP 1. Consult the [standalone report](docs/MACOS_STANDALONE_ENCODER.md) and [FilmScan 3 report](docs/MACOS_FILMSCAN3_CPU.md) for geometry, validation and decoder limitations.

```sh
cmake -S . -B build-standalone -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_BENCHMARKS=OFF
cmake --build build-standalone --config Release --parallel
ctest --test-dir build-standalone -C Release --output-on-failure
./build-standalone/hypercine_encode_smoke gradient gradient.cfhd
```

### CUDA sanitizer checks

```powershell
compute-sanitizer --tool memcheck .\build-release-sm89\tests\Release\test_entropy_capacity.exe
compute-sanitizer --tool memcheck .\build-release-sm89\tests\Release\test_phase3a.exe --quick-sanitize
```

On Windows, Compute Sanitizer may request permission for local `127.0.0.1` IPC through Windows Firewall. No external network access is used.

### Reproduce the canonical benchmark

`benchmark_phase3a1.exe` requires a clean git worktree (including `upstream/`) for canonical publication. Generate the JSON outside the repository first. `--allow-dirty` is only for exploratory results marked `"publication": "DIRTY"`.

```powershell
# Canonical 5-trial benchmark suite (equivalence + headline + slots + graph) on a clean working tree:
.\build-release-sm89\benchmarks\Release\benchmark_phase3a1.exe --suite=all --trials=5 --out="$env:TEMP\phase3a2c_release_candidate.json"
Copy-Item "$env:TEMP\phase3a2c_release_candidate.json" "benchmarks\phase3a2c_release_candidate.json"

# Exploratory run on a modified working tree (marked "publication": "DIRTY"):
.\build-release-sm89\benchmarks\Release\benchmark_phase3a1.exe --allow-dirty --suite=headline --trials=1 --out="$env:TEMP\phase3a1_tmp.json"
```

## Limitations

The following are the **Phase 3A.2c release-candidate boundaries**, not a claim of compatibility with every codec mode or production editing host.

**CUDA release-candidate platform.** The Phase 3A.2c GPU path targets **Windows x64 + NVIDIA CUDA**. Vulkan is not part of this release. A **separate experimental Apple Metal frontend** exists for the SDK-free standalone encoder, but it is not the Phase 3A.2c CUDA pipeline. The original-SDK CPU reference build is verified on Windows x64 (MSVC 19.44) and in [CPU-only CI](.github/workflows/ci-cpu.yml); the cross-platform [standalone CI](.github/workflows/ci-standalone.yml) tests a different, SDK-free build. Do not conflate those validation scopes.

**Pixel Format & Compression Profile Scope.** The full GPU pipeline (`Phase3AEncoderContext`) currently accelerates 8-bit `BGRA` (`CFHD_PIXEL_FORMAT_BGRA`) $\to$ 12-bit internal `YUV 4:2:2` (`CFHD_ENCODED_FORMAT_YUV_422`) at `FilmScan 3` (`CFHD_ENCODING_QUALITY_FILMSCAN3`), progressive, `GOP = 1`. Other CineForm pixel formats (`v210`, `RG48`, `b64a`, `BYR4` Bayer RAW, `YUV 4:4:4`, alpha channels) fall back to the upstream CPU SDK.

**Encoder-Only GPU Acceleration.** Phase 3A.2c GPU acceleration is implemented for **encoding**, not decoding. Reference verification uses the original GoPro CPU decoder; some test paths limit its worker count via `TAG_CPU_MAX = 1`. Reference-decoder nondeterminism remains under investigation ([#120](https://github.com/louis-0hrs/HyperCine/issues/120)); successful API status alone does not establish identical decoded pixels.

**Research/Engineering API Packaging.** The multi-frame GPU encoder is exposed via internal C++ headers (`include/hypercine/phase3a.h`, `RunPhase3A`) rather than a drop-in DirectShow/QuickTime/VFW system installer or commercial NLE plugin bundle.

**External NLE & FFmpeg Interoperability.** The CUDA release-candidate equivalence gate compares masked bytes (`masked_diff_bytes == 0`) with pristine upstream `CFHD_EncodeSample` and performs upstream decoder checks. Separately, the **SDK-free standalone** workflow runs automated FFmpeg decode validation. Neither constitutes automated Adobe Premiere Pro / DaVinci Resolve import certification or a mandatory FFmpeg compatibility gate for the Phase 3A.2c CUDA output.

**Volatile Clip Metadata in Cross-Handle Comparisons.** In production mode (`deterministic_test_metadata = false`, the default), every encode writes live `TAG_CLIP_GUID`, `TAG_ENCODE_DATE`, `TAG_ENCODE_TIME`, and `TAG_TIMECODE` tags into the `CFHD` header. Cross-handle byte-identity tests mask only those 4 volatile metadata fields (`MaskCfhdVolatileMetadata`).

**Laptop GPU Clock & Thermal Variance.** On mobile GPUs (such as the tested RTX 4090 Laptop GPU), dynamic boost clocks and power/thermal caps (`SwPowerCap`, `SwThermalSlowdown`, or `GpuIdle` clock transitions between trials) introduce run-to-run variance (`~5–11%` stdev at 1080p/4K). All benchmark outputs therefore record 5-trial medians alongside full NVML clock, power, temperature, and throttle telemetry.

The decoder reproducibility and release-gating investigations are tracked separately in [#120](https://github.com/louis-0hrs/HyperCine/issues/120) and [#116](https://github.com/louis-0hrs/HyperCine/issues/116).

## Documentation

**Orientation:** [Architecture](docs/ARCHITECTURE.md) · [Compatibility matrix](docs/COMPATIBILITY.md) · [Upstream provenance and rules](docs/UPSTREAM.md)

**Performance:** [Canonical results in detail](docs/BENCHMARK_DETAILS.md) · [Benchmark methodology and historical comparison](docs/PHASE3A1_BENCHMARK_HARDENING.md) · [Benchmark results guide](benchmarks/results/README.md) · [Optimization opportunities](docs/PHASE3B_PERFORMANCE_OPPORTUNITIES.md)

**Implementation and correctness:** [Sequence correctness](docs/PHASE3A_SEQUENCE_CORRECTNESS.md) · [Entropy](docs/ENTROPY.md) · [CUDA Phase 2D](docs/CUDA_PHASE2D.md)

**Contributing:** [Contribution guide](CONTRIBUTING.md) · [Security policy](SECURITY.md)

## License and attribution

HyperCine is dual-licensed under the [MIT License](LICENSE-MIT) or [Apache License 2.0](LICENSE-APACHE), at your option. See [third-party notices](THIRD_PARTY_NOTICES.md).

Original GoPro CineForm SDK: Copyright © 2017 GoPro, Inc.; original [MIT](upstream/LICENSE.txt) and [Apache 2.0](upstream/LICENSE-APACHE) license texts.

HyperCine additions: Copyright © 2026 HyperCine Contributors.
