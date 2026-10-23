# HyperCine Phase 0 Reference Baseline Report

> [!NOTE]
> **Historical Phase 0 snapshot.** The compiler environment, CTest inventory, performance numbers and reproduction commands below describe that original frozen-SDK study, **not** the latest HyperCine CUDA encoder or the current test suite. For the later canonical throughput record see the [benchmark guide](../benchmarks/results/README.md).

This document records the exact build configuration, test suite results, and hardware benchmark figures established during Phase 0 for the pristine GoPro CineForm SDK reference baseline.

---

## 1. Toolchain & Environment

- **Operating System:** Windows 11 x64
- **Host CPU:** AMD Ryzen 9 7940HS w/ Radeon 780M Graphics (8 Cores, 16 Threads @ 4.0 GHz base / 5.2 GHz boost)
- **Compiler:** Microsoft Visual C++ Compiler (MSVC) 19.44.35207.0 (Visual Studio 2022 Community)
- **Build System:** CMake 3.31.6-msvc6 (Ninja / MSVC generator)
- **Language Standard:** ISO C++17 (`/std:c++17`)
- **Upstream Commit:** `11574d0295771edccadd17af14af74a539f924c7`
- **External Dependencies:** Zero (standard C/C++ runtime only)

---

## 2. Build Instructions

### Configure & Build
From the repository root in PowerShell:
```powershell
# Configure out-of-source build
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release

# Build all libraries, tests, and benchmarks
cmake --build build --config Release
```

### Run Reference Tests
```powershell
ctest --test-dir build -C Release --output-on-failure
```

### Run Benchmarks
```powershell
.\build\Release\benchmark_codec.exe
```

---

## 3. Reference Test Suite Verification

The reference test suite exercises every major mathematical and operational layer of the CineForm codec without depending on external libraries or frameworks:

| Test Executable | Target Layer | Verification Method | Status |
|-----------------|--------------|---------------------|--------|
| `test_transform_wavelet` | 2-6 Biorthogonal Wavelet Math | Direct calls to `H26Wavelet`, `V26Wavelet`, and inverse functions. Verifies impulse response, horizontal/vertical filtering, linear phase, and roundtrip reconstruction bounds. | **PASS** (0.02s) |
| `test_codec_roundtrip` | Full Codec Pipeline | Encodes and decodes 9 synthetic deterministic test vectors across varying visual frequencies (Flat Field, Color Bars, Diagonal Ramp, High-Freq Checkerboard, Naturalistic Noise, Circle Impulse, Studio Pedestal, Banding Ramp, Salt & Pepper). Verifies PSNR $> 35\text{ dB}$, bitstream headers, and active metadata. | **PASS** (0.03s) |
| `test_avi_sample` | Real-World Bitstream Demux & Decode | Demuxes and decodes all 24 frames of `upstream/Example/videos/CFHDsample422.AVI`. Verifies frame dimensions ($1920 \times 1080$), format (`CFHD_PIXEL_FORMAT_YUY2`), decode status codes, and non-zero frame buffers. | **PASS** (0.15s) |

**Total CTest Execution Time:** 0.22 seconds (100% pass rate).

---

## 4. Reference Baseline Benchmark Metrics

The benchmark suite (`benchmarks/benchmark_codec.cpp`) runs multi-threaded encode and decode loops using the FilmScan 3 quality preset (`CFHD_ENCODING_QUALITY_FILM3`) in YUY2 / BGRA 4:2:2 mode across 1080p, 4K UHD, and 8K UHD.

Hardware: AMD Ryzen 9 7940HS (16 logical threads), Windows x64.
Source JSON: `benchmarks/results/reference_windows_msvc.json`.

| Metric | 1080p Full HD ($1920 \times 1080$) | 4K UHD ($3840 \times 2160$) | 8K UHD ($7680 \times 4320$) |
|---|---|---|---|
| **Frames Tested** | 50 frames | 25 frames | 10 frames |
| **Encode Throughput** | **77.70 FPS** (161.11 MP/s) | **21.84 FPS** (181.13 MP/s) | **4.28 FPS** (142.08 MP/s) |
| **Encode Avg Latency** | 12.56 ms | 44.79 ms | 229.71 ms |
| **Encode Min Latency** | 11.93 ms | 42.07 ms | 158.42 ms |
| **Encode Max Latency** | 14.70 ms | 49.17 ms | 266.60 ms |
| **Decode Throughput** | **394.15 FPS** (817.30 MP/s) | **125.31 FPS** (1,039.40 MP/s) | **27.96 FPS** (927.78 MP/s) |
| **Decode Avg Latency** | 2.54 ms | 7.98 ms | 35.76 ms |
| **Decode Min Latency** | 2.10 ms | 7.31 ms | 32.80 ms |
| **Decode Max Latency** | 3.10 ms | 9.08 ms | 38.24 ms |
| **Avg Compressed Frame** | 1,186,352 bytes (~1.19 MB) | 4,673,177 bytes (~4.67 MB) | 17,674,354 bytes (~17.67 MB) |
| **Compression Ratio** | 6.99 : 1 | 7.10 : 1 | 7.51 : 1 |
| **Peak Memory Footprint** | 92.5 MB | 229.3 MB | 614.2 MB |

---

## 5. Architectural Observations & GPU Acceleration Potential

1. **Decode vs. Encode Disparity:**
   The CPU decoder achieves $> 1\text{ Gigapixel/sec}$ at 4K because inverse synthesis and inverse quantization require fewer computations than forward multi-scale analysis. The CPU encoder is substantially more compute-bound (21.8 fps at 4K, 4.3 fps at 8K).
2. **8K Real-Time Bottleneck:**
   At 8K UHD, CPU encoding runs at 4.28 fps, far below real-time 24/30/60 fps capture requirements.
   Offloading 2-6 wavelet decomposition and color conversion to GPU kernels (targeting $> 30\text{ fps}$ encode and $> 120\text{ fps}$ decode) is both essential and achievable.
3. **Memory Footprint Scalability:**
   Peak memory scales linearly with frame resolution ($92\text{ MB} \to 229\text{ MB} \to 614\text{ MB}$). GPU device memory consumption will remain well within the 8 GB - 24 GB VRAM envelope of modern GPUs.
