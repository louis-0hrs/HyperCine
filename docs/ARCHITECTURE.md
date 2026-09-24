# HyperCine Architecture

HyperCine is a modern, high-performance, GPU-accelerated implementation of the GoPro CineForm codec. The goal of HyperCine is to provide ultra-fast CineForm encoding and decoding (targeting multi-stream real-time 4K and 8K playback and ingest) while maintaining 100% bitstream compatibility with the SMPTE VC-5 standard and the official GoPro CineForm SDK.

This document outlines the architectural pipeline of the CineForm codec, the structure of the upstream reference implementation, the threading model, memory access patterns, and the integration strategy for HyperCine.

---

## 1. CineForm Codec Pipeline Overview

CineForm is an intra-frame wavelet video codec designed specifically for digital intermediate (DI) editing, acquisition, and high-fidelity video mastering. Unlike DCT-based codecs (such as ProRes, DNxHD, or JPEG), CineForm employs subband biorthogonal wavelet transforms across the entire image (or large slices), avoiding macroblocking artifacts at high compression ratios.

### High-Level Encoding Pipeline

```
  [Input Video Frame] (RGBA, BGRA, YUY2, RGB48, Bayer RAW)
           │
           ▼
  ┌─────────────────────────┐
  │ Color Space Conversion  │  Convert to internal working format
  │ (RGB -> YUV, Pedestal)  │  (YUV 4:2:2, YUV 4:4:4, or Bayer subbands)
  └─────────────┬───────────┘
                │
                ▼
  ┌─────────────────────────┐
  │ Multi-Level Wavelet     │  Hierarchical 2-6 Biorthogonal Wavelet Transform
  │ Decomposition           │  Separable Horizontal & Vertical filtering
  │ (Spatial Subbands)      │  Yields LL, LH, HL, HH bands across 2-4 levels
  └─────────────┬───────────┘
                │
                ▼
  ┌─────────────────────────┐
  │ Dead-Zone Uniform       │  Scalar quantization per subband
  │ Quantization            │  Scaling factors determined by quality level & bitrate
  └─────────────┬───────────┘
                │
                ▼
  ┌─────────────────────────┐
  │ Run-Length & VLC        │  Finite State Machine (FSM) / Huffman variable-length
  │ Entropy Coding          │  entropy coding of zero runs and quantized coefficients
  └─────────────┬───────────┘
                │
                ▼
  ┌─────────────────────────┐
  │ Bitstream Container &   │  SMPTE VC-5 / CFHD bitstream framing,
  │ Metadata Packaging      │  subband indices, slice headers, color tags
  └─────────────┬───────────┘
                │
                ▼
  [Output CineForm Bitstream]
```

### High-Level Decoding Pipeline

```
  [Input CineForm Bitstream]
           │
           ▼
  ┌─────────────────────────┐
  │ Bitstream Unpacking &   │  Parse container chunks, extract metadata,
  │ Header Parsing          │  locate subband slice bitstreams
  └─────────────┬───────────┘
                │
                ▼
  ┌─────────────────────────┐
  │ VLC Entropy Decoding    │  Variable-length decoding to reconstruct
  │ & Run-Length Expansion  │  quantized coefficient matrices
  └─────────────┬───────────┘
                │
                ▼
  ┌─────────────────────────┐
  │ Inverse Quantization    │  Subband coefficient de-quantization
  │ (Dequant)               │  with center-deadband reconstruction
  └─────────────┬───────────┘
                │
                ▼
  ┌─────────────────────────┐
  │ Multi-Level Inverse     │  Hierarchical 2-6 Biorthogonal Wavelet Synthesis
  │ Wavelet Reconstruction  │  Separable Vertical & Horizontal synthesis
  │ (Spatial Synthesis)     │  Reconstructs lowpass planes from subbands
  └─────────────┬───────────┘
                │
                ▼
  ┌─────────────────────────┐
  │ Color Space Conversion  │  Convert from internal YUV/Bayer to target display
  │ (YUV -> RGB, Gamma)     │  format (BGRA, RGBA, YUY2, RGB48)
  └─────────────┬───────────┘
                │
                ▼
  [Output Decoded Frame]
```

---

## 2. Mathematical Foundations: The 2-6 Biorthogonal Wavelet

The core mathematical engine of CineForm is the **2-6 Biorthogonal Wavelet Filter Pair**. This filter pair operates with symmetric FIR filters that possess exact linear phase and compact support.

### Forward Decomposition Filter Coefficients
For an input sequence $x[n]$:
1. **Lowpass Filter (2-tap averaging):**
   $$L[n] = \left\lfloor \frac{x[2n] + x[2n+1]}{2} \right\rfloor$$
2. **Highpass Filter (6-tap analysis):**
   $$H[n] = x[2n+1] - x[2n] + \left\lfloor \frac{-x[2n-2] - x[2n-1] + x[2n+2] + x[2n+3] + 8}{16} \right\rfloor$$
   In the upstream implementation (`spatial.c`, `H26Wavelet`), this is structured symmetrically with coefficients $[-1, 9, 9, -1]$ over difference taps.

### Inverse Synthesis Filter Coefficients
To reconstruct $x[2n]$ and $x[2n+1]$ from lowpass $L$ and highpass $H$:
1. The lowpass signal provides the coarse average.
2. The highpass signal provides detail adjustments filtered across neighboring taps.
3. The reconstruction produces exact integer recovery in lossless mode and minimum mean squared error (MMSE) in lossy quantization modes.

### Separable 2D Transform
The 2D transform is strictly separable:
1. Horizontal pass: Every row is filtered into $L$ (left half) and $H$ (right half).
2. Vertical pass: Every column of the intermediate result is filtered into $LL$ (top-left), $HL$ (bottom-left), $LH$ (top-right), and $HH$ (bottom-right).
3. Hierarchical decomposition: The $LL$ subband is recursively decomposed for $N$ levels (typically 2 to 4 levels depending on frame resolution).

---

## 3. Upstream Codebase Organization

The upstream reference code (`upstream/`) is organized as follows:

| Directory | Responsibilities | Key Source Files |
|-----------|------------------|------------------|
| `upstream/Codec/` | Core encoder and decoder pipelines, frame state, entropy coding, bitstream packaging | `encoder.c`, `decoder.c`, `frame.c`, `bitstream.c`, `entropy.c`, `quantize.c`, `spatial.c`, `wavelet.c` |
| `upstream/Common/` | Cross-platform utilities, thread abstractions, locks, dynamic array allocators, metadata parsing | `thread.c`, `lock.c`, `config.h`, `mem.h`, `metadata.c` |
| `upstream/Convert/` | Color space conversion kernels (RGB to YUV, YUV to RGB, Bayer demosaic, pedestal adjustments) | `RGB2YUV.c`, `YUV2RGB.c`, `convert.c`, `bayer.c` |
| `upstream/Wavelet/` | Historical wavelet routines, demo harnesses, standalone transform tests | `wavelet.c`, `subband.c` |
| `upstream/Example/` | Command-line test harness (`TestCFHD`), demo apps, sample AVI test bitstream | `TestCFHD/TestCFHD.cpp`, `videos/CFHDsample422.AVI` |

---

## 4. Concurrency and Threading Model

### Upstream CPU Model
- **Thread Pools:** Uses native platform worker threads (Win32 worker threads on Windows via `Common/thread.c`).
- **Slice / Band Parallelism:** Video frames are vertically partitioned into horizontal bands or slices. Each worker thread processes a set of slices independently through the wavelet decomposition and quantization stages.
- **Entropy Coding Serialization:** Entropy coding of subband bitstreams is performed per-slice or per-subband, then concatenated into the master bitstream sequentially.

### HyperCine Target Execution Model
HyperCine separates the codec pipeline into:
1. **Host-bound stages (CPU):**
   - Bitstream demuxing / syntax parsing.
   - Run-length & Huffman / VLC entropy decoding (or fused GPU entropy decode for architectures supporting high-throughput prefix-sum scans).
2. **Device-bound stages (GPU / CUDA / Vulkan):**
   - Color space conversion and matrix transforms.
   - Multi-level forward and inverse 2-6 wavelet transforms.
   - Quantization and de-quantization.
   - Pinned memory asynchronous transfers via double/triple buffered streams.

---

## 5. SIMD and Memory Alignment Requirements

1. **16-Byte Alignment for SSE2:**
   The upstream CPU implementation contains hand-tuned x86-64 SSE2 intrinsics (`_mm_load_si128`, `_mm_store_si128`, `_mm_adds_epi16`, `_mm_subs_epi16`). If image rows or temporal coefficient buffers are not aligned to 16-byte boundaries (e.g. standard `malloc` or unaligned `std::vector`), instructions fault or trigger internal fallback exceptions.
2. **Buffer Strides (Pitch):**
   All image pitches must be padded to multiples of 16 or 32 bytes to ensure aligned vector loads across every scanline.
3. **HyperCine Aligned Allocators:**
   HyperCine introduces `AlignedAllocator<T, 16>` and `AlignedVector<T>` in `tests/test_vectors.h` and core wrappers to guarantee cross-platform memory alignment safety.
