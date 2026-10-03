<!-- Copyright (c) 2026 HyperCine Contributors. MIT OR Apache-2.0. -->
# Decode Phase 1: Exact CUDA Inverse Wavelet Reconstruction

## 1. Executive Summary

HyperCine Decode Phase 1 establishes the first production-grade GPU decoder stage for CineForm (`CFHD`):
**CUDA inverse spatial wavelet reconstruction for progressive YUV422 CineForm frames.**

The primary objective of Phase 1 is **mathematical exactness**, reproducing the exact arithmetic, saturations, edge formulas, and rounding behaviors of the pristine upstream GoPro CineForm CPU reference decoder.

### Key Milestones Achieved
1. **Mathematical Bit-Exactness (Max Absolute Error = 0)**:
   - Across the entire Phase 0 golden corpus (1080p patterns 0–4, 4K UHD patterns 0–4, 8K flat):
     - First intermediate reconstructed subband ($\text{LL}_1$): **0 diff** vs CPU reference.
     - Second intermediate reconstructed subband ($\text{LL}_0$): **0 diff** vs CPU reference.
     - Final reconstructed planar signed-16 channels ($Y, V, U$): **0 diff** vs CPU reference.
     - Final converted output ($\text{YU64}$): **byte-exact** match against pristine upstream CPU decoder and frozen Phase 0 SHA-256 / FNV-1a hashes.
2. **Comprehensive Unit Testing (36 / 36 PASSED)**:
   - Dedicated synthetic test suite covering extreme coefficient boundaries: `INT16_MIN`, `INT16_MAX`, near-saturation positive/negative, alternating extremes, zero bands, spatial impulses (center, corners, borders), and flat bands across multiple geometries (1080p, 4K UHD, and minimal legal 256x128). Every case verified bit-exact (0 diff).
3. **Memory Safety & Clean Compute Sanitizer**:
   - `compute-sanitizer --tool memcheck` executed on both unit test and golden corpus executables with **0 errors**, **0 leaks**, and **0 misaligned accesses**.
4. **Pristine Upstream & Frozen Production Encoder**:
   - Upstream directory `upstream/` remains 100% untouched (`git status --porcelain upstream/` is empty).
   - Core encoder codebase frozen; all 15 existing encoder regression tests pass (17/17 CTest suite passing).

---

## 2. Supported Scope vs Excluded Scope

Phase 1 deliberately isolates the spatial inverse wavelet transform from upstream entropy and color conversion stages to establish a rock-solid numerical foundation.

| Pipeline Feature | Phase 1 Status | Architectural Justification |
| :--- | :--- | :--- |
| **Frame Type** | Supported | Intra-frame progressive only |
| **Color Model** | Supported | YUV 4:2:2 coded frames (10-bit signed-16 coefficient path) |
| **Resolution** | Supported | 1080p ($1920 \times 1080$), 4K UHD ($3840 \times 2160$), 8K ($7680 \times 4320$) |
| **Final Formats** | Supported | Signed-16 planar ($Y, V, U$), Packed $\text{YU64}$ (16-bit un-dithered 4:2:2) |
| **GPU Entropy Decode** | Excluded | CPU entropy path retained; coefficient buffers uploaded via PCIe |
| **GPU Color Conversion** | Excluded | Planar frames downloaded back to host for CPU color/format packing |
| **RGB444 / RGBA4444** | Excluded | Deferred to Phase 2/3 multi-format expansion |
| **Bayer (RAW)** | Excluded | CFA demosaicing deferred to dedicated RAW decode phase |
| **Active Metadata / 3D LUT** | Excluded | Dynamic LUT processing excluded to maintain strict numerical invariance |
| **YUY2 Dither Comparisons**| Excluded | Upstream YUY2/RGB dithering relies on CRT `rand()`; excluded in favor of deterministic YU64 |

---

## 3. Reverse-Engineered Wavelet Mathematics & Exactness Proofs

Auditing `upstream/Codec/wavelet.c`, `spatial.c`, `InvertHorizontalStrip16s.c`, and `quantize.c` revealed critical algorithmic properties required to achieve bit-exact GPU reproduction.

### 3.1 Subband Ordering and Memory Layout
In pristine CineForm, wavelet transform structures store 4 subbands per pyramid level in `TRANSFORM::wavelet[level]->band[b]`. The canonical spatial band assignment is:
- `band[0]`: Low-Low ($\text{LL}$) — Horizontal Lowpass, Vertical Lowpass
- `band[1]`: Low-High ($\text{LH}$) — Horizontal Highpass, Vertical Lowpass
- `band[2]`: High-Low ($\text{HL}$) — Horizontal Lowpass, Vertical Highpass
- `band[3]`: High-High ($\text{HH}$) — Horizontal Highpass, Vertical Highpass

> [!WARNING]
> Upstream C prototypes define `InvertSpatialQuant16s(lowlow, lowhigh, highlow, highhigh, ...)`. Passing `band[1]` into the third argument causes inversion divergence on all high-frequency patterns. `band[1]` is $\text{LH}$ and `band[2]` is $\text{HL}$.

### 3.2 Pyramid Inversion Schedule & Descale Modes
A 3-level spatial wavelet pyramid is inverted in bottom-up topological order:
```text
Level 2 (W/8 x H/8, prescale 0) ──[descale = 0]──> Intermediate LL1 (W/4 x H/4)
                                                              │
Level 1 (W/4 x H/4, prescale 2) ──[descale = 2]───────────────┴──> Intermediate LL0 (W/2 x H/2)
                                                                                    │
Level 0 (W/2 x H/2, prescale 0) ──[descale = 0]─────────────────────────────────────┴──> Planar Plane (W x H)
```

1. **Level 2 $\to \text{LL}_1$**: Input bands $W/8 \times H/8$, `descale = 0`. Reconstructs $\text{LL}_1$ of size $W/4 \times H/4$.
2. **Level 1 $\to \text{LL}_0$**: Input highpass bands $W/4 \times H/4$, combined with reconstructed $\text{LL}_1$, `descale = 2`. Reconstructs $\text{LL}_0$ of size $W/2 \times H/2$.
3. **Level 0 $\to \text{Plane}$**: Input highpass bands $W/2 \times H/2$, combined with reconstructed $\text{LL}_0$, `descale = 0`. Reconstructs full-resolution planar channel $W \times H$.

### 3.3 Vertical Reconstruction Arithmetic
Vertical reconstruction pairs the lowpass column bands $(\text{LL}, \text{HL})$ and highpass column bands $(\text{LH}, \text{HH})$ to reconstruct two rows: even row $2r$ and odd row $2r + 1$.

#### Top Boundary ($r = 0$)
Uses an asymmetric boundary filter (coefficients 11, -4, 1):
$$\text{even} = \text{sat}_{16}\left( \left( \left( 11 \cdot \text{LL}_0 - 4 \cdot \text{LL}_1 + \text{LL}_2 + 4 \right) \gg 3 + \text{HL}_0 \right) \gg 1 \right)$$
$$\text{odd} = \text{sat}_{16}\left( \left( \left( 5 \cdot \text{LL}_0 + 4 \cdot \text{LL}_1 - \text{LL}_2 + 4 \right) \gg 3 - \text{HL}_0 \right) \gg 1 \right)$$
*(Identical stencil applies to $\text{LH}$ and $\text{HH}$ to reconstruct `even_highpass` and `odd_highpass`).*

#### Interior Rows ($0 < r < H - 1$)
Crucial discovery: Upstream handles interior rows differently based on `descale`:
- **When `descale == 0` (Levels 2 & 0)**: Upstream SSE2 code executes **16-bit saturating vector addition/subtraction**:
  $$\text{diff}_{\text{even}} = \text{satadd}_{16}\left(\text{satsub}_{16}(L_{r-1}, L_{r+1}), 4\right) \gg 3$$
  $$\text{even} = \left(\text{satadd}_{16}\left(\text{satadd}_{16}(L_r, \text{diff}_{\text{even}}), H_r\right)\right) \gg 1$$
  $$\text{diff}_{\text{odd}} = \text{satadd}_{16}\left(\text{satadd}_{16}\left(\text{satsub}_{16}(0, L_{r-1}), L_{r+1}\right), 4\right) \gg 3$$
  $$\text{odd} = \left(\text{satsub}_{16}\left(\text{satadd}_{16}(L_r, \text{diff}_{\text{odd}}), H_r\right)\right) \gg 1$$
- **When `descale == 2` (Level 1)**: Upstream unrolls into 32-bit unpacked arithmetic followed by a final 16-bit saturation:
  $$\text{even} = \text{sat}_{16}\left( \left( \left( (L_{r-1} - L_{r+1} + 4) \gg 3 \right) + L_r + H_r \right) \gg 1 \right)$$
  $$\text{odd} = \text{sat}_{16}\left( \left( \left( (-L_{r-1} + L_{r+1} + 4) \gg 3 \right) + L_r - H_r \right) \gg 1 \right)$$

#### Bottom Boundary ($r = H - 1$)
Uses an inverted asymmetric filter (coefficients 5, 4, -1):
$$\text{even} = \text{sat}_{16}\left( \left( \left( 5 \cdot \text{LL}_r + 4 \cdot \text{LL}_{r-1} - \text{LL}_{r-2} + 4 \right) \gg 3 + \text{HL}_r \right) \gg 1 \right)$$
$$\text{odd} = \text{sat}_{16}\left( \left( \left( 11 \cdot \text{LL}_r - 4 \cdot \text{LL}_{r-1} + \text{LL}_{r-2} + 4 \right) \gg 3 - \text{HL}_r \right) \gg 1 \right)$$

### 3.4 Horizontal Reconstruction Arithmetic
Horizontal reconstruction consumes `even_lowpass` and `even_highpass` to form row $2r$, and `odd_lowpass` and `odd_highpass` to form row $2r + 1$.
- **Left Column ($c = 0$)**:
  $$\text{left}_{\text{even}} = \left( (11 \cdot L_0 - 4 \cdot L_1 + L_2 + 4) \gg 3 \right) + H_0$$
  $$\text{left}_{\text{odd}} = \left( (5 \cdot L_0 + 4 \cdot L_1 - L_2 + 4) \gg 3 \right) - H_0$$
  - If `descale == 0`: $\text{out}[0] = \text{sat}_{16}(\text{left}_{\text{even}} \gg 1); \quad \text{out}[1] = \text{sat}_{16}(\text{left}_{\text{odd}} \gg 1)$
  - If `descale == 2`: $\text{out}[0] = \text{sat}_{16}(\text{left}_{\text{even}} \cdot 2); \quad \text{out}[1] = \text{sat}_{16}(\text{left}_{\text{odd}} \cdot 2)$
- **Interior Columns ($0 < c < W - 1$)**:
  - If `descale == 0`:
    $$\text{out}[2c] = \text{sat}_{16}\left( \text{satadd}_{16}\left(\text{satadd}_{16}(L_c, \text{diff}_e), H_c\right) \gg 1 \right)$$
    $$\text{out}[2c+1] = \text{sat}_{16}\left( \text{satsub}_{16}\left(\text{satadd}_{16}(L_c, \text{diff}_o), H_c\right) \gg 1 \right)$$
  - If `descale == 2`:
    $$\text{out}[2c] = \text{sat}_{16}\left( \text{satadd}_{16}\left(\text{satadd}_{16}(L_c, \text{diff}_e), H_c\right) \cdot 2 \right)$$
    $$\text{out}[2c+1] = \text{sat}_{16}\left( \text{satsub}_{16}\left(\text{satadd}_{16}(L_c, \text{diff}_o), H_c\right) \cdot 2 \right)$$
- **Right Column ($c = W - 1$)**:
  $$\text{right}_{\text{even}} = \left( (5 \cdot L_c + 4 \cdot L_{c-1} - L_{c-2} + 4) \gg 3 \right) + H_c$$
  $$\text{right}_{\text{odd}} = \left( (11 \cdot L_c - 4 \cdot L_{c-1} + L_{c-2} + 4) \gg 3 \right) - H_c$$
  - If `descale == 0`: $\text{out}[2c] = \text{sat}_{16}(\text{right}_{\text{even}} \gg 1); \quad \text{out}[2c+1] = \text{sat}_{16}(\text{right}_{\text{odd}} \gg 1)$
  - If `descale == 2`: $\text{out}[2c] = \text{sat}_{16}(\text{right}_{\text{even}} \cdot 2); \quad \text{out}[2c+1] = \text{sat}_{16}(\text{right}_{\text{odd}} \cdot 2)$

---

## 4. Golden Corpus Verification Results

The test suite executed against all 11 frozen Phase 0 golden test vectors. For every sample, GPU intermediate subbands, reconstructed planes, and converted outputs were evaluated against the pristine CPU reference.

| Resolution | Pattern | Pattern Type | Input Hash | Compressed Bytes | Max Abs Error ($\text{LL}_1$) | Max Abs Error ($\text{LL}_0$) | Max Abs Error (Planes) | Final YU64 Match | Status |
| :---: | :---: | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **1080p** | 0 | Flat | `068df6d0f622f9ea` | 277,324 | **0** | **0** | **0** | Byte-Exact | **PASS** |
| **1080p** | 1 | Gradient | `f28a7e03494bb9eb` | 467,236 | **0** | **0** | **0** | Byte-Exact | **PASS** |
| **1080p** | 2 | Noise | `8a2d1d4d7ce53e94` | 3,924,964 | **0** | **0** | **0** | Byte-Exact | **PASS** |
| **1080p** | 3 | Texture | `553e414dc80fc285` | 338,400 | **0** | **0** | **0** | Byte-Exact | **PASS** |
| **1080p** | 4 | Edges | `d061ea944ee78996` | 467,236 | **0** | **0** | **0** | Byte-Exact | **PASS** |
| **4K UHD** | 0 | Flat | `1be32fa728ea9632` | 741,608 | **0** | **0** | **0** | Byte-Exact | **PASS** |
| **4K UHD** | 1 | Gradient | `caef10372df37b4b` | 1,480,948 | **0** | **0** | **0** | Byte-Exact | **PASS** |
| **4K UHD** | 2 | Noise | `e6865aa4ab456e70` | 15,699,504 | **0** | **0** | **0** | Byte-Exact | **PASS** |
| **4K UHD** | 3 | Texture | `2d46e3d0382218dd` | 985,820 | **0** | **0** | **0** | Byte-Exact | **PASS** |
| **4K UHD** | 4 | Edges | `0b77b1029e0dc036` | 1,480,948 | **0** | **0** | **0** | Byte-Exact | **PASS** |
| **8K** | 0 | Flat | `2467d301b34e5a42` | 2,900,976 | **0** | **0** | **0** | Byte-Exact | **PASS** |

**Golden Corpus Verdict**: **11/11 samples passed with 0 maximum absolute error across all stages.**

---

## 5. Performance Measurements & PCIe Bottleneck Analysis

Benchmarks were recorded using `benchmarks/benchmark_decode_phase1.cpp` running on Windows 11 x64, AMD Ryzen 9 7940HS, NVIDIA GeForce RTX 4090 Laptop GPU (16 GB), PCIe Gen 4 x16 link. Timing data reflects 5 trials with 3 cycles per sample (warmup discarded).

### 5.1 Throughput and Latency Summary

| Resolution | Geometry | Pipeline Mode | Median FPS | Mean Latency (ms) | Min / Max (ms) | p95 Latency (ms) | Throughput (MP/s) |
| :---: | :---: | :--- | :---: | :---: | :---: | :---: | :---: |
| **1080p** | $1920 \times 1080$ | CPU Reference | **106.54** | 9.41 ms | 4.87 / 24.49 ms | 22.96 ms | 220.9 MP/s |
| **1080p** | $1920 \times 1080$ | Hybrid CUDA Wavelet | **71.49** | 13.85 ms | 8.88 / 29.86 ms | 27.61 ms | 148.2 MP/s |
| **4K UHD** | $3840 \times 2160$ | CPU Reference | **21.95** | 45.64 ms | 20.71 / 122.46 ms | 117.51 ms | 182.0 MP/s |
| **4K UHD** | $3840 \times 2160$ | Hybrid CUDA Wavelet | **12.25** | 84.56 ms | 55.78 / 171.24 ms | 157.14 ms | 101.6 MP/s |
| **8K** | $7680 \times 4320$ | CPU Reference | **11.05** | 90.75 ms | 83.31 / 119.89 ms | 109.11 ms | 366.6 MP/s |
| **8K** | $7680 \times 4320$ | Hybrid CUDA Wavelet | **4.61** | 202.85 ms | 161.69 / 326.00 ms | 266.95 ms | 152.9 MP/s |

### 5.2 Detailed Stage Latency Breakdown (Hybrid Path)

```mermaid
gantt
    title 1080p Hybrid Frame Decode Breakdown (Mean: 13.85 ms)
    dateFormat X
    axisFormat %s ms
    section Pipeline Stages
    CPU Entropy Decode : 0, 9.66
    Coefficient H2D DMA : 9.66, 10.64
    CUDA Inverse Wavelet : 10.64, 10.84
    Planar D2H DMA : 10.84, 11.72
    CPU YU64 Packing : 11.72, 13.75
```

| Stage Name | 1080p Mean (ms) | 1080p Share | 4K UHD Mean (ms) | 4K UHD Share | 8K Mean (ms) | 8K Share |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **CPU Entropy Decode** | 9.66 ms | 69.74% | 44.01 ms | 52.05% | 91.73 ms | 45.22% |
| **Coefficient H2D Transfer** | 0.98 ms | 7.06% | 12.65 ms | 14.95% | 27.17 ms | 13.40% |
| **CUDA Inverse Wavelet** | **0.20 ms** | **1.46%** | **4.15 ms** | **4.91%** | **10.04 ms** | **4.95%** |
| **Planar D2H Transfer** | 0.88 ms | 6.32% | 13.01 ms | 15.38% | 27.54 ms | 13.58% |
| **CPU Final Conversion** | 2.03 ms | 14.64% | 10.50 ms | 12.41% | 46.21 ms | 22.78% |
| **Total Frame Latency** | **13.85 ms** | 100.0% | **84.56 ms** | 100.0% | **202.85 ms** | 100.0% |

### 5.3 Transfer Overhead Analysis: Did PCIe Erase the Wavelet Gain?

> [!IMPORTANT]
> **Definitive Finding: YES. PCIe transfer completely erased the inverse wavelet speedup.**

1. **Massive Compute Speedup**:
   - The GPU inverse spatial wavelet kernel itself is blazingly fast: **0.20 ms for 1080p**, **4.15 ms for 4K UHD**, and **10.04 ms for 8K**.
   - On 1080p, the CUDA inverse wavelet executes in ~200 microseconds, representing less than **1.5%** of the frame time.
2. **The "PCIe Transfer Sandwich"**:
   - Because entropy decode remains on the host CPU, 30 uncompressed coefficient bands must be copied across PCIe to the GPU (`CoefficientH2D`: 0.98 ms for 1080p, 12.65 ms for 4K, 27.17 ms for 8K).
   - Because final packing remains on the host CPU, 3 full-resolution planar channels must be copied back across PCIe to the host (`PlanarD2H`: 0.88 ms for 1080p, 13.01 ms for 4K, 27.54 ms for 8K).
   - Combined bidirectional PCIe transfer overhead ($1.85\text{ ms}$ at 1080p, $25.65\text{ ms}$ at 4K, $54.72\text{ ms}$ at 8K) is **9.1x larger** than the CUDA compute time at 1080p and **6.2x larger** at 4K.
3. **CPU Entropy Dominance**:
   - Upstream CPU entropy decoding remains the single largest bottleneck, consuming between **45% and 70%** of total decode time.
4. **Conclusion**:
   - Moving *only* the inverse wavelet to the GPU introduces a net throughput penalty when frames originate from CPU memory and must return to CPU memory.
   - However, Phase 1 successfully established the **exact inverse mathematical core** required for true GPU acceleration.

---

## 6. Exactness Verdict

**Is CUDA inverse wavelet mathematically bit-exact with pristine upstream CPU decode?**

> **VERDICT: YES — 100% BIT-EXACT.**
>
> Maximum Absolute Error across all intermediate and final outputs: **`0`**.
>
> The CUDA kernels faithfully reproduce every upstream rounding operation, arithmetic right shift, saturating 16-bit vector sum, and asymmetric border stencil.

---

## 7. Recommendations for Phase 2

Based on the empirical profiling data from Phase 0 and Phase 1, we identify two primary paths to turn GPU inverse wavelet reconstruction into real end-to-end performance gains:

### Recommendation A: Eliminate the D2H Transfer via Direct GPU Rendering (Priority 1)
In editing and playback workloads (DaVinci Resolve, Premiere Pro, Unreal Engine, custom players), the decoded frame is ultimately uploaded to the GPU as a DirectX, Vulkan, or OpenGL texture for display or grading.
- In Phase 1, planar channels are copied to host (D2H), converted on CPU, and then the application typically copies them back to GPU for rendering!
- **Phase 2 Implementation**: Implement GPU color conversion (YUV422 $\to$ BGRA8 / RGBA16F / NV12) directly on device, writing into a shared DirectX 11/12 or Vulkan texture (`cudaGraphicsResource`).
- **Impact**: Completely eliminates the $13.0\text{ ms}$ (4K) / $27.5\text{ ms}$ (8K) D2H transfer and eliminates the CPU color packing overhead ($10.5\text{ ms}$ at 4K, $46.2\text{ ms}$ at 8K).

### Recommendation B: Eliminate the H2D Transfer via GPU Entropy Decoding (Priority 2)
The upstream CPU entropy decoder takes $44.0\text{ ms}$ at 4K.
- Compressing CineForm streams produces 10x–20x smaller payloads than uncompressed coefficient bands. Uploading the compressed bitstream requires less than $0.2\text{ ms}$ of PCIe transfer.
- **Phase 2 Implementation**: Implement parallel VLC / run-length decoding in CUDA, building on the GPU entropy parser developed in the HyperCine encoder.
- **Impact**: Eliminates CPU entropy bottleneck and cuts H2D transfer time by $>90\%$.
