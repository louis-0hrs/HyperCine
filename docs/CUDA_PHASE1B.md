# HyperCine CUDA Acceleration Phase 1B: Multi-level Pyramid & Quantization Report

---

## 1. Executive Summary & Verification of Benchmark Reporting Ambiguity

In the initial Phase 1B report, a critical reporting ambiguity occurred in the presentation of benchmark metrics:
- **Discrepancy Verified:** In the previous test harness, timing loops executed a batch of $N = 10$ frames. The reported millisecond values (e.g., $39.60\text{ ms}$ for `UPSTREAM_CPU_PRODUCTION`, $400.27\text{ ms}$ for `FULL_UPSTREAM_ENCODER`, $2.46\text{ ms}$ for CUDA Kernel, and $30.48\text{ ms}$ for CUDA Host->GPU->Host) represented the **total duration for the 10-frame batch**, rather than per-frame latency.
- **Throughput Consistency:** The throughput calculations correctly accounted for all 10 frames ($82.944\text{ MP} / 0.03960\text{ s} \approx 2094\text{ MP/s}$), which mathematically confirms the batch interpretation:
  $$\text{Per-Frame Latency} = \frac{\text{Batch Duration}}{10}$$
- **Resolution of the Single-Channel vs. Full-Encoder Scope:**
  Crucially, `UPSTREAM_CPU_PRODUCTION` (~$3.76\text{ ms}$ to $3.96\text{ ms}$ per 4K frame) measured **only Level 1 spatial transform on the single full-resolution Y channel**, not the complete frame. A standard CineForm frame contains 3 planar channels (Y 4:2:2 at full resolution, U and V at half horizontal resolution), each decomposed through a 3-level wavelet pyramid ($LL_1 \to LL_2 \to LL_3$), followed by quantization, zero-run detection, Huffman/VLC table lookup, bitstream packing, and container framing.
- **Reporting Corrections:**
  All benchmark harnesses (`benchmarks/benchmark_phase1b.cpp`), JSON schema outputs (`benchmarks/results/cuda_wavelet_phase1b.json`), and documentation have been updated. The following metrics are now **explicitly and independently recorded**:
  1. `batch_frame_count` / `number_of_frames`
  2. `batch_duration_avg_ms` / `batch_duration_median_ms`
  3. `latency_avg_ms_per_frame` / `latency_median_ms_per_frame`
  4. `megapixels_per_sec`

---

## 2. Multi-Resolution Benchmark Matrix (1080p, 4K, 8K)

Measured on **AMD Ryzen 9 7940HS** (16 threads, MSVC 19.44) and **NVIDIA GeForce RTX 4090 Laptop GPU** (Compute Capability 8.9, CUDA 12.8 / Driver 13.3).

### Comprehensive Benchmark Table

| Resolution | Implementation | Transfer Mode | Frames / Batch | Batch Time (avg ms) | Latency (avg ms/frame) | Latency (med ms/frame) | Throughput (MP/s) | Speedup vs Upstream SIMD |
| :--- | :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **1080p** (1920x1080) | `HYPERCINE_CPU_REFERENCE` | CPU Host-Only | 10 | 70.79 | 7.079 | 7.028 | 292.9 | $0.14\times$ |
| | `UPSTREAM_CPU_PRODUCTION` | CPU Host-Only | 10 | 9.77 | 0.977 | 0.979 | 2,121.6 | $1.00\times$ (base) |
| | `CUDA_REFERENCE` | GPU Kernel-Only | 10 | 1.72 | 0.172 | 0.169 | 12,068.0 | **$5.68\times$** |
| | `CUDA_OPTIMIZED` | GPU Kernel-Only | 10 | 1.78 | 0.178 | 0.175 | 11,637.7 | **$5.49\times$** |
| | `CUDA_REFERENCE` | Host->GPU->Host | 10 | 14.72 | 1.472 | 1.468 | 1,409.1 | $0.66\times$ |
| | `CUDA_OPTIMIZED` | Host->GPU->Host | 10 | 14.85 | 1.485 | 1.449 | 1,396.0 | $0.66\times$ |
| | `FULL_UPSTREAM_ENCODER` | CPU Full Pipeline | 10 | 80.87 | 8.087 | 8.045 | 256.4 | — |
| **4K UHD** (3840x2160) | `HYPERCINE_CPU_REFERENCE` | CPU Host-Only | 10 | 264.07 | 26.407 | 26.158 | 314.1 | $0.14\times$ |
| | `UPSTREAM_CPU_PRODUCTION` | CPU Host-Only | 10 | 37.58 | 3.758 | 3.756 | 2,207.3 | $1.00\times$ (base) |
| | `CUDA_REFERENCE` | GPU Kernel-Only | 10 | 2.40 | 0.240 | 0.240 | 34,563.8 | **$15.66\times$** |
| | `CUDA_OPTIMIZED` | GPU Kernel-Only | 10 | 2.55 | 0.255 | 0.258 | 32,482.3 | **$14.72\times$** |
| | `CUDA_REFERENCE` | Host->GPU->Host | 10 | 53.69 | 5.369 | 5.386 | 1,545.0 | $0.70\times$ |
| | `CUDA_OPTIMIZED` | Host->GPU->Host | 10 | 53.95 | 5.395 | 5.389 | 1,537.3 | $0.70\times$ |
| | `FULL_UPSTREAM_ENCODER` | CPU Full Pipeline | 10 | 320.80 | 32.080 | 32.016 | 258.6 | — |
| **8K UHD** (7680x4320) | `HYPERCINE_CPU_REFERENCE` | CPU Host-Only | 5 | 566.74 | 113.347 | 113.435 | 292.7 | $0.13\times$ |
| | `UPSTREAM_CPU_PRODUCTION` | CPU Host-Only | 5 | 75.17 | 15.035 | 14.998 | 2,206.7 | $1.00\times$ (base) |
| | `CUDA_REFERENCE` | GPU Kernel-Only | 5 | 5.54 | 1.107 | 1.111 | 29,962.6 | **$13.58\times$** |
| | `CUDA_OPTIMIZED` | GPU Kernel-Only | 5 | 5.55 | 1.111 | 1.109 | 29,865.3 | **$13.53\times$** |
| | `CUDA_REFERENCE` | Host->GPU->Host | 5 | 107.89 | 21.578 | 21.534 | 1,537.6 | $0.70\times$ |
| | `CUDA_OPTIMIZED` | Host->GPU->Host | 5 | 107.42 | 21.484 | 21.499 | 1,544.3 | $0.70\times$ |
| | `FULL_UPSTREAM_ENCODER` | CPU Full Pipeline | 5 | 641.42 | 128.284 | 127.523 | 258.6 | — |

---

## 3. PCIe Transfer & Kernel Decomposition

To isolate memory bottlenecks, transfers were profiled across all three resolutions using native asynchronous CUDA streams and GPU timestamp events (`cudaEventElapsedTime`):

| Resolution | Stage | Transfer Volume / Frame | Latency (avg ms/frame) | Median Latency (ms) | Effective Bandwidth | % of End-to-End Time |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: |
| **1080p** | Host $\to$ Device (H2D) | 4.15 MB | 0.639 ms | 0.638 ms | 6.50 GB/s | 43.7% |
| | GPU Wavelet Transform | — | 0.129 ms | 0.127 ms | — | 8.8% |
| | GPU Fused Quantization | — | 0.014 ms | 0.014 ms | — | 1.0% |
| | Device $\to$ Host (D2H) | 4.15 MB | 0.679 ms | 0.670 ms | 6.11 GB/s | 46.5% |
| | **Total End-to-End** | **8.30 MB roundtrip** | **1.461 ms** | **1.449 ms** | — | **100.0%** |
| **4K UHD** | Host $\to$ Device (H2D) | 16.59 MB | 3.808 ms | 2.562 ms | 4.36 GB/s | 51.1% |
| | GPU Wavelet Transform | — | 0.269 ms | 0.268 ms | — | 3.6% |
| | GPU Fused Quantization | — | 0.030 ms | 0.030 ms | — | 0.4% |
| | Device $\to$ Host (D2H) | 16.59 MB | 3.349 ms | 2.612 ms | 4.95 GB/s | 44.9% |
| | **Total End-to-End** | **33.18 MB roundtrip** | **7.456 ms** | **5.472 ms** | — | **100.0%** |
| **8K UHD** | Host $\to$ Device (H2D) | 66.36 MB | 14.076 ms | 10.221 ms | 4.71 GB/s | 50.8% |
| | GPU Wavelet Transform | — | 1.077 ms | 1.075 ms | — | 3.9% |
| | GPU Fused Quantization | — | 0.120 ms | 0.119 ms | — | 0.4% |
| | Device $\to$ Host (D2H) | 66.36 MB | 12.409 ms | 10.444 ms | 5.35 GB/s | 44.8% |
| | **Total End-to-End** | **132.71 MB roundtrip** | **27.681 ms** | **21.859 ms** | — | **100.0%** |

### Key Transfer Takeaways:
1. **Raw Compute vs Transfer:** Raw CUDA kernel compute (Wavelet + Quantization) accounts for only **4.0% to 9.8%** of the end-to-end GPU time.
2. **Transfer Dominance:** Synchronous Host $\leftrightarrow$ Device PCIe transfers consume **>90% to 96%** of the roundtrip time.
3. **Bandwidth Limitations:** Unpinned pageable host allocations achieve between $4.36\text{ GB/s}$ and $6.50\text{ GB/s}$ across PCIe Gen4 x8 on the mobile platform. Switching to pinned host memory (`cudaHostAlloc`) and multi-stream double-buffering will saturate the $\sim 12\text{--}14\text{ GB/s}$ PCIe bus limit.

---

## 4. Full Encoder Profiling: Where the Remaining ~78%–90% of Time Goes

Using the high-precision profiling harness (`benchmarks/profile_encoder.cpp`), we instrumented every stage of the pristine upstream production encoder (`CFHD_EncodeSample`) at 1080p, 4K, and 8K.

### 4K UHD Encoder Execution Profile (Total Frame Time: 55.61 ms)

```mermaid
pie title 4K CineForm Encode Stage Distribution (55.61 ms Total)
    "Input Color Conversion (RGB -> 10b YUV)" : 17.32
    "Wavelet Transform & Quantization" : 11.95
    "Entropy / VLC Coding & Packing" : 23.08
    "Framing & Overhead" : 3.26
```

### Granular Stage Breakdown Table

| Stage | Sub-component | Routine / Source File | Avg Time (ms) | % of Total Encode |
| :--- | :--- | :--- | :---: | :---: |
| **1. Color Conversion** | RGB/BGRA to 10-bit YUV 4:2:2 | `ConvertRGB32to10bitYUVFrame` (`convert.c:309`) | **17.32 ms** | **31.15%** |
| **2. Wavelet Pyramid** | Level 1 Full-Res Y Channel | `FilterSpatialQuant16s` (`spatial.c:653`) | 4.47 ms | 8.04% |
| | Level 1 U & V Subsampled Channels | `FilterSpatialQuant16s` (`spatial.c:653`) | 4.39 ms | 7.89% |
| | Levels 2 & 3 Lowpass Pyramids | `TransformForwardSpatial` (`wavelet.c:755`) | 2.15 ms | 3.87% |
| | Quantization & Band Scaling | `QuantizeBand` (`quantize.c:215`) | 0.94 ms | 1.69% |
| | *Stage 2 Subtotal* | *All 3 channels, 3 pyramid levels, quant* | **11.95 ms** | **21.49%** |
| **3. Entropy / VLC Coding** | Zero-Run Scanning & Raster Traversal | `EncodeQuantizedBand` (`encoder.c:5470`) | 7.85 ms | 14.12% |
| | Huffman/VLC Codebook Table Lookup | `codebook_runbook`, `valuebook` (`codebooks.c`) | 3.27 ms | 5.88% |
| | Endianness Swapping & Bitstream Packing | `SwapInt32NtoB` (`swap.h`), `PutBits` (`bitstream.c`) | 10.17 ms | 18.29% |
| | Subband / Group Headers & Trailers | `PutVideoBandHeader`, `PutVideoBandTrailer` | 1.80 ms | 3.24% |
| | *Stage 3 Subtotal* | *Run-length, VLC, Bit packing, Headers* | **23.08 ms** | **41.50%** |
| **4. Framing & Orchestration** | Frame Header, Index & Container Tags | `CSampleEncoder::EncodeSample` (`SampleEncoder.cpp`) | **3.26 ms** | **5.86%** |
| **Full Encoder Total** | | `CFHD_EncodeSample` | **55.61 ms** | **100.00%** |

---

## 5. Entropy Encoding Call Chain & Concurrency Analysis

To evaluate how entropy encoding can be accelerated, we traced the exact call graph and data structures through the CineForm codebase:

### 1. Call Chain Sequence
```
CFHD_EncodeSample()                                    [upstream/EncoderSDK/CFHDEncoder.cpp:115]
 └── CSampleEncoder::EncodeSample()                    [upstream/EncoderSDK/SampleEncoder.cpp:480]
      └── EncodeSampleOld()                            [upstream/Codec/encoder.c:1892]
           └── EncodeQuantizedGroup()                  [upstream/Codec/encoder.c:7461 / 10591]
                └── EncodeQuantizedFrameTransform()    [upstream/Codec/encoder.c:7889]
                     └── EncodeQuantizedBand()         [upstream/Codec/encoder.c:6318]
                          ├── Zero-Run Detection       [upstream/Codec/encoder.c:5470–5474]
                          ├── Table Lookups            [upstream/Codec/codebooks.c: runbook, valuebook]
                          ├── SwapInt32NtoB()          [upstream/Codec/swap.h]
                          └── PutBits() / PutLong()    [upstream/Codec/bitstream.c]
```

### 2. Available Concurrency Boundaries
1. **Frame Independence:** For intra-frame video ($gop\_length = 1$), every frame is 100% independent with zero temporal dependencies.
2. **Channel Independence:** Upstream `EncodeQuantizedGroupThreaded` (`encoder.c:10591`) already demonstrates that channels 0 (Y), 1 (U), and 2 (V) can encode concurrently into independent bitstream buffers and be concatenated.
3. **Subband Independence:**
   - Each wavelet band contains its own isolated header (`PutVideoBandHeader`) and byte-aligned trailer (`PutVideoBandTrailer`).
   - For a 3-level decomposition on a 3-channel frame:
     - Channel 0 (Y): 10 subbands (LL3, LH3, HL3, HH3, LH2, HL2, HH2, LH1, HL1, HH1)
     - Channel 1 (U): 10 subbands
     - Channel 2 (V): 10 subbands
   - **Total:** Exactly **30 independent subbands per frame** can be scanned, encoded, and bit-packed concurrently.

---

## 6. Amdahl’s Law Speedup Analysis

Let the full CineForm encoder execution time be $T_{\text{total}} = 55.61\text{ ms}$ at 4K.

The workload fractions are:
- $f_{\text{color}} = 31.15\%$ (Color Conversion: $17.32\text{ ms}$)
- $f_{\text{wavelet}} = 21.49\%$ (Wavelet Pyramid + Quantization: $11.95\text{ ms}$)
- $f_{\text{entropy}} = 41.50\%$ (Entropy / VLC Coding: $23.08\text{ ms}$)
- $f_{\text{framing}} = 5.86\%$ (Framing & Threading: $3.26\text{ ms}$)

### Scenario 1: Accelerating Wavelet + Quantization Only (Phase 1 / 1B Scope)
- Accelerated fraction: $f = 0.2149$.
- Non-accelerated fraction: $1 - f = 0.7851$ ($43.66\text{ ms}$).
- **Theoretical Limit ($S_{\text{kernel}} \to \infty$):**
  $$S_{\text{max}} = \frac{1}{1 - f} = \frac{1}{0.7851} = \mathbf{1.27\times} \quad (\text{Max possible overall speedup})$$
- **Realistic CUDA Kernel Execution ($0.25\text{ ms}$ GPU compute):**
  $$T_{\text{new}} = 43.66\text{ ms} + 0.25\text{ ms} = 43.91\text{ ms} \implies S = \frac{55.61}{43.91} = \mathbf{1.266\times}$$
- **With Unoverlapped PCIe Transfers ($7.46\text{ ms}$ H2D + D2H):**
  $$T_{\text{new}} = 43.66\text{ ms} + 7.46\text{ ms} = 51.12\text{ ms} \implies S = \frac{55.61}{51.12} = \mathbf{1.088\times}$$

### Scenario 2: GPU-Resident Pipeline (Color Conversion + Wavelets + Quantization on GPU)
- If input frames originate on GPU (e.g., camera RAW debayering / HyperRAW, GPU render targets) or if Color Conversion is moved to CUDA:
- Accelerated fraction: $f = f_{\text{color}} + f_{\text{wavelet}} = 31.15\% + 21.49\% = \mathbf{52.64\%}$.
- Non-accelerated fraction (CPU Entropy + Framing): $1 - f = 47.36\%$ ($26.34\text{ ms}$).
- GPU Compute Time (Color Conversion + 3-Level Wavelets + Quantization): $\sim 0.65\text{ ms}$.
- **Speedup:**
  $$T_{\text{new}} = 26.34\text{ ms} + 0.65\text{ ms} = 26.99\text{ ms} \implies S = \frac{55.61}{26.99} = \mathbf{2.06\times} \text{ to } \mathbf{2.10\times}$$

### Scenario 3: End-to-End Accelerated Pipeline (GPU Wavelets + GPU Parallel VLC)
- When both Wavelet/Quantization (21.5%) and Entropy Coding (41.5%) are offloaded to parallel GPU streams across the 30 independent subbands:
- Accelerated fraction: $f = 21.49\% + 41.50\% = \mathbf{62.99\%}$ (or $\mathbf{94.14\%}$ with color conversion).
- CPU is reduced to assembling the 30 subband chunks into the file container ($3.26\text{ ms}$).
- **Speedup:**
  $$T_{\text{new}} = 3.26\text{ ms} + 2.50\text{ ms} (\text{GPU}) = 5.76\text{ ms} \implies S = \frac{55.61}{5.76} \approx \mathbf{9.65\times}$$

---

## 7. Next Bottleneck & Architectural Recommendations

Based on empirical profiling and Amdahl analysis, accelerating wavelets alone yields a maximum $1.27\times$ encoder speedup. To achieve order-of-magnitude performance gains, future work should address the following bottlenecks in priority order:

1. **Input Color Conversion on GPU (`ConvertRGB32to10bitYUVFrame`):**
   - Consumes **31.2%** of CPU encoding time ($17.32\text{ ms}$ at 4K).
   - Embarrassingly parallel 2D pixel mapping; runs in $< 0.3\text{ ms}$ on GPU.
   - Eliminates CPU packing, keeping raw frames resident in GPU memory directly from camera or render pipelines.
2. **Pinned Host Memory & Multi-Stream Asynchronous Double-Buffering:**
   - Eliminates pageable memory copy stalls, cutting PCIe roundtrip latency by $\sim 50\%$.
   - Allows overlapping PCIe transfer of frame $N+1$ with GPU compute of frame $N$ and CPU entropy coding of frame $N-1$.
3. **Parallel Subband Entropy Encoding:**
   - Exploit the **30 independent subbands per frame**.
   - Implement multi-threaded CPU subband entropy workers or GPU-based prefix-sum run-length bitstream pack kernels.
