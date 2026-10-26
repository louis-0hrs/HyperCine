# HyperCine Architecture Boundary Map

This document maps the boundaries between architectural layers in HyperCine to prevent developers from building separate complete codec systems for each resolution and GPU. It documents the current state of each layer with verifiable anchors.

## Layer Status Definitions
- **Implementiert**: Basic implementation exists
- **Nativ getestet**: Tested on specific platform with evidence (test name/PR/issue)
- **Unabhängig referenzqualifiziert**: Qualified against independent reference (GoPro SDK/FFmpeg)
- **Experimentell**: Implemented but not fully validated
- **Nicht implementiert**: No implementation
- **Bekannt blockiert**: Known blockage with reason

## Layer Boundaries

### 1. Codec-/Bitstream-Core
Handles bitstream structure, chunk headers, and overall CineForm container format.
- **Status**: Implementiert
- **Anchor**: `src/core/standalone_encoder.cpp` (Writer/Sample classes), `src/core/filmscan3_encoder.cpp` (Writer class), `tests/test_standalone.cpp`
- **Evidence**: Bitstream writing functionality for both standalone and FilmScan3 encoders

### 2. Profil- und Qualitaetszustand (Profile and Quality State)
Manages encoding profiles, quality settings, and rate limiting state.
- **Status**: Implementiert
- **Anchor**: `src/core/quality_state.cpp`, `src/core/filmscan3_encoder.cpp` (state management), `tests/test_quality_state.cpp`
- **Evidence**: Quality state machine with limiter calculations and table lookups

### 3. Pixelkonvertierung (Pixel Conversion)
Converts input pixel formats (BGRA, etc.) to internal working color space (YUV).
- **Status**: Nativ getestet
- **Platform**: Linux x86_64 with NVIDIA A2 (as per CAPABILITY_8K_17K_AUDIT.md)
- **Anchor**: `src/cpu/filmscan3_frontend.cpp::ConvertFilmScan3Bgra()`, `src/core/filmscan3_encoder.cpp` (frontend.Transform), `docs/CAPABILITY_8K_17K_AUDIT.md`
- **Evidence**: Synthetic BGRA encoding validated for 3840x2160 (UHD 4K) with CPU/Metal packet parity and independent FFmpeg decode (#248)

### 4. Wavelet-Compute
Performs the 2-6 biorthogonal wavelet decomposition and reconstruction.
- **Status**: Nativ getestet
- **Platform**: Linux x86_64 with NVIDIA A2 (as per CAPABILITY_8K_17K_AUDIT.md)
- **Anchor**: `src/core/filmscan3_encoder.cpp` (WaveletLevel processing), `src/cpu/filmscan3_frontend.cpp` (ForwardLevel), `tests/test_cuda_wavelet.cpp`, `tests/test_transform_wavelet.cpp`
- **Evidence**: Wavelet transforms tested in CUDA and CPU implementations with reference comparisons

### 5. Quantisierung (Quantization)
Applies dead-zone uniform quantization to wavelet coefficients.
- **Status**: Implementiert
- **Anchor**: `src/core/filmscan3_encoder.cpp` (QuantizeFilmScan3 calls), `src/core/quality_state.cpp` (quant[] table calculation), `tests/test_quality_state.cpp`
- **Evidence**: Quantization tables computed per-frame based on limiter state and quality settings

### 6. Entropiekodierung (Entropy Coding)
Encodes quantized coefficients using run-length and VLC entropy coding.
- **Status**: Implementiert
- **Anchor**: `src/core/entropy_fast.h` (EntropyBits class), `src/core/filmscan3_encoder.cpp` (PackHighpass usage), `tests/test_entropy_bitexact.cpp`, `tests/test_entropy_capacity.cpp`
- **Evidence**: Entropy coding implementation with bit-exact testing against upstream reference

### 7. Backend-Auswahl (Backend Selection)
Selects between CPU, Metal, CUDA backends for computation.
- **Status**: Experimentell
- **Anchor**: `src/core/filmscan3_encoder.cpp` (FilmScan3Encoder constructors), `detail::SelectFrontend()` in filmscan3_encoder.cpp, `docs/MACOS_METAL_BACKEND.md`
- **Evidence**: Backend selection mechanism exists but Metal backend is marked experimental (see MACOS_METAL_BACKEND.md)

### 8. Speicherverwaltung (Memory Management)
Handles aligned allocations, buffer lifetimes, and memory budgets.
- **Status**: Implementiert
- **Anchor**: `include/hypercine/standalone.h` (AlignedAllocator, AlignedVector), `src/core/standalone_encoder.cpp` (Sample class with kCapacity), `tests/test_vectors.h`
- **Evidence**: Custom aligned allocators guarantee 32-byte alignment for SIMD safety

### 9. Hardware-Capabilities
Queries and adapts to hardware capabilities (GPU compute, memory limits, etc.).
- **Status**: Experimentell
- **Anchor**: `src/core/phase3a_encoder.cpp` (CUDA capability checks), `docs/CAPABILITY_8K_17K_AUDIT.md` (A2 GPU validation), `src/cuda/filmscan3_cuda_frontend.cpp`
- **Evidence**: CUDA backend queries device properties but lacks formal capability negotiation framework

### 10. Decoder-Scheduling
Manages decode task dependencies and parallelism (research phase).
- **Status**: Nicht implementiert (for GPU decode)
- **Anchor**: `docs/DECODE_TASK_GRAPH_147.md`, `docs/WAVELET_DEPENDENCY_SCHEDULING_144.md`, `tests/test_decode_task_graph.cpp`
- **Evidence**: Portable decode DAG exists but does not yet parse/entropy-decode/reconstruct arbitrary CFHD frames

## Known Boundary Issues

### Geometry Constraint Mixing (Blocked)
The geometry constraint layer suffers from mixed concerns:
- `kMaxDimension = 4096` in `include/hypercine/standalone.h` serves dual purpose as both format limit and memory budget
- `kMaxPlaneElements = 3840*2160` excludes 4096x2160 despite being format-legal
- **Pure format rule**: Breite%128==0, Hoehe%8==0, Breite>=128, Hoehe>=24 (per `WaveletShape` in `src/core/encoder_capability.cpp` and `ValidateFilmScan3Geometry` in `src/cpu/filmscan3_frontend.cpp`)
- **Status**: Bekannt blockiert
- **Anchor**: `include/hypercine/standalone.h` (lines defining kMaxDimension/kMaxPlaneElements), `src/cpu/filmscan3_frontend.cpp::ValidateFilmScan3Geometry()`, `docs/CAPABILITY_8K_17K_AUDIT.md`
- **Blocking reason**: Couples format constraints with memory budgets preventing independent evolution
- **Ongoing work**: Branch `codex/issue259-geometry-budget-separation-20261026` seeks to separate these concerns

### Illegal Format Notification
Per the pure format rule (Breite%128==0, Hoehe%8==0, Breite>=128, Hoehe>=24):
- **17520x8040 is ILLEGAL** because 17520 % 128 = 112 (not 0)
- Next legal widths: 17408 (17520-112) or 17536 (17520+16)
- **Status**: Dokumentierte Grenze
- **Anchor**: `src/cpu/filmscan3_frontend.cpp::ValidateFilmScan3Geometry()` (width%128 check), `docs/CAPABILITY_8K_17K_AUDIT.md` (URSA 17K width discussion)

## Layer Status Summary

| Layer | Status | Evidence Anchor |
|-------|--------|-----------------|
| 1. Codec-/Bitstream-Core | Implementiert | standalone_encoder.cpp, filmscan3_encoder.cpp |
| 2. Profil- und Qualitaetszustand | Implementiert | quality_state.cpp |
| 3. Pixelkonvertierung | Nativ getestet (Linux/NVIDIA A2) | CAPABILITY_8K_17K_AUDIT.md, ConvertFilmScan3Bgra |
| 4. Wavelet-Compute | Nativ getestet (Linux/NVIDIA A2) | test_cuda_wavelet.cpp, ForwardLevel |
| 5. Quantisierung | Implementiert | quality_state.cpp, QuantizeFilmScan3 |
| 6. Entropiekodierung | Implementiert | entropy_fast.h, PackHighpass |
| 7. Backend-Auswahl | Experimentell | MACOS_METAL_BACKEND.md, FilmScan3Encoder ctors |
| 8. Speicherverwaltung | Implementiert | standalone.h (AlignedAllocator), test_vectors.h |
| 9. Hardware-Capabilities | Experimentell | CAPABILITY_8K_17K_AUDIT.md, phase3a_encoder.cpp |
| 10. Decoder-Scheduling | Nicht implementiert | DECODE_TASK_GRAPH_147.md, WAVELET_DEPENDENCY_SCHEDULING_144.md |

## Verification Notes
All status claims are anchored to specific files, tests, or documents. No speculative statements are included. Where evidence was unavailable, status reflects the highest verifiable implementation level.