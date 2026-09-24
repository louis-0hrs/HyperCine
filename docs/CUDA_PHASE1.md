# HyperCine Phase 1A: CineForm 2-6 Wavelet Transform Source Analysis & CUDA Design

**Target Component:** CineForm 2-6 Biorthogonal Wavelet Transform  
**Status:** Phase 1A Prototype Analysis & Architectural Specification  
**Upstream Commit Baseline:** `11574d0295771edccadd17af14af74a539f924c7`

---

## 1. Upstream Production Implementation Inspection

### 1.1 Source Files Targeted
The CineForm 2-6 biorthogonal wavelet transform is implemented across multiple upstream files in `upstream/Codec/` and demonstrated in `upstream/Example/WaveletDemo/`:

| Upstream Source File | Key Targeted Functions / Symbols | Purpose |
|---|---|---|
| `upstream/Codec/spatial.c` | `FilterSpatialQuant16s`, `FilterHorizontalRow16s`, `InvertVertical16s`, `InvertHorizontalRow16s` | Core production forward & inverse 2-6 wavelet transforms (generic C & SSE2 paths) |
| `upstream/Codec/spatial.h` | Prototypes for `FilterSpatialQuant16s`, `FilterHorizontalRow16s`, `InvertVertical16s`, `InvertHorizontalRow16s` | Function declarations and transform interfaces |
| `upstream/Codec/wavelet.c` | `TransformForwardSpatial`, `TransformForwardSpatialYUVPlanarThreaded`, `TransformInverseSpatial` | High-level wavelet pyramid decomposition and band allocation |
| `upstream/Codec/wavelet.h` | `TRANSFORM`, `IMAGE`, `LL_BAND`, `LH_BAND`, `HL_BAND`, `HH_BAND` | Subband identifiers, transform state, and band hierarchy definitions |
| `upstream/Codec/filter.h` | `ROUNDING(x, y)`, `DivideByShift(x, s)`, `SATURATE` | Deterministic integer rounding macros, shift-based arithmetic, and saturation rules |
| `upstream/Codec/image.h` | `PIXEL`, `SATURATE_16S`, `_SATURATE` | 16-bit signed coefficient type definition and clamping limits |
| `upstream/Example/WaveletDemo/utils.c` | `H26Wavelet`, `V26Wavelet`, `InvertH26Wavelet`, `InvertV26Wavelet` | Standalone demonstration wavelet implementation |

---

## 2. Production Call Chain & Control Flow

### 2.1 Forward Transform Call Chain (Encoding)
1. **API / SDK Entry**: `CFHD_EncodeSample` -> `encoder.c: EncodeFrame(...)`
2. **Pyramid Level Processing**: `encoder.c` invokes `wavelet.c: TransformForwardSpatial(...)` for each wavelet pyramid level (1 to 4 levels).
3. **Core Spatial Filtering**: `wavelet.c: TransformForwardSpatial(...)` prepares intermediate line buffers and calls:
   ```c
   spatial.c: FilterSpatialQuant16s(
       PIXEL *input_image, int input_pitch,
       PIXEL *lowlow_band, int lowlow_pitch,
       PIXEL *lowhigh_band, int lowhigh_pitch,
       PIXEL *highlow_band, int highlow_pitch,
       PIXEL *highhigh_band, int highhigh_pitch,
       PIXEL *buffer, size_t buffer_size,
       ROI roi, int quantization[4]
   )
   ```
4. **Horizontal Row Pass**:
   - `FilterSpatialQuant16s` calls `spatial.c: FilterHorizontalRow16s` on each pair of input rows to produce intermediate horizontal lowpass (`lowpass[6]`) and highpass (`highpass[6]`) rows.
5. **Vertical Column Pass**:
   - Vertical lowpass and highpass filters are applied across the cached horizontal intermediate rows:
     - Vertical lowpass on horizontal lowpass $\to$ **LL (Band 0)**
     - Vertical highpass on horizontal lowpass $\to$ **HL (Band 2)**
     - Vertical lowpass on horizontal highpass $\to$ **LH (Band 1)**
     - Vertical highpass on horizontal highpass $\to$ **HH (Band 3)**
6. **Quantization** (Optional in codec, unquantized in transform test vectors):
   - `QuantizeRow16sTo16s` or `QuantizeRow16sTo8s`.

### 2.2 Inverse / Synthesis Call Chain (Decoding)
1. **API / SDK Entry**: `CFHD_DecodeSample` -> `decoder.c: DecodeFrame(...)`
2. **Dequantization**: Entropy decoder dequantizes subband coefficients.
3. **Vertical Reconstruction**:
   ```c
   spatial.c: InvertVertical16s(
       PIXEL *lowpass, int lowpass_pitch,
       PIXEL *highpass, int highpass_pitch,
       PIXEL *output, int output_pitch,
       ROI roi
   )
   ```
   Reconstructs pairs of intermediate horizontal rows from vertical lowpass and highpass subbands.
4. **Horizontal Reconstruction**:
   ```c
   spatial.c: InvertHorizontalRow16s(
       PIXEL *lowpass,
       PIXEL *highpass,
       PIXEL *output,
       int width
   )
   ```
   (or strip-based pipelined variants in `InvertHorizontalStrip16s.c` combined with color conversion).

---

## 3. Discrepancy Analysis: Production Codec vs. WaveletDemo

A critical finding from source inspection is that **`WaveletDemo/utils.c` is NOT identical to the production codec**:

| Feature / Attribute | `WaveletDemo/utils.c` | Production Codec (`spatial.c`, `wavelet.c`) |
|---|---|---|
| **Data Type** | `int*` (32-bit signed integer) | `PIXEL*` (`short int` / `int16_t` signed 16-bit integer) |
| **Coefficient Buffering** | Single contiguous buffer with horizontal split: lowpass at $[0, \frac{W}{2}-1]$, highpass at $[\frac{W}{2}, W-1]$. | Dedicated planar band buffers (`lowlow_band`, `lowhigh_band`, `highlow_band`, `highhigh_band`), each with independent pitch. |
| **Pitch / Stride** | Single stride `w` for both bands. | Explicit byte/pixel pitches (`input_pitch`, `lowlow_pitch`, `highlow_pitch`, etc.). |
| **Overflow / Saturation** | No saturation during transform; unbounded 32-bit math. | Strict saturation to $[-32768, 32767]$ (`SATURATE_16S`) after every filter stage. |
| **Rounding Bias** | Explicit constant `+4` (`ROUNDING = 4`). | `ROUNDING(sum, 8) = 4` via macro in `filter.h:96`. |
| **Division Operator** | Arithmetic shift `>> 3` and `>> 1`. | `DivideByShift(sum, s)` $\to$ `(sum >> s)` via `filter.h:173`. |
| **Filter Coefficients** | Identical biorthogonal 2-6 filter taps. | Identical biorthogonal 2-6 filter taps. |

**Conclusion:** The mathematical filter taps and boundary equations are identical, but the production codec operates on **16-bit signed integers (`int16_t`) with 16-bit saturation and arbitrary pitch/stride**. HyperCine's CUDA implementation must target the production 16-bit format and semantics with bit-exact precision.

---

## 4. Exact Mathematical Formulation & Boundary Rules

### 4.1 Forward 2-6 Wavelet Transform

Given an even dimension $N \ge 6$:

#### A. Lowpass Filter (Scaling Function, 2-Tap Haar-Like)
For all even sample indices $x = 2c$, $c \in [0, \frac{N}{2}-1]$:
$$\text{sum} = \text{input}[2c] + \text{input}[2c + 1]$$
$$\text{lowpass}[c] = \text{SATURATE\_16S}(\text{sum})$$
- **DC Gain:** $\times 2$ for 1D, $\times 4$ for 2D (LL band).

#### B. Highpass Filter (Wavelet Function, 6-Tap Biorthogonal)
The highpass filter employs a 6-tap symmetric filter kernel $[-1/8, -1/8, +1, -1, +1/8, +1/8]$ with asymmetric boundary extension to ensure zero DC response without ring artifacts.

1. **Left Boundary ($c = 0$, $x = 0$):**
   $$\text{sum} = 5 \cdot \text{input}[0] - 11 \cdot \text{input}[1] + 4 \cdot \text{input}[2] + 4 \cdot \text{input}[3] - \text{input}[4] - \text{input}[5]$$
   $$\text{sum} = (\text{sum} + 4) \gg 3$$
   $$\text{highpass}[0] = \text{SATURATE\_16S}(\text{sum})$$
   *(Note: $5 - 11 + 4 + 4 - 1 - 1 = 0$, zero DC response)*

2. **Interior ($c = 1 \dots \frac{N}{2}-2$, $x = 2c$):**
   $$\text{hp\_sum} = -\text{input}[x - 2] - \text{input}[x - 1] + \text{input}[x + 2] + \text{input}[x + 3]$$
   $$\text{hp\_sum} = (\text{hp\_sum} + 4) \gg 3$$
   $$\text{sum} = \text{hp\_sum} + \text{input}[x] - \text{input}[x + 1]$$
   $$\text{highpass}[c] = \text{SATURATE\_16S}(\text{sum})$$

3. **Right Boundary ($c = \frac{N}{2}-1$, $x = N - 2$):**
   $$\text{sum} = 11 \cdot \text{input}[x] - 5 \cdot \text{input}[x + 1] - 4 \cdot \text{input}[x - 1] - 4 \cdot \text{input}[x - 2] + \text{input}[x - 3] + \text{input}[x - 4]$$
   $$\text{sum} = (\text{sum} + 4) \gg 3$$
   $$\text{highpass}[c] = \text{SATURATE\_16S}(\text{sum})$$
   *(Note: $11 - 5 - 4 - 4 + 1 + 1 = 0$, zero DC response)*

---

### 4.2 Inverse / Synthesis 2-6 Wavelet Transform

Given lowpass and highpass subbands of length $M = \frac{N}{2}$ ($M \ge 3$):
For each $c \in [0, M-1]$, the reconstruction generates even sample $x = 2c$ and odd sample $x = 2c+1$:

1. **Left Boundary ($c = 0$):**
   $$\text{even} = ((11 \cdot \text{lowpass}[0] - 4 \cdot \text{lowpass}[1] + 1 \cdot \text{lowpass}[2] + 4) \gg 3) + \text{highpass}[0]$$
   $$\text{output}[0] = \text{SATURATE\_16S}(\text{even} \gg 1)$$

   $$\text{odd} = ((5 \cdot \text{lowpass}[0] + 4 \cdot \text{lowpass}[1] - 1 \cdot \text{lowpass}[2] + 4) \gg 3) - \text{highpass}[0]$$
   $$\text{output}[1] = \text{SATURATE\_16S}(\text{odd} \gg 1)$$

2. **Interior ($c = 1 \dots M - 2$):**
   $$\text{even} = ((\text{lowpass}[c-1] - \text{lowpass}[c+1] + 4) \gg 3) + \text{lowpass}[c] + \text{highpass}[c]$$
   $$\text{output}[2c] = \text{SATURATE\_16S}(\text{even} \gg 1)$$

   $$\text{odd} = ((-\text{lowpass}[c-1] + \text{lowpass}[c+1] + 4) \gg 3) + \text{lowpass}[c] - \text{highpass}[c]$$
   $$\text{output}[2c+1] = \text{SATURATE\_16S}(\text{odd} \gg 1)$$

3. **Right Boundary ($c = M - 1$):**
   $$\text{even} = ((5 \cdot \text{lowpass}[c] + 4 \cdot \text{lowpass}[c-1] - 1 \cdot \text{lowpass}[c-2] + 4) \gg 3) + \text{highpass}[c]$$
   $$\text{output}[2c] = \text{SATURATE\_16S}(\text{even} \gg 1)$$

   $$\text{odd} = ((11 \cdot \text{lowpass}[c] - 4 \cdot \text{lowpass}[c-1] + 1 \cdot \text{lowpass}[c-2] + 4) \gg 3) - \text{highpass}[c]$$
   $$\text{output}[2c+1] = \text{SATURATE\_16S}(\text{odd} \gg 1)$$

---

## 5. Integer Arithmetic, Rounding, and Saturation Safety

1. **Arithmetic Right Shift on Signed Integers:**
   - Both forward and inverse transforms rely on arithmetic right shift (`>> 3` and `>> 1`).
   - In ISO C++17, right-shifting a signed integer is implementation-defined; on x86/x64 MSVC, GCC, Clang, and NVIDIA PTX/CUDA, `>>` on signed types (`int16_t`, `int32_t`) produces arithmetic shift (sign extension).
2. **Intermediate Bit Depth:**
   - Although inputs and outputs are `int16_t`, intermediate sums in the 6-tap filter can exceed 16 bits (e.g. $11 \times 32767 = 360437$, requiring up to 20 bits of dynamic range).
   - In CUDA and CPU C++, all intermediate accumulation must occur in `int32_t` before shifting and saturating to `int16_t`.
3. **Saturation:**
   - Clamping rule:
     ```cpp
     inline __device__ __host__ int16_t Saturate16S(int32_t val) {
         if (val > 32767) return 32767;
         if (val < -32768) return -32768;
         return static_cast<int16_t>(val);
     }
     ```
   - On CUDA, this compiles efficiently to `__saturate` or `__min(__max(val, -32768), 32767)` or PTX `min.s32` / `max.s32`.

---

## 6. Stride & Pitch Handling Requirements

1. **Line Stride (Pitch):**
   - The CineForm SDK guarantees that frame buffers have strides padded to multiples of 16 bytes (8 `int16_t` pixels).
   - The CUDA implementation must take pitch in elements (`stride_elements`) or pitch in bytes (`pitch_bytes`) as an explicit parameter for every buffer (input, lowpass, highpass, intermediate).
2. **Minimum Dimensions:**
   - CineForm requires even dimensions, with minimum $W \ge 6$ and $H \ge 6$ (due to the 6-tap boundary filter).
   - Typical subband dimensions are multiples of 2, 4, 8, 16, or 32.

---

## 7. HyperCine Architecture for Phase 1A

- `include/hypercine/wavelet.h`: Public C/C++ API for CPU and GPU wavelet transforms.
- `src/cpu/wavelet_cpu.cpp`: Deterministic CPU reference conforming bit-exact to upstream `spatial.c`.
- `src/cuda/wavelet_cuda_reference.cu`: Transparent reference CUDA kernels (`CUDA_REFERENCE`).
- `src/cuda/wavelet_cuda_optimized.cu`: Shared-memory optimized CUDA kernels (`CUDA_OPTIMIZED`).
- `tests/test_cuda_wavelet.cpp`: Transform-level bit-exact test suite.
- `benchmarks/benchmark_wavelet.cpp`: Comprehensive 3-measurement benchmark harness.
