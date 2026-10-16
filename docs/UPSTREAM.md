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

Decoding the same sample twice does not reconstruct the same pixels. The stock library is built with `_THREADED_DECODER=1`, and `InvertHorizontalStrip16sToYUYV` adds `rand()` into the inverse rounding. Those calls consume CRT random state on decoder worker threads; dither assignment and a persistent worker RNG stream can differ between decodes. `srand` does not make two opens match. Pixel PSNR is therefore not a pass/fail gate for this test. Both tests still require `CFHD_DecodeSample` to return success and the output geometry to be 128×128 YUY2 with pitch 256. Upstream sources were not modified.

### 9. Decoder Scheduling Hazard and Scratch Sizing

The Phase 0 source audit corrects the earlier explanation of the multithreaded decoder failure. On this frozen revision, `DecodeEntropyInit` (`upstream/Codec/decoder.c:1689`) caps the entropy pool at **four** workers. Lazy output pools can use the full processor capability count (up to the usual maximum of 32). The previous claim of 32 entropy workers indexing an eight-worker allocation does not match these sources.

`BANDS_ALL_STARTED` (`image.h:231`) checks the highpass mask, while `BANDS_ALL_VALID` (`image.h:230`) compares `band_valid_flags` with `band_started_flags`. Completion sets both masks. `DecodeSampleSubband` queues an inverse when its highpasses have been started; entropy workers then claim queued inverses under the lock when `BANDS_ALL_VALID` is true and mark the queue entry done **before** executing the transform.

A lower wavelet with completed highpasses but no parent-produced LL can have both masks equal to `0x0E`. It passes this predicate while another worker is still reconstructing its LL. The child inverse can therefore consume stale LL memory. The final worker wait cannot repair the already computed output. This is a general dependency hazard, not a test-only issue; the source establishes the possible schedule, rather than providing an instruction trace of a particular historical failure. There is no `WaitForTransformLevel` function or `0x0F`/`0xF0` state comparison in this revision.

`AllocDecoderBuffer` (`decoder.c:1179`) scales its general scratch allocation above four and sixteen CPUs. Output workers divide it by pool size and reserve row/intermediate storage; entropy reconstruction has separate per-worker `threads_buffer` allocations. Scratch capacity remains a format/worker-count risk to verify, but the earlier claimed hardcoded `8 * subband_size` overflow mechanism is unsupported by this source.

The existing Phase 3A verification wrapper and new Phase 0 reference harness set `TAG_CPU_MAX = 1` before the first decode and clamp the typed native decoder capability/limit fields. The Phase 0 harness also verifies that entropy workers remain zero and any output pool has at most one worker. No upstream file is changed. A future GPU decoder must require all four input-band completions and explicit level dependencies; queue claim does not establish input readiness.

See [DECODE_PHASE0.md](DECODE_PHASE0.md) for the call graph, exact predicates, scheduling analysis and deterministic BGRA/YU64 reference. YUY2 final-strip dither remains a separate repeatability limitation, including when a persistent output worker has its own CRT RNG state.

### 10. SSE2 temporal inverse reads past the end of the plane allocation

Host AddressSanitizer (MSVC 19.44, `/fsanitize=address`) reports a 16-byte
heap-buffer-overflow read in `InvertTemporalQuant16s` (`upstream/Codec/temporal.c:9606`)
for 1920x1080 and 3840x2160 FIELDPLUS/GOP-2 samples that are decoded through
`CFHD_DecodeSample` into a `CFHD_PIXEL_FORMAT_YU64` target. The reported chain is
`TransformInverseTemporalQuant` (`wavelet.c:2355`) -> `ReconstructWaveletBand` ->
`DecodeSampleSubband`, and the read lands exactly 0 bytes past a plane allocated by
`AllocWaveletStack` (`wavelet.c:478`) through `AllocImage` (`image.c:169`).

The SSE2 loop preloads the next iteration's lowpass and highpass coefficients with
unconditional `_mm_load_si128` loads. On the last group of the last row that read-ahead
passes the end of the plane. The 64x64 variant of the same path stays inside its
allocation and reports nothing, so the behaviour is size dependent.

The access is a read, it happens inside the frozen oracle rather than in HyperCine
production decode, and the frozen reference hashes for the same fixtures still
reproduce in a non-ASan Release build. It is recorded as a plausible explanation for
the single historical Windows heap-corruption exit in a full CTest run, not as a proven
cause. Upstream is not modified; HyperCine SIMD backends must keep defined boundary
behaviour and must not reproduce this read-ahead. Evidence:
[DECODE_PHASE3B2A_ISSUE13_WINDOWS.md](DECODE_PHASE3B2A_ISSUE13_WINDOWS.md) and
`benchmarks/results/decode_phase3b2a_issue13_windows_20261016/`.

### RC extreme-input encoder capacity probe

The Phase 3A-RC test generator observed an upstream access violation (Windows
exit `0xC0000005`) while encoding deterministic full-range binary noise at
1920x1080, YUY2, FilmScan2, with `CFHD_ENCODING_FLAGS_NONE`. One hundred preceding
progressive configurations completed; the logged 101st configuration failed.
Repeating the bounded 144-configuration search with the existing public
`CFHD_ENCODING_FLAGS_LARGER_OUTPUT` flag completed successfully. This flag is
used only by the extreme-input regression generator and preserves progressive
coding and the requested quality. The observation is consistent with an output
capacity limitation; the exact memory-fault mechanism was not established.
No upstream source or encoder algorithm was changed. See
[DECODE_PHASE3A_RC.md](DECODE_PHASE3A_RC.md) for the retained probe evidence.

### GOP-1 interlaced reconstruction and inactive-metadata BGRA

Phase 3B-1 keeps the SDK frozen and reproduces the SSE2 sequence in
`InvertInterlacedRow16sToRow16u`: saturated signed low-minus-high (even row)
or low-plus-high (odd row), saturated signed addition of 30720, unsigned
saturated subtraction of 30720, arithmetic right shift by one, then a
16-bit left shift by six for the upstream output container. Ordinary negative
10-bit reconstruction values clamp to zero before the right shift. This is
not equivalent to unsigned reinterpretation before reconstruction or to a
signed shift followed by output clipping.

At artificial extreme signed-16 inputs, the bias addition can remain negative.
The subsequent unsigned subtraction then has wrap semantics; replacing the
instruction sequence with a generic clamp changes those results. The direct
CUDA regression compares all 225 pairs from fifteen signed boundary values,
plus repeated first/last-row and padded-pitch coverage, against both an
independent CPU instruction model and the pristine upstream routine's packed
output. This quirk is preserved in the wrapper rather than patched upstream.

Inactive-metadata interlaced BGRA uses `ChannelYUYV16toPlanarYUV16`, then
`PlanarYUV16toPlanarRGB16`, including `TWEAK_YUV2RGB`, followed by
`ConvertLinesToOutput`. Progressive BGRA retains the separate eight-bit
conversion. The existing lowpass offsets are +24 for BGRA and +4 for the
planar/YU64 reference convention. No YU64 production output is added.

An interrupted GOP-1 sample missing its final frame trailer still contains all
thirty entropy chunks but is rejected by `CFHD_DecodeSample` with error 2048.
The new production interlaced path now requires a frame/group trailer and
rejects a contradictory non-YUV encoded-format tag. The progressive parser
policy remains unchanged. See [DECODE_PHASE3B1.md](DECODE_PHASE3B1.md) for the
validation and promotion record.

### Progressive FIELDPLUS deepest inverse bottom border

`InvertSpatialQuantOverflowProtected16s` in `upstream/Codec/spatial.c` delays
advancing its low-low input pointer until the interior-row loop. After that loop,
the low-low pointer addresses row `height - 2`, while the high-low pointer has
advanced to row `height - 1`. The final vertical border therefore filters the
low-low taps from rows `height - 2`, `height - 3`, and `height - 4`, and applies
the high-pass correction from row `height - 1`. This differs from the nominal
bottom-border filter and from the low-high boundary row handling.

The Phase 3B-2A 64×64 progressive FIELDPLUS fixture exposed this behavior on its
narrow U-channel w5 band: using the nominal bottom row changed 16 reconstructed
values by a constant 256 and propagated into the temporal-low stage. The
HyperCine CUDA wrapper models the frozen pointer position only for the
overflow-protected FIELDPLUS inverse. It does not alter upstream source or the
validated GOP-1 inverse path. The exact w5, w4, temporal, planar YU64 and BGRA
oracle checks cover this compatibility rule.
