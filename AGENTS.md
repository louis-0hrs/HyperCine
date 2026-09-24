# AGENTS.md — HyperCine AI Agent & Contributor Guidelines

This document specifies mandatory rules, design constraints, and operational protocols for any AI agent or software engineer contributing to the **HyperCine** repository.

Every agent operating in this repository must strictly adhere to these 11 rules.

---

## 1. Upstream Immutability (Absolute Rule)
- The entire contents of the `upstream/` directory are **frozen reference code**.
- **NEVER** edit, refactor, reformat, modernize, or patch any file inside `upstream/`.
- Do not fix compiler warnings, change CRT calls, or modernize CMake inside `upstream/`. Suppress warnings or configure flags in root `CMakeLists.txt` or wrapper layers instead.
- If upstream behavior has a quirk or bug, document it in `docs/UPSTREAM.md` and handle or bypass it in the `hypercine/` wrapper layer.
- Upstream baseline commit: `11574d0295771edccadd17af14af74a539f924c7`.

---

## 2. Dual Licensing and Copyright Preservation
- GoPro's original code is dual-licensed under **MIT** or **Apache-2.0** (`Copyright (c) 2017 GoPro, Inc.`).
- Preserve all existing license files and copyright headers untouched in `upstream/`.
- All new files created for HyperCine must also be dual-licensed under MIT OR Apache-2.0.
- Standard file header for new HyperCine source files:
  ```cpp
  // Copyright (c) 2026 HyperCine Contributors.
  // Dual-licensed under the MIT License or Apache License, Version 2.0.
  // See LICENSE-MIT and LICENSE-APACHE for details.
  ```

---

## 3. Zero External Dependencies for Core Codec
- The core HyperCine codec libraries (`include/hypercine/`, `src/`) must have **zero external third-party dependencies**.
- Do not introduce Boost, Abseil, OpenCV, Conan, vcpkg, or GoogleTest for core functionality.
- Rely strictly on:
  - ISO C++17 standard library.
  - Platform-native GPU SDKs when enabled (NVIDIA CUDA Toolkit, Khronos Vulkan SDK, Apple Metal).
  - CMake 3.25+ and CTest for build and test harnesses.

---

## 4. Strict Bitstream Compatibility
- Any bitstream emitted by HyperCine must conform to the SMPTE VC-5 / CineForm `CFHD` standard.
- Any bitstream produced by HyperCine must decode correctly in:
  - The pristine upstream GoPro CineForm SDK (`upstream/Example/TestCFHD`).
  - Adobe Premiere Pro / After Effects.
  - DaVinci Resolve.
  - FFmpeg (`cfhd` decoder).
- Any valid bitstream produced by upstream tools must decode correctly in HyperCine.

---

## 5. Test-Driven Verification (TDD)
- Never merge or commit changes without running the full test suite via CTest:
  ```powershell
  ctest --test-dir build -C Release --output-on-failure
  ```
- Any new algorithm, color conversion kernel, or GPU acceleration path must have a corresponding test vector in `tests/`.
- Lossy compression tests must verify PSNR thresholds ($> 35\text{ dB}$ for FilmScan 3); lossless modes must verify mathematical bit-exactness.

---

## 6. Deterministic Execution
- All test patterns, benchmarks, and mathematical verifications must be deterministic.
- Use explicit seed values (such as `0x1337BEEF`) for test generators; do not use non-deterministic sources like `std::random_device` without a reproducible seed.
- Wavelet transforms and color conversions must produce consistent, reproducible output across runs.

---

## 7. SIMD & Memory Alignment Safety
- The CineForm codec relies on 16-byte (and 32-byte) aligned vector memory for SSE2, AVX2, and GPU transfers.
- Never pass unaligned standard heap allocations to codec buffers.
- Use `AlignedAllocator<T, 16>` / `AlignedVector<T>` (defined in `tests/test_vectors.h`) or platform-specific aligned allocators (`_aligned_malloc` / `posix_memalign`).
- Ensure line pitch / stride is padded to a multiple of 16 bytes.

---

## 8. Modular Multi-Backend Architecture
- Maintain strict separation of concerns across directories:
  - `include/hypercine/`: Public C and C++ API headers.
  - `src/core/`: Common frame handling, format conversion, and bitstream orchestration.
  - `src/cpu/`: CPU-based fallback and reference wrappers.
  - `src/cuda/`: NVIDIA CUDA acceleration backend (Phase 1).
  - `src/vulkan/`: Cross-vendor Vulkan compute backend (Phase 2).
  - `src/metal/`: Apple Metal compute backend (Phase 3).
  - `tools/`: Command-line utilities (`hypercine_transcode`, `hypercine_info`).
  - `tests/`: Lightweight, standalone CTest suites.
  - `benchmarks/`: Automated performance measurement harnesses.

---

## 9. Safe Memory & Resource Management
- In modern C++ code, use RAII for all resource lifecycles:
  - Wrap encoder/decoder handles (`CFHD_DecoderRef`, `CFHD_EncoderRef`, `CFHD_MetadataRef`) in smart pointers with custom deleters.
  - Wrap GPU streams, device buffers, and pinned host allocations in RAII wrappers.
- Never leak raw handles or invoke raw `free()` on memory allocated with `_aligned_malloc()`.

---

## 10. Performance Regression Protection
- Before claiming optimization improvements or completing acceleration milestones, run `benchmark_codec` and compare results against the recorded baseline in:
  ```
  benchmarks/results/reference_windows_msvc.json
  ```
- Record FPS, Megapixels/sec, latency (avg/min/max), and peak memory footprint.
- Do not commit changes that cause unexplained throughput drops or latency spikes.

---

## 11. Git & Commit Conventions
- Use **Conventional Commits** formatting:
  - `feat(...)`: New features, acceleration backends, or tools.
  - `fix(...)`: Bug fixes or compatibility corrections.
  - `perf(...)`: Proven performance optimizations.
  - `test(...)`: New test cases or test framework updates.
  - `docs(...)`: Documentation updates.
  - `build(...)`: Build system or CMake adjustments.
- Ensure `git status --porcelain upstream/` is always 100% empty before committing.
