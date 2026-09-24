# HyperCine GPU Acceleration Roadmap (Phase 1)

This document outlines the concrete architectural roadmap and implementation plan for Phase 1 GPU acceleration in HyperCine, transitioning from the pristine CPU baseline to high-throughput hardware compute.

---

## 1. Bottleneck Profiling & Analysis

Based on upstream CPU execution analysis and the Phase 0 baseline benchmark results (AMD Ryzen 9 7940HS, 16 threads):

| Resolution | CPU Encode FPS | CPU Decode FPS | Target GPU Encode FPS | Target GPU Decode FPS |
|------------|----------------|----------------|-----------------------|-----------------------|
| **1080p**  | 77.7 fps       | 394.1 fps      | > 300 fps             | > 1,200 fps           |
| **4K UHD** | 21.8 fps       | 125.3 fps      | > 100 fps             | > 450 fps             |
| **8K UHD** | 4.3 fps        | 28.0 fps       | > 30 fps              | > 120 fps             |

### Runtime Distribution by Stage
Profiling the upstream CineForm codec reveals the computational breakdown across pipeline stages:

```
┌──────────────────────────────────────────────────────────────┐
│ Wavelet Transform (Spatial Filtering 2-6): 55% - 65%         │
├──────────────────────────────────────────────────────────────┤
│ Color Conversion & Format Packing: 15% - 20%                 │
├──────────────────────────────────────────────────────────────┤
│ Quantization / Dequantization: 10% - 15%                     │
├──────────────────────────────────────────────────────────────┤
│ Entropy Coding (Run-Length & VLC) / Bitstream: 10% - 15%     │
└──────────────────────────────────────────────────────────────┘
```

1. **Wavelet Transform (`spatial.c`, `wavelet.c`):**
   - The horizontal (`H26Wavelet`) and vertical (`V26Wavelet`) 2-6 biorthogonal wavelet filters dominate CPU cycle consumption.
   - Operations are 100% data-parallel across rows and columns with fixed FIR stencil taps.
   - **GPU Viability:** Extremely High. Fits into CUDA shared memory tiles with zero bank conflicts.
2. **Color Conversion (`RGB2YUV.c`, `YUV2RGB.c`):**
   - Pixel-independent matrix operations with clipping and scale offsets.
   - **GPU Viability:** Extremely High. High arithmetic throughput via float/half or vectorized int32 SIMD.
3. **Quantization / De-quantization (`quantize.c`):**
   - Element-wise integer division / multiplication with dead-zone thresholding.
   - **GPU Viability:** Extremely High. Can be fused directly into the terminal wavelet synthesis/decomposition pass to eliminate memory roundtrips.
4. **Entropy Coding (`entropy.c`, `bitstream.c`):**
   - Serial variable-length coding (VLC) and run-length states with bit-level accumulation.
   - High branch divergence on GPU.
   - **Phase 1 Strategy:** Maintain entropy coding on CPU host worker threads, or implement a hybrid staged pipeline.

---

## 2. Phase 1 Architecture: Hybrid CPU-GPU Pipeline

Phase 1 focuses on offloading the primary computational bottlenecks to an NVIDIA CUDA backend while preserving compatibility and bit-exact equivalence.

### Target Decode Pipeline Flow

```
  [Bitstream] (Host Memory)
       │
       ▼ (CPU)
  ┌─────────────────────────────────┐
  │ Bitstream Parsing & VLC Decode  │  CPU Host Worker Threads
  │ (Produces Quantized Subbands)   │  (High single-thread branch efficiency)
  └────────────────┬────────────────┘
                   │
                   ▼ (CUDA Async DtoH Transfer via Pinned Host Memory)
  ┌─────────────────────────────────┐
  │ Host-to-Device Memory Transfer  │  Overlapped on CUDA Stream 1
  └────────────────┬────────────────┘
                   │
                   ▼ (GPU Kernel 1)
  ┌─────────────────────────────────┐
  │ Fused Dequantization &          │  Reconstructs subbands into
  │ Multi-Level Inverse 2-6 Wavelet │  spatial YUV planes in Shared Memory
  └────────────────┬────────────────┘
                   │
                   ▼ (GPU Kernel 2)
  ┌─────────────────────────────────┐
  │ Color Space Conversion Kernel   │  YUV 4:2:2 -> BGRA / RGBA / RGB48
  │ & Gamma / Pedestal Map          │  Direct write to target frame buffer
  └────────────────┬────────────────┘
                   │
                   ▼ (CUDA Async DtoH Transfer or Direct D3D12/Vulkan Interop)
  [Render / Host Frame] (GPU Surface or Host RAM)
```

### Target Encode Pipeline Flow

```
  [Source Frame] (Host RAM or GPU Texture)
       │
       ▼ (CUDA Async HtoD Transfer)
  ┌─────────────────────────────────┐
  │ Device Frame Ingestion          │  Pinned Host Memory -> GPU Device
  └────────────────┬────────────────┘
                   │
                   ▼ (GPU Kernel 1)
  ┌─────────────────────────────────┐
  │ Color Conversion Kernel         │  RGBA / BGRA -> YUV 4:2:2 / 4:4:4
  └────────────────┬────────────────┘
                   │
                   ▼ (GPU Kernel 2)
  ┌─────────────────────────────────┐
  │ Multi-Level Forward 2-6 Wavelet │  Separable Horizontal & Vertical
  │ Decomposition & Quantization    │  Fused dead-zone quantization
  └────────────────┬────────────────┘
                   │
                   ▼ (CUDA Async DtoH Transfer)
  ┌─────────────────────────────────┐
  │ Subband Transfer to Host        │  Quantized coefficients transferred to host
  └────────────────┬────────────────┘
                   │
                   ▼ (CPU Worker Threads)
  ┌─────────────────────────────────┐
  │ VLC Entropy Coding & Bitstream  │  Parallel CPU encoding of subbands
  └────────────────┬────────────────┘
                   │
                   ▼
  [CineForm Bitstream]
```

---

## 3. Wavelet CUDA Kernel Design

### 2D Cooperative Block Strategy
The 2-6 biorthogonal wavelet filter requires a stencil radius of 2 taps for lowpass and 3 taps for highpass:
- Stencil window: $x[2n-2], x[2n-1], x[2n], x[2n+1], x[2n+2], x[2n+3]$
- Coefficients: $[-1, -1, 8, 8, -1, -1] / 8$ or difference formulation $[-1, 9, 9, -1] / 16$.

### CUDA Implementation Details:
1. **Tile Dimension:** $32 \times 8$ or $16 \times 16$ threads per block.
2. **Shared Memory Halo Caching:**
   - Allocate `__shared__ int16_t tile[TILE_H + 2 * HALO][TILE_W + 2 * HALO]`.
   - Each thread loads internal samples plus boundary apron samples using clamp-to-edge mirroring (matching upstream `spatial.c` boundary behavior).
   - `__syncthreads()` barrier once apron is populated.
3. **Register Storage:**
   - Store filter weights in constant registers or `#pragma unroll` immediate literals.
   - Use integer SIMD intrinsics (`__vsubss2`, `__vaddss2` or standard 32-bit math for 16-bit packed components).
4. **Separable Passes vs. 2D Fused:**
   - *Pass 1 (Horizontal):* Filter rows into left/right subband buffers.
   - *Pass 2 (Vertical):* Filter columns into 4 subbands ($LL, LH, HL, HH$).
   - For levels $> 1$, recursively invoke the kernel on the $LL$ quadrant.

---

## 4. Host-Device Transfer Pipelining

To eliminate PCIe transfer latency bottlenecks at 8K resolutions (~132 MB per uncompressed RGBA frame):
1. **Pinned Host Memory (Page-Locked):**
   Allocate host frame buffers using `cudaHostAlloc` with `cudaHostAllocPortable | cudaHostAllocWriteCombined`.
2. **Multi-Stream Ring Buffer:**
   Implement a 3-stream circular pipeline (Ingest, Compute, Egress):
   - Stream 0: Transfer Frame $N+1$ from Host to Device.
   - Stream 1: Execute Wavelet & Color Kernels for Frame $N$.
   - Stream 2: Transfer Coefficients / Frame $N-1$ from Device to Host.
3. **Zero-Copy Direct Display Interop:**
   For real-time video playback applications, support direct CUDA-Direct3D11/12 and CUDA-Vulkan surface sharing via `cudaGraphicsRegisterResource` to avoid Host memory roundtrips completely.

---

## 5. Implementation Stages & Milestones

| Milestone | Scope | Deliverables |
|-----------|-------|--------------|
| **Phase 1.1** | CUDA Build Infrastructure & Wrappers | CMake FindCUDAToolkit integration, `src/cuda/` tree, device capability detection, host memory manager. |
| **Phase 1.2** | Color Space Kernels | CUDA kernels for BGRA/RGBA <-> YUV 4:2:2 and RGB48, validated against `tests/test_vectors.h`. |
| **Phase 1.3** | 2-6 Wavelet GPU Kernels | CUDA implementations of `H26Wavelet` and `V26Wavelet` forward and inverse transforms, validated against `test_transform_wavelet`. |
| **Phase 1.4** | Quantization Fusion | Fused quantization and dequantization passes, end-to-end coefficient matching with CPU codec. |
| **Phase 1.5** | Hybrid Pipeline Integration | Connect GPU wavelet engine with CPU entropy engine; verify bitstream roundtrip and measure speedup against `reference_windows_msvc.json`. |
| **Phase 1.6** | Multi-Stream Pipelining | Ring-buffered CUDA streams and pinned memory; reach 8K 60fps decode target. |
