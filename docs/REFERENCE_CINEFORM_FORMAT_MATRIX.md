<!-- Copyright (c) 2026 HyperCine Contributors. Dual-licensed under the MIT License or Apache License, Version 2.0. See LICENSE-MIT and LICENSE-APACHE for details. -->

# CineForm Format Capability Matrix (Issue #36, stage F0)

Measured with the pristine GoPro CineForm SDK, on Windows x64 with MSVC 19.44 in Release, at 256×256.
This is the **F0 capability matrix** of [issue #36](https://github.com/louis-0hrs/HyperCine/issues/36): it records which
compressed CineForm families the original SDK actually encodes *and* decodes, and which it rejects, using the real
SDK entry points rather than header declarations.

It is **not** a fidelity statement. The probe measures API capability on a synthetic source; it makes no claim about
decoded pixel values, PSNR, or visual correctness. A `BIDIRECTIONAL_PASS` here does **not** mean HyperCine's own
encoder or decoder supports that mode.

- Upstream reference (frozen, unmodified): `11574d0295771edccadd17af14af74a539f924c7`
- Generator: [`tools/format_corpus_probe.cpp`](../tools/format_corpus_probe.cpp)
- Machine-readable evidence: [`benchmarks/results/format_capability_f0_windows.json`](../benchmarks/results/format_capability_f0_windows.json)

---

## 1. How the probe measures

For every compressed family and candidate source `CFHD_PixelFormat`, one deterministic synthetic frame is pushed
through the real SDK:

1. `CFHD_OpenEncoder` and `CFHD_GetInputFormats` (declared encoder input list).
2. `CFHD_PrepareToEncode` → `CFHD_EncodeSample` → `CFHD_GetSampleData`.
3. `CFHD_OpenDecoder` on a **fresh instance**, then `CFHD_GetSampleInfo`, `CFHD_GetOutputFormats`,
   `CFHD_PrepareToDecode`, `CFHD_DecodeSample` with `TAG_PROCESS_PATH` and `TAG_CPU_MAX=1` set through
   `CFHD_SetActiveMetadata` (the documented workaround for the >8-core decoder race, see
   [UPSTREAM.md](UPSTREAM.md) quirk 9).

Each case is run for all six regular qualities (`low`, `medium`, `high`, `filmscan1`, `filmscan2`, `filmscan3`).

**Verdicts**

| Verdict | Meaning |
| --- | --- |
| `BIDIRECTIONAL_PASS` | Both directions returned `CFHD_ERROR_OKAY` for this family, source format and quality. |
| `SDK_UNSUPPORTED` | The SDK rejected the combination (`CFHD_ERROR_BADFORMAT`, `CFHD_ERROR_INVALID_ARGUMENT`, `CFHD_ERROR_BADSCALING`, `CFHD_ERROR_BAD_RESOLUTION`, `CFHD_ERROR_LICENSING`), either as a return code or as a thrown value. |
| `DECODE_FAILED` | Encoding succeeded but decoding the produced sample failed with a non-capability error code. |
| `ENCODE_FAILED` | Encoding failed with a code outside the capability set (an SDK defect, not a rejection). |
| `NOT_TESTED` | The probe could not build an input for this format. |

## 2. Running it

From a Windows x64 checkout with the SDK reference build enabled (the default):

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target format_corpus_probe
build/tests/Release/format_corpus_probe.exe --out build/f0-matrix.json
```

`--width`, `--height`, `--family NAME` and `--source CODE` narrow the run, which is the supported way to isolate a
single hostile combination without walking the whole matrix.

## 3. Capability matrix

Verdict is identical across all six qualities unless noted. Source format codes are the SDK's four-character codes.

| Encoded family | Source `CFHD_PixelFormat` | Verdict | Measured evidence |
| --- | --- | --- | --- |
| `YUV_422` (0) | `BGRA`, `BGRa`, `YUY2`, `2vuy`, `v210`, `YU64`, `RG24` | `BIDIRECTIONAL_PASS` | Sample sizes 26 KB (low) to 139 KB (FilmScan3) at 256×256; decode returns BGRA 256×256. |
| `YUV_422` | `yuyv` (`CFHD_PIXEL_FORMAT_YUYV`) | `SDK_UNSUPPORTED` | `PrepareToEncode` accepts, `EncodeSample` returns `CFHD_ERROR_BADFORMAT`. |
| `RGB_444` (1) | `RG24`, `RG48`, `BGRA`, `RG30`, `AB10`, `AR10`, `r210`, `DPX0` | `BIDIRECTIONAL_PASS` | Decode reports `CFHD_SAMPLE_ENCODED_FORMAT = 1`; output BGRA 256×256. |
| `RGB_444` | `WP13` | `SDK_UNSUPPORTED` | `PrepareToEncode` accepts, `EncodeSample` returns `CFHD_ERROR_BADFORMAT`. |
| `RGB_444` | `b48r` (`CFHD_PIXEL_FORMAT_B48R`) | `SDK_UNSUPPORTED` | `CFHD_GetImagePitch`/`CFHD_GetImageSize` throw `CFHD_ERROR_BADFORMAT` while sizing the format. |
| `RGBA_4444` (2) | `BGRA`, `BGRa`, `b64a`, `RG64` | `BIDIRECTIONAL_PASS` | Decode reports `CFHD_SAMPLE_ENCODED_FORMAT = 2`. |
| `RGBA_4444` | `W13A` | `SDK_UNSUPPORTED` | `PrepareToEncode` accepts, `EncodeSample` returns `CFHD_ERROR_BADFORMAT`. |
| `BAYER` (3) | `BYR4` | `BIDIRECTIONAL_PASS` | Decode reports `CFHD_SAMPLE_ENCODED_FORMAT = 3`; the offered output list starts with `BYR2`, `BYR4`. |
| `BAYER` | `BYR2` | `SDK_UNSUPPORTED` | `PrepareToEncode` accepts, `EncodeSample` returns `CFHD_ERROR_BADFORMAT`. |
| `BAYER` | `BYR5` | `SDK_UNSUPPORTED` | `CFHD_GetImagePitch`/`CFHD_GetImageSize` throw `CFHD_ERROR_BADFORMAT` while sizing the format. |
| `YUVA_4444` (4) | `BGRA`, `BGRa` | `DECODE_FAILED` | Encoding **succeeds**, `CFHD_PrepareToDecode` returns `CFHD_ERROR_OKAY`, `CFHD_DecodeSample` returns `CFHD_ERROR_CODEC_ERROR` (2048). No decoded output. |

Totals over 168 cases (28 format pairs × 6 qualities): **120 `BIDIRECTIONAL_PASS`, 36 `SDK_UNSUPPORTED`,
12 `DECODE_FAILED`, 0 `ENCODE_FAILED`, 0 `NOT_TESTED`.**

### 3.1 Consequences worth carrying forward

- **`YUVA_4444` is not usable through the SDK.** The encoder produces a sample and the decoder accepts its header,
  but decoding fails with a codec error. This matches the SDK's own "not implemented" comment on
  `CFHD_ENCODED_FORMAT_YUVA_4444`; the family must be documented as unavailable rather than fabricated, and no
  F1 corpus can be produced for it.
- **`BYR5` cannot even be sized.** The Bayer 12-bit packed input format is rejected by the SDK's geometry helpers,
  which is stronger than "encoding is unsupported": the probe cannot allocate a source for it at all.
- **`WP13` / `W13A` / `BYR2` are accepted at the prepare stage and rejected at encode.** A caller that only checks
  `CFHD_PrepareToEncode` would wrongly conclude these work, which is why this matrix records the per-stage code.
- **The declared encoder input list is not the capability list.** `CFHD_GetInputFormats` returns
  `RG64 b64a BYR4 BYR5 RG48 RG30 AB10 AR10 r210 DPX0 BGRA BGRa RG24 v210 YUY2 2vuy`. It advertises `BYR5`, which
  the geometry helpers then reject, and it omits `YU64`, which does encode. Do not derive format support from that
  enumeration alone.

## 4. Observed sample metadata

`CFHD_GetSampleInfo` on the produced samples returns, for a YUV 4:2:2 256×256 case:

```json
{"encoded_format": 0, "channels": 1, "display_width": 256, "display_height": 0,
 "progressive": 1, "key_frame": 1, "sdk_version": 655616, "encode_version": 655616}
```

`CFHD_SAMPLE_ENCODED_FORMAT` tracks the family correctly (0/1/2/3), and it is the value to gate on. Note that
`CFHD_SAMPLE_DISPLAY_HEIGHT` reports 0 while `CFHD_PrepareToDecode` returns the real 256×256 geometry; the tag is
recorded as measured and no semantics are asserted for it here.

The decoder offers a 16-entry output list for YUV 4:2:2 samples, in decreasing order of stated preference:

```
YU64 v210 2vuy YUY2 b64a r210 DPX0 RG30 AB10 AR10 RG48 WP13 W13A BGRA BGRa RG24
```

The probe requests `BGRA` whenever it is offered so results stay comparable across families. For `YUVA_4444`
samples the SDK returns the same list, which is a consequence of the sample header parsing defaulting rather than
evidence that the family decodes.

## 5. Determinism

Verified over three consecutive runs at 256×256: `source_fnv1a64`, every encode/decode status, the sample
geometry, the offered output list and `output_fnv1a64` are **identical**.

The raw **sample** hash is *not* reproducible, and this is expected upstream behaviour: the pristine encoder stamps
wall-clock `DATE`/`TIME`/`TIMC` metadata and a fresh `GUID` into every sample (see [UPSTREAM.md](UPSTREAM.md)
quirk 8 and the existing `tests/test_yuy2_stability`). The matrix JSON records this explicitly in its
`determinism` block. Normalizing those value bytes is stage F1 work; until then the raw sample hash must not be
used as a cross-run gate.

## 6. Method notes and limits

- **Compilation model.** The probe is built with `/EHs`. The SDK's geometry helpers throw across an `extern "C"`
  boundary, which is undefined behaviour under MSVC's default `/EHsc`; see [UPSTREAM.md](UPSTREAM.md) quirk 11.
  Without this flag the probe aborts part-way through the matrix on the first format the SDK rejects.
- **Geometry.** 256×256 was chosen deliberately: 128-pixel widths are `UPSTREAM_ORACLE_UNDEFINED` for the deepest
  chroma band ([UPSTREAM.md](UPSTREAM.md) quirk 10) and must not be used as an oracle. F1 still owes the
  1920×1080 measurements.
- **Source signal.** The synthetic source is a deterministic per-pixel byte ramp. It is adequate for asking
  "does the SDK accept this combination"; it is not a valid source for judging fidelity, channel semantics, alpha
  handling or Bayer phase. Those need the real per-family F1 sources described in issue #36.
- **Stage boundaries.** `YUV_INTERLACED` and `YUV_2FRAME_GOP` encoding flags, sequence rate-control behaviour,
  4K/8K geometry, stereo/dual-channel and the KEYING / uncompressed quality flags are **F2** and were not probed.
  RGBA alpha preservation, Bayer CFA pattern and phase, and pre-quantization stage oracles are **F1**.
- **No fidelity or equivalence claim** is made anywhere in this document.

## 7. Next steps

1. **F1**: 1080p RGB444, RGBA4444 and Bayer corpora with source bytes, the pristine SDK packet and a real
   SDK-native decode, per the source lists this matrix confirms (`RG24`/`RG48`/`RG30`/`AB10`/`AR10`/`r210`/`DPX0`
   for RGB444; `BGRA`/`BGRa`/`b64a`/`RG64` for RGBA4444; `BYR4` for Bayer), plus the metadata normalization that
   makes raw sample hashes reproducible.
2. Do **not** plan F1 corpora for `YUVA_4444`, `BYR5`, `b48r`, `WP13`, `W13A`, `BYR2` or `yuyv`: measured
   `SDK_UNSUPPORTED` / `DECODE_FAILED` at this geometry.
3. Re-run this probe on Linux A2 and macOS ARM64 before any cross-platform claim; this evidence is Windows x64
   MSVC only.
