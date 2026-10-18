# HyperCine Architecture

HyperCine contains an upstream-compatible **CUDA encoder**, an independent **SDK-free CPU encoder** with an optional experimental Apple **Metal frontend**, and separate decoder research. These paths are not yet one unified cross-platform codec runtime. The frozen GoPro SDK remains an unchanged reference, not an implementation to modify.

This document distinguishes the general CineForm algorithm from features actually implemented and validated in HyperCine. For tested formats and host limitations, read [Compatibility](COMPATIBILITY.md).

## Implementation status and ownership

| Component | Current boundary | Evidence |
| :--- | :--- | :--- |
| Phase 3A.2c CUDA encoder | Windows / NVIDIA; BGRA8 → CFHD YUV422 FilmScan 3, GOP 1 | [Canonical reference record](../benchmarks/phase3a2c_release_candidate.json) |
| Frozen GoPro SDK | CPU encode/decode reference with known decoder reproducibility concerns | [Upstream policy and investigation](UPSTREAM.md) |
| SDK-free CPU encoder | Standalone fixed FS3-Q2 and stateful FilmScan 3 paths; separately scoped tests | [Standalone](MACOS_STANDALONE_ENCODER.md) · [FilmScan 3](MACOS_FILMSCAN3_CPU.md) |
| Experimental Metal frontend | Color and forward-wavelet work for SDK-free FilmScan 3; host quantization and packaging remain | [Metal implementation report](MACOS_METAL_BACKEND.md) |
| Portable decode scheduling | Synthetic dependency graph; **not yet a CFHD decoder** | [Task graph](DECODE_TASK_GRAPH_147.md) |
| Vulkan / complete GPU decode | Not a qualified mainline release feature | [Compatibility](COMPATIBILITY.md) |

**Key distinction:** The encoder's bitstream-equivalence benchmark and a decoder's pixel/release-safety gate answer different questions. An identical compressed sample can still trigger a reference-decoder failure; see [#120](https://github.com/louis-0hrs/HyperCine/issues/120).

## 1. CineForm algorithm (conceptual)

CineForm is an intra-frame wavelet codec used for intermediates and high-fidelity media workflows. The following diagrams explain the **format's general algorithm**, not a claim that each listed stage or input format is accelerated on every backend.

---

### General encoding pipeline

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

### General decoding pipeline (not an implemented GPU decoder)

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

## 2. Mathematical background: the 2-6 biorthogonal wavelet

CineForm uses a **2-6 biorthogonal wavelet** with integer filtering and specific edge handling. The simplified expressions below are an orientation aid, not a replacement for the SDK's exact rounding, saturation, prescale and boundary contracts. Production bit-exactness must be checked against the relevant reference vectors.

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

### HyperCine's implemented encoder execution models

**CUDA Phase 3A.2c (`src/core/phase3a_encoder.cpp`, `src/core/phase2d_encoder.cpp`):** The host handles the reference encoder state, pageable/pinned staging where applicable, scheduling, and final CFHD sample assembly. CUDA kernels perform color conversion, forward 2-6 wavelet decomposition with per-frame quantization, and highpass entropy encoding. Frontend work for frame `k+1` can overlap later stages for frame `k` across frame slots. The entropy stage uses a cached CUDA Graph; rate-control updates occur outside the captured entropy graph.

**Separate SDK-free standalone (`src/cpu/`, `src/core/standalone_encoder.cpp`, `src/core/filmscan3_encoder.cpp`):** This encoder is independent of GoPro SDK linkage. Its scalar CPU routines perform format-specific conversion and wavelet processing. Optional `HYPERCINE_ENABLE_METAL=ON` adds the experimental macOS FilmScan 3 GPU frontend; quantization, stateful rate control, VLC/packet writing and metadata remain host-side. Its validation domain is narrower than the CUDA throughput matrix.

**Decoder research:** The reference decoder uses the original GoPro SDK. A separate portable decode DAG models producer/consumer dependencies but does **not** yet parse, entropy-decode, reconstruct and output arbitrary CFHD frames. Draft decoder work should not be presented as a shipping backend. See [decoder scheduler design](WAVELET_DEPENDENCY_SCHEDULING_144.md) and [task graph](DECODE_TASK_GRAPH_147.md).

### Concurrency and safety distinction

The GoPro SDK decoder has reproducible success-status / pixel-output divergence on certain hosts and worker topologies. The ongoing [Windows](https://github.com/louis-0hrs/HyperCine/issues/120) and [Linux](https://github.com/louis-0hrs/HyperCine/issues/159) campaigns investigate this independently of GPU encoder parity. Limiting workers may change whether the bug manifests but is not a causal fix. Code-level explanations remain hypotheses unless reproduced by controlled diagnostics.


---

## 5. SIMD and Memory Alignment Requirements

1. **16-Byte Alignment for SSE2:**
   The upstream CPU implementation contains hand-tuned x86-64 SSE2 intrinsics (`_mm_load_si128`, `_mm_store_si128`, `_mm_adds_epi16`, `_mm_subs_epi16`). If image rows or temporal coefficient buffers are not aligned to 16-byte boundaries (e.g. standard `malloc` or unaligned `std::vector`), instructions fault or trigger internal fallback exceptions.
2. **Buffer Strides (Pitch):**
   All image pitches must be padded to multiples of 16 or 32 bytes to ensure aligned vector loads across every scanline.
3. **HyperCine Aligned Allocators:**
   HyperCine introduces `AlignedAllocator<T, 16>` and `AlignedVector<T>` in `tests/test_vectors.h` and core wrappers to guarantee cross-platform memory alignment safety.
