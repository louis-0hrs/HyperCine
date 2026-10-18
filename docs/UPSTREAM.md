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
1. **The MIT License** (`upstream/LICENSE-MIT` and `upstream/LICENSE.txt`)
2. **The Apache License, Version 2.0** (`upstream/LICENSE-APACHE` and `upstream/LICENSE.txt`)

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

### 7. `_THREADED_ENCODER` does not build

`upstream/Codec/config.h` defaults `_THREADED_ENCODER` to 0. The active encode path therefore calls `EncodeQuantizedGroup`, not `EncodeQuantizedGroupThreaded`.

Compiling the same pristine sources with `-D_THREADED_ENCODER=1` fails, so there is no functioning upstream threaded configuration on this tree:

- In C++, `encoder.h` uses the `ENCODER` typedef inside the threaded structs (around line 124) before the typedef at line 203. A forced forward declaration (`cmake/threaded_encoder_prefix.h`, `/FI`, no upstream edit) gets past that C++ rule.
- `encoder.c` then fails inside the threaded functions. Examples from the MSVC 19.44 build: `display_height` and `ChromaFullRes` are undeclared, `encoder` has no `display_height` or `metasize` members, and calls such as `SetEncoderFormat`, `QuantizationSetRate`, `PutVideoIntraFrameHeader`, and `EncodeQuantizedFrameTransform` do not match their current prototypes. `wavelet.c` has the same class of mismatch in `FilterSpatialYUVQuant16s`.

The optional `CFHDCodecThreaded` target is `EXCLUDE_FROM_ALL`. It reproduces this failure. The sources under `upstream/` were not modified. The stock encoder used for performance comparisons remains the default build (`_THREADED_ENCODER=0`).

### 8. 128×128 YUY2 roundtrip is not pixel-stable

`tests/test_codec_roundtrip` encodes a fixed 128×128 YUY2 corpus with the stock SDK and decodes it again. Across separate processes the encoded sample size stays constant and the wavelet payload stays constant, but the sample hash does not. The bytes that change are the fixed-width metadata written by `CSampleEncoder::HandleMetadata`:

- `TAG_CLIP_GUID` (`GUID`, 16 bytes) from `CoCreateGuid` on every fresh encoder
- `TAG_ENCODE_DATE` (`DATE`), `TAG_ENCODE_TIME` (`TIME`), and `TAG_TIMECODE` (`TIMC`) from `localtime`

`tests/test_yuy2_stability` zeroes those payloads and requires the remaining sample to match across two encoder opens in the same process, including across a one-second clock step. It also requires the two GUID payloads to differ. A second process can still differ by a few coded bytes (4 bytes on a flat black frame, repeated at the same sites in two channels). The regression therefore compares opens inside one process and does not freeze a cross-process hash.

Decoding the same sample twice does not reconstruct the same pixels. The stock library is built with `_THREADED_DECODER=1`, and `InvertHorizontalStrip16sToYUYV` adds `rand()` into the inverse rounding. Those calls share the process CRT state across decoder worker threads, so the dither assignment depends on scheduling. `srand` does not make two opens match. Pixel PSNR is therefore not a pass/fail gate for this test. Both tests still require `CFHD_DecodeSample` to return success and the output geometry to be 128×128 YUY2 with pitch 256. Upstream sources were not modified.

### 9. Multi-Threaded Decoder Bugs on >8-Core CPUs (`AllocDecoderBuffer` Scratch Overflow & `BANDS_ALL_STARTED` Race)

In `upstream/Codec/decoder.c` (`_THREADED_DECODER=1`), two latent concurrency bugs occur on modern CPUs with more than 8 logical processors (e.g., 16-core / 32-thread CPUs) when decoding high-entropy or multi-frame sequences:

1. **Worker Thread Scratch Buffer Heap Overflow (`AllocDecoderBuffer`, lines 5192–5215 vs `InitDecoder`, line 2131):**
   `AllocDecoderBuffer` allocates `decoder->buffer.scratch.size = 8 * Align16(subband_size)` (hardcoded for at most 8 concurrent worker threads), while `InitDecoder` sets `threads = GetProcessorCount()` (clamped to `ENTROPY_THREADS_MAX = 32` in `upstream/Codec/entropy_threading.c`). In `EntropyDecodeBand` (line 11523), worker threads with `thread_index >= 8` index `decoder->buffer.scratch.base_ptr + thread_index * scratch_size`, reading and writing up to `24 * scratch_size` past the end of the heap allocation when decoding high-complexity frames whose subbands queue more than 8 entropy jobs concurrently.
2. **Wavelet Reconstruction Race Condition (`DecodeSampleTransform`, line 11703):**
   In `DecodeSampleTransform`, when level-1 highpass bands (`HL_BAND`, `LH_BAND`, `HH_BAND`) are queued to the entropy worker pool, the main thread checks `decoder->thread_cntrl.state[transform_index][wavelet_index] == BANDS_ALL_STARTED` (`0x0F`, set as soon as worker threads *start* entropy decoding) instead of `BANDS_ALL_VALID` (`0xF0`, set when worker threads *finish* entropy decoding). Only if that check fails does it wait on `WaitForTransformLevel(decoder, transform_index, wavelet_index)` for `BANDS_ALL_VALID`. If worker threads have started all 3 bands (`state == 0x0F`) but have not yet finished writing decoded coefficients (`state != 0xF0`), the main decoder thread immediately proceeds to `ReconstructWaveletBand` and reconstructs from partially decoded highpass band memory.

- *Mitigation:* In accordance with Rule 1 (`upstream/` immutability), `upstream/Codec/decoder.c` is untouched. In HyperCine's decode verification harnesses (`DecodeCfhdSample3A` in `src/core/phase3a_encoder.cpp`), the upstream decoder worker count is clamped to 1 thread (`TAG_CPU_MAX = 1` via `CFHD_SetActiveMetadata` and `m_decoder->thread_cntrl.capabilities = (capabilities & 0xffff) | (1 << 16)` after `CFHD_PrepareToDecode`), forcing `EntropyDecodeBand` onto the synchronous `thread_count == 1` path (`scratch_memory = decoder->buffer.scratch.base_ptr`, no worker race).

### 10. Small-width horizontal highpass prefix reads (#21)

On Windows x64 with `_XMMOPT=1`, pristine `FilterHorizontalRow16s` skips
its SIMD loop for row widths 12/14/16 and enters the scalar interior formula
at column zero, reading `input[-2]` and `input[-1]`. Controlled aligned prefix
experiments change exactly `highpass[0]`; a poisoned-prefix MSVC ASan run
reports the read in `upstream/Codec/spatial.c:540`. A flat width-16 row of 512s
produces 0, +128 or -128 solely from changing prefix values.

The 128px YUV422 deepest-chroma width-16 SDK coefficients are therefore
`UPSTREAM_ORACLE_UNDEFINED`. HyperCine must retain defined boundary handling
rather than mimic arbitrary memory content. Production code and upstream
remain unchanged. The test-only negative regression preserves this oracle
limitation and checks prefix independence at larger tested widths, without
granting new full-frame parity claims. See [diagnosis and reproduction](SMALL_CHROMA_ORACLE.md).

### 11. `CFHD_GetImagePitch` / `CFHD_GetImageSize` throw out of `extern "C"`

`upstream/DecoderSDK/SampleDecoder.cpp` reaches `default: throw CFHD_ERROR_BADFORMAT;` in `GetPixelSize` for any pixel format its switch does not list. That path is reachable from the public helpers `CFHD_GetImagePitch` and `CFHD_GetImageSize`, which `upstream/Common/CFHDDecoder.h` declares `extern "C"`.

MSVC's `/EHsc` — the CMake default, and the model the upstream library is built with — tells the compiler that `extern "C"` functions never throw. A caller compiled with `/EHsc` leaves out the unwind information for that call, so the throw is undefined behaviour rather than a catchable exception. Measured with MSVC 19.44 on Windows x64 Release: a caller that merely wraps the call in `try { ... } catch (CFHD_Error) { ... }` reproducibly terminates the process without entering the handler. The outcome is codegen-sensitive — in `tools/format_corpus_probe.cpp` an unrelated extra logging statement near the call was enough to change whether the same source survived.

- *Mitigation:* `tools/format_corpus_probe.cpp` is compiled with `/EHs`, which drops the `extern "C"`-never-throws assumption; the identical catch then works and the probe completes its full case matrix. MSVC reports D9025 because `/EHs` overrides the inherited `/EHsc`. Callers that never pass an unlisted pixel format are unaffected. `upstream/` is untouched.

### 12. Encoder `rand()` state is seeded from volatile metadata; Bayer payloads are run-dependent

`upstream/Codec/encoder.c` derives an uncompressed-frame decision seed from the first frame bytes
plus the CRCs of the global and local metadata blocks, then calls `srand(seed)` before testing
`rand()`. Because every encoded packet carries a fresh `GUID` and wall-clock `DATE`/`TIME`/`TIMC`
metadata (quirk 8), that seed changes between runs. The Bayer conversion path uses `rand()` for
7-bit dithering in `upstream/Codec/bayer.c` (`ConvertBayerToRGBA` and related helpers), so those
dither values — and therefore the compressed Bayer payload bytes — differ between runs.

Measured with `tools/format_corpus_export.cpp` on MSVC 19.44 Windows x64 at 128×128, FilmScan 1:

- `RGB_444` and `RGBA_4444`: the raw packet hash differs between runs (metadata), but the
  GUID/DATE/TIME/TIMC-normalized hash and the decoded-output hash reproduce.
- `BAYER` (`BYR4`): the normalized hash and the decoded-output hash also differ between runs.
  Both a flat frame and a high-detail frame differed in the final measurement.
  `OMP_NUM_THREADS=1` did not remove the difference.

- *Mitigation:* F1 Bayer hashes are recorded as per-run evidence, not as a cross-run byte gate.
  The export tool keeps the raw and normalized packets exactly as measured. Upstream remains
  untouched; a future deterministic corpus must either seed the CRT explicitly inside a wrapper
  harness or record the seed-to-payload relationship instead of assuming byte equality.

### 13. 8-bit `RG24` and `BGRA` sources are read bottom-up; `RG24` bytes are B,G,R

The public pixel-format comments call both formats "inverted", and the encoder path confirms it in
memory rather than in the four-character name. Measured with `tools/format_corpus_export.cpp` at
128×128 FilmScan 1 by encoding a known asymmetric scene and comparing the decoded `RG48`/`b64a`
output:

- `CFHD_PIXEL_FORMAT_RG24` (`COLOR_FORMAT_RGB24`): the SDK reads the row buffer bottom-up and
  interprets the three bytes as **B,G,R**. A top-down R,G,B buffer decodes with G at ~0 dB PSNR and
  the red/blue channels swapped; flipping the rows and writing B,G,R raises the same case to
  ~42–51 dB.
- `CFHD_PIXEL_FORMAT_BGRA` (`COLOR_FORMAT_BGRA`): the SDK reads the buffer bottom-up, with the
  expected B,G,R,A byte order. A top-down buffer decodes vertically flipped; flipping the rows
  raises the measured PSNR from ~5–11 dB to ~53 dB.

- *Mitigation:* the F1 exporter stores `RG24` and `BGRA` sources in the orientation and byte order
  the SDK expects, records that orientation in the manifest, and flips only those source rows in
  the same-layout comparison. `RG48`, `RG64` and `BYR4` are top-down. Upstream remains untouched.
