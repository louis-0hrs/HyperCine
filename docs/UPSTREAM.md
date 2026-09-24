# Upstream CineForm SDK Reference

This document records the exact upstream provenance, licensing, integration rules, and known quirks of the GoPro CineForm SDK reference baseline incorporated into HyperCine.

---

## 1. Upstream Provenance

- **Repository:** [https://github.com/gopro/cineform-sdk](https://github.com/gopro/cineform-sdk)
- **Baseline Git Commit:** `11574d0295771edccadd17af14af74a539f924c7`
- **Upstream Release / Branch:** `master`
- **Local Directory:** `upstream/`

The upstream repository was incorporated into HyperCine via `git subtree` to maintain full historical integrity and traceability back to GoPro's original open-source release.

---

## 2. Licensing and Copyright

The upstream GoPro CineForm SDK is dual-licensed under:
1. **The MIT License** (`upstream/LICENSE.txt`)
2. **The Apache License, Version 2.0** (`upstream/LICENSE-APACHE.txt`)

### Upstream Copyright Notice
```
Copyright (c) 2017 GoPro, Inc.
```

### HyperCine Policy
- All original upstream copyright notices, license headers, and license files are preserved intact without modification.
- HyperCine itself is distributed under the same dual MIT / Apache-2.0 terms to ensure total downstream compatibility.
- Any new HyperCine code (residing outside `upstream/`) carries the dual MIT / Apache-2.0 license notice attributing contributions to the HyperCine project contributors.

---

## 3. Strict Upstream Immutability Rule

> [!IMPORTANT]
> The entire contents of `upstream/` are **immutable reference code**.
> No file under `upstream/` shall be modified, refactored, modernized, or reformatted.

### Rationales
1. **Pristine Reference Baseline:** We must always have an authoritative, unaltered reference implementation to verify mathematical correctness, bitstream compatibility, and regression behavior against modern MSVC and Clang toolchains.
2. **Upstream Drift & Rebasing:** Keeping `upstream/` untouched allows clean, conflict-free tracking or rebasing if any upstream patches or community fixes are ever merged.
3. **Clean Architecture:** Modern C++17 wrappers, high-level APIs, and GPU accelerators must sit in `include/hypercine/` and `src/`, consuming upstream via clean public headers and library links rather than inline code tampering.

---

## 4. Documented Upstream Quirks and Behaviors

During the Phase 0 baseline verification on modern Windows (Visual Studio 2022 / MSVC 19.44 x64), several upstream implementation quirks and behaviors were documented:

### 1. MSVC Compiler Warnings
The upstream C codebase was authored for older compilers (C99 / MSVC 2015). Under modern MSVC 19.44 with default flags, it emits several classes of benign warnings:
- **`C4996` (Deprecated CRT Functions):** Extensive use of `strcpy`, `sprintf`, `fopen`, and POSIX names (`stricmp`, `strnicmp`). Suppressed via `_CRT_SECURE_NO_WARNINGS` and `_CRT_NONSTDC_NO_WARNINGS` in the root `CMakeLists.txt`.
- **`C4244` / `C4267` (Type Truncation / Conversion):** Implicit conversions between `size_t` (64-bit on x64) and `int` / `short` (32-bit / 16-bit). Suppressed at compile time (`/wd4244 /wd4267`).
- **`C4101` (Unreferenced Local Variables):** Several legacy scratch variables. Suppressed (`/wd4101`).

### 2. Multi-Core Processor Detection (`GetProcessorCount`)
In `upstream/Common/thread.c`, processor core counts are queried using `GetSystemInfo` or `GetActiveProcessorCount`. On modern systems with >64 logical processors or asymmetric P/E cores, legacy logic limits thread counts to 32 or 64. HyperCine's host wrappers handle thread pool allocation externally rather than modifying this upstream routine.

### 3. `ConvertRGBAtoRGB48` Inversion Hazard
In `upstream/Codec/encoder.c` (lines 2398–2402), handling of inverted RGB buffers (`COLOR_FORMAT_RGB32_INVERTED` / `CFHD_PIXEL_FORMAT_BGRa`) offsets the input pointer to the bottom row (`data += (display_height - 1) * pitch; pitch = -pitch;`). In `upstream/Codec/frame.c` (`ConvertRGBAtoRGB48`, line 6364), a secondary inversion calculation (`rgb_ptr += (display_height - 1 - row) * rgb_row_pitch;`) results in reading before the beginning of the buffer.
- *Mitigation:* In reference tests and benchmarks, `CFHD_PIXEL_FORMAT_YUY2` / `CFHD_PIXEL_FORMAT_BGRA` are used with correct stride and non-conflicting format paths. HyperCine's GPU color conversion pipeline bypasses this CPU routine entirely.

### 4. Strict 16-Byte Alignment Requirement for SSE2
The upstream decoder (`upstream/Codec/decoder.c` and `upstream/Convert/`) executes vectorized SSE2 instructions (`_mm_load_si128`). If input or output image buffers are not aligned to a 16-byte boundary, an unhandled exception or internal `catch` block occurs, causing frames to decode as black or empty buffers.
- *Mitigation:* All HyperCine buffers, test harnesses, and wrappers utilize `AlignedAllocator<T, 16>` or platform aligned allocations (`_aligned_malloc`).

### 5. Decoder Active Metadata Initialization (`TAG_PROCESS_PATH`)
The upstream sample decoder (`SampleDecoder.cpp`) requires explicit initialization of active metadata before decoding frames:
```cpp
CFHD_MetadataRef metadata;
CFHD_InitSampleMetadata(&metadata, ...);
CFHD_SetActiveMetadata(decoder, metadata, TAG_PROCESS_PATH, ...);
```
Without `TAG_PROCESS_PATH` set to `PROCESSING_ALL_ON (0xffff)`, decoder process flags remain uninitialized, leading to bypassed color stages. HyperCine test suites and wrappers always explicitly set this metadata.

### 6. CMake `string(STRIP ${ADDITIONAL_LIBS} ...)` Syntax Error When CUDA is Enabled
In `upstream/CMakeLists.txt` (line 35), `string(STRIP ${ADDITIONAL_LIBS} ADDITIONAL_LIBS)` is called without quotes. When CUDA is enabled as a project language in modern CMake, `find_package(OpenMP)` in upstream checks OpenMP for C, CXX, and CUDA. Because CUDA lacks OpenMP flags, `OpenMP_FOUND` evaluates to false, leaving `ADDITIONAL_LIBS` unset. An unquoted empty variable causes `string(STRIP)` to fail with "requires two arguments".
- *Mitigation:* The root `CMakeLists.txt` initializes `set(ADDITIONAL_LIBS " ")`, `set(OpenMP_CUDA_FOUND TRUE)`, and `set(OpenMP_CUDA_FLAGS "")` before `add_subdirectory(upstream)`, ensuring clean CMake configuration without modifying any upstream file.

