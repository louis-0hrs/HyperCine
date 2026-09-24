# HyperCine

**HyperCine** is a modern, high-performance, GPU-accelerated implementation of the GoPro CineForm codec (SMPTE VC-5), designed for ultra-fast digital intermediate editing, real-time 4K/8K multi-stream playback, and high-fidelity video mastering.

HyperCine maintains 100% bitstream and format compatibility with the official GoPro CineForm SDK, Adobe Premiere Pro, DaVinci Resolve, Apple Final Cut Pro, and FFmpeg.

---

## Key Highlights

- **Pristine Reference Baseline:** Integrates the upstream GoPro CineForm SDK (`11574d0295771edccadd17af14af74a539f924c7`) as an immutable reference foundation.
- **Zero External Core Dependencies:** Pure ISO C++17 with native platform GPU compute backends (CUDA, Vulkan, Metal) and standard CMake/CTest.
- **High-Coverage Test Suite:** Deterministic synthetic test vectors, 2-6 biorthogonal wavelet math verification, and real-world AVI bitstream validation.
- **High-Performance Benchmarking:** Automated multi-resolution throughput, latency, and peak memory benchmarking (1080p, 4K UHD, 8K UHD).
- **Dual Licensed:** Dual-licensed under the **MIT License** OR **Apache License, Version 2.0**, fully preserving all upstream GoPro copyrights.

---

## Baseline Performance (Phase 0 CPU Reference)

Benchmarked on **AMD Ryzen 9 7940HS** (8 cores / 16 threads @ 4.0–5.2 GHz), Windows 11 x64, MSVC 19.44 (Visual Studio 2022 Community), FilmScan 3 Quality:

| Resolution | Frames | Encode FPS | Encode Latency (avg) | Decode FPS | Decode Latency (avg) | Compression Ratio | Peak RAM |
|------------|--------|------------|----------------------|------------|----------------------|-------------------|----------|
| **1080p Full HD** ($1920 \times 1080$) | 50 | **77.70 FPS** (161 MP/s) | 12.56 ms | **394.15 FPS** (817 MP/s) | 2.54 ms | 6.99 : 1 | 92 MB |
| **4K UHD** ($3840 \times 2160$) | 25 | **21.84 FPS** (181 MP/s) | 44.79 ms | **125.31 FPS** (1,039 MP/s) | 7.98 ms | 7.10 : 1 | 229 MB |
| **8K UHD** ($7680 \times 4320$) | 10 | **4.28 FPS** (142 MP/s) | 229.71 ms | **27.96 FPS** (928 MP/s) | 35.76 ms | 7.51 : 1 | 614 MB |

*Detailed benchmark metrics are saved in [`benchmarks/results/reference_windows_msvc.json`](benchmarks/results/reference_windows_msvc.json).*

---

## Quick Start & Build Instructions

### Prerequisites
- Windows 10/11 x64, Linux x86_64, or macOS
- CMake 3.25 or newer
- Visual Studio 2022 (MSVC 19.30+) or Clang 16+ / GCC 12+
- ISO C++17 support

### Building HyperCine
```powershell
# Clone the repository
git clone https://github.com/your-username/HyperCine.git
cd HyperCine

# Configure build with CMake
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release

# Compile all targets (libraries, tests, benchmarks)
cmake --build build --config Release
```

### Running the Reference Test Suite
```powershell
ctest --test-dir build -C Release --output-on-failure
```
*Expected output: 3/3 tests pass (100% pass rate) in < 1 second.*

### Running the Benchmark Suite
```powershell
.\build\Release\benchmark_codec.exe
```

---

## Documentation

Comprehensive architecture, upstream reference notes, and roadmap specifications are available in the [`docs/`](docs/) directory:

- [**Architecture Overview**](docs/ARCHITECTURE.md): Deep-dive into CineForm 2-6 biorthogonal wavelet math, quantization, entropy coding, and threading models.
- [**Upstream Provenance & Quirks**](docs/UPSTREAM.md): Upstream git provenance, dual licensing, immutability rules, and documented quirks.
- [**Phase 1 GPU Acceleration Plan**](docs/GPU_PLAN.md): Profiling bottlenecks, CUDA shared memory wavelet kernel design, stream pipelining, and roadmap milestones.
- [**Format & Bitstream Compatibility**](docs/COMPATIBILITY.md): Supported pixel layouts (`YUY2`, `BGRA`, `RGBA`, `RGB48`, `b64a`, Bayer RAW), quality levels, and SMPTE VC-5 compatibility guarantees.
- [**Phase 0 Reference Baseline Report**](docs/REFERENCE_BASELINE.md): Build environment, hardware specifications, test summaries, and baseline benchmark numbers.
- [**Agent Guidelines & Rules**](AGENTS.md): Mandatory 11 rules for AI agents and developers contributing to HyperCine.

---

## License

HyperCine is dual-licensed under the **MIT License** and the **Apache License, Version 2.0**.
See [LICENSE-MIT](upstream/LICENSE.txt) and [LICENSE-APACHE](upstream/LICENSE-APACHE.txt) for details.

Original CineForm SDK: Copyright (c) 2017 GoPro, Inc.  
HyperCine: Copyright (c) 2026 HyperCine Contributors.
