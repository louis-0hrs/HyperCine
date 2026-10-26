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
- **Status**: Nativ getestet (CPU scalar, CPU NEON, Metal)
- **Platform**: Linux x86_64 (CPU), macOS arm64 M2 Pro (NEON, Metal). **Nicht** auf der A2 qualifiziert.
- **Anchor**: `src/cpu/filmscan3_frontend.cpp::ConvertFilmScan3Bgra()`, `src/core/filmscan3_encoder.cpp` (frontend.Transform),
  `tests/test_filmscan3.cpp`, `tests/test_metal_filmscan3_4k.cpp`
- **Evidence**: Synthetic BGRA encoding validated at 3840x2160 with CPU/Metal packet parity and independent FFmpeg
  decode (#248, #251). NEON parity extended to 4K with 1,457,338 checks (#255).
- **Correction**: an earlier version of this row cited `CAPABILITY_8K_17K_AUDIT.md` as an A2 platform proof.
  That document states of itself: "Source/measurement audit, not a 8K/12K/17K codec qualification." It is not a
  platform qualification for this layer and was misused as one.

### 4. Wavelet-Compute
Performs the 2-6 biorthogonal wavelet decomposition and reconstruction.
- **Status**: Nativ getestet (CPU scalar, CPU NEON, Metal); CUDA-Frontend experimentell
- **Platform**: Linux x86_64 (CPU, CUDA-A2), macOS arm64 M2 Pro (NEON, Metal)
- **Anchor**: `src/core/filmscan3_encoder.cpp` (WaveletLevel processing), `src/cpu/filmscan3_frontend.cpp` (ForwardLevel),
  `tests/test_cuda_wavelet.cpp`, `tests/test_transform_wavelet.cpp`, `tests/test_neon_wavelet.cpp`
- **Evidence**: CPU/CUDA coefficient parity and full packet equality at 720p/1080p/2048x1080/4K (#249);
  NEON kernel 3.1-3.4x scalar with bit-exact parity to 4K (#255); Metal 4K CPU parity, 68/68 native (#251).
- **Grenze**: the CUDA path is an experimental SDK-free frontend, not a qualified backend. #249 evidence was
  produced before the merge; a post-merge A2 rerun is still outstanding.

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
- **Anchor**: `src/core/phase3a_encoder.cpp` (CUDA capability checks), `docs/CAPABILITY_8K_17K_AUDIT.md` (capability audit, **not** a GPU qualification), `src/cuda/filmscan3_cuda_frontend.cpp`
- **Evidence**: CUDA backend queries device properties but lacks formal capability negotiation framework

### 10. Decoder-Scheduling
Manages decode task dependencies and parallelism.
- **Status**: Synthetisch implementiert und nativ getestet; fuer echte Decoder **nicht** qualifiziert
- **Anchor**: `src/core/decode_task_graph.{h,cpp}` (Generation, TaskKey, publish/admit, fail-closed),
  `src/core/decode_s3_plane.h` + `src/core/decode_s3_consumer.cpp` (typisierte Signed-16-Plane, Extent, Generation),
  `tests/decode_task_graph/test_scheduler_t1_t2_144.cpp`, `tests/decode_task_graph/test_scheduler_s2_output_144.cpp`,
  `tests/decode_task_graph/test_decode_s3_plane.cpp`
- **Evidence**: Merged on main: `eddad41` (topology + versioned publication), `9d290ab` (generation-owned output,
  fail-closed nonblocking admission), `1e22aea` (bounded synthetic signed-plane S3, `holds=10 failures=0`).
  T4 (four-band plane gated on generation/identity/budget) is in draft PR #265, ASan and TSan 21/21.
- **Nicht belegt**: no parsing of real CFHD streams, no entropy decode, no validated inverse transform,
  no GoPro integration. The original SDK races from #195 are **not** fixed and stay open.

> This line previously read "Nicht implementiert". That was wrong: the scheduler, its S2 retention
> contract and the S3 signed-plane attachment are merged and tested on `main`. The correct statement is
> that the *synthetic* scheduler exists and is tested, while a real decoder still does not.

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
| 3. Pixelkonvertierung | Nativ getestet (CPU scalar/NEON, Metal) | test_filmscan3.cpp, test_metal_filmscan3_4k.cpp, ConvertFilmScan3Bgra |
| 4. Wavelet-Compute | Nativ getestet (CPU scalar/NEON, Metal); CUDA experimentell | test_cuda_wavelet.cpp, test_neon_wavelet.cpp, filmscan3_encoder.cpp |
| 5. Quantisierung | Implementiert | quality_state.cpp, QuantizeFilmScan3 |
| 6. Entropiekodierung | Implementiert | entropy_fast.h, PackHighpass |
| 7. Backend-Auswahl | Experimentell | MACOS_METAL_BACKEND.md, FilmScan3Encoder ctors |
| 8. Speicherverwaltung | Implementiert | standalone.h (AlignedAllocator), test_vectors.h |
| 9. Hardware-Capabilities | Experimentell | CAPABILITY_8K_17K_AUDIT.md, phase3a_encoder.cpp |
| 10. Decoder-Scheduling | Synthetisch implementiert + getestet; echte Decoder nicht qualifiziert | decode_task_graph.{h,cpp}, decode_s3_plane.h, test_scheduler_s2_output_144.cpp, test_decode_s3_plane.cpp |

## Verification Notes
All status claims are anchored to specific files, tests, or documents. No speculative statements are included. Where evidence was unavailable, status reflects the highest verifiable implementation level.