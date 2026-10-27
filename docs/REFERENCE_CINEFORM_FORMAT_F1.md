<!-- Copyright (c) 2026 HyperCine Contributors. Dual-licensed under the MIT License or Apache License, Version 2.0. See LICENSE-MIT and LICENSE-APACHE for details. -->

# CineForm Format Golden Corpora (Issue #36, stage F1)

This document describes the **F1** stage of [issue #36](https://github.com/louis-0hrs/HyperCine/issues/36):
real pristine-SDK golden corpora for the compressed families the [F0 capability matrix](REFERENCE_CINEFORM_FORMAT_MATRIX.md)
actually qualified.

- Family scope: `RGB_444` (coded 12-bit), `RGBA_4444` (coded 12-bit), `BAYER` (12-bit log-coded).
- Excluded because F0 measured them as unavailable: `YUVA_4444` (`DECODE_FAILED`), `BYR5`, `BYR2`,
  `b48r`, `WP13`, `W13A`, `yuyv` (`SDK_UNSUPPORTED`).
- Out of scope: F2 geometries beyond the measured 128 px and 1920×1080 cases, stereo/dual-channel,
  `YUV_INTERLACED`/`YUV_2FRAME_GOP`, `KEYING`, and uncompressed flags.
- A golden-PASS here is **not** evidence that HyperCine's own encoder or decoder implements the mode.

Upstream reference (frozen, unmodified): `11574d0295771edccadd17af14af74a539f924c7`.

## 1. What one case contains

`tools/format_corpus_export.cpp` writes, for every case:

| File | Content |
| --- | --- |
| `source.bin` | Exact source bytes handed to `CFHD_EncodeSample`, including row padding. |
| `sdk.cfhd` | The unmodified packet returned by `CFHD_GetSampleData`. |
| `sdk.normalized.cfhd` | The same packet with only the parsed `GUID`/`DATE`/`TIME`/`TIMC` payload bytes zeroed. |
| `decoded.bin` | A decode from a fresh decoder instance with `TAG_PROCESS_PATH` and `TAG_CPU_MAX=1`. |

Layout:

```
<family>/f1/<width>x<height>/<source>/<quality>/<pattern>/
```

Every case also records in `manifest.json`:

- source SHA-256, byte length, pitch, component count, component width and layout;
- raw and normalized packet SHA-256, sample byte length and the exact masked ranges;
- `CFHD_GetSampleInfo` values (`encoded_format`, channels, progressive, key frame, SDK/encode version);
- the offered output formats, the requested one and the actual one;
- decoded SHA-256, byte length, pitch, component width and layout;
- a same-layout comparison against the source (per-channel PSNR, maximum absolute difference,
  exact-match ratio, and alpha statistics for RGBA);
- the status (`EXPORTED` or the failing stage and SDK error).

`TRANSFER_SHA256.tsv` lists every artifact as `SHA256<TAB>bytes<TAB>path`, and the manifest records
that file's own SHA-256. `--verify DIR` re-checks both the manifest records and the transfer rows.

## 2. Deterministic sources

Sources are generated from seed `0x1337BEEF` and are independent of wall-clock time or
`std::random_device`. The measured source SHA-256 reproduces across runs.

| Family | Source layouts | Layout detail | Patterns |
| --- | --- | --- | --- |
| `RGB_444` | `RG48`, `RG24` | Packed R,G,B native-endian `uint16`, top-down (RG48); packed B,G,R 8-bit, bottom-up (RG24, the SDK's inverted layout) | `asymmetric`, `flat`, `gradient`, `primaries`, `checker`, `noise`, `edge`, `minmax` |
| `RGBA_4444` | `RG64`, `BGRA` | Packed R,G,B,A native-endian `uint16`, top-down (RG64); packed B,G,R,A 8-bit, bottom-up (BGRA) | `rgb_independent`, `alpha_zero`, `alpha_half`, `alpha_full`, `alpha_ramp`, `alpha_edge`, `alpha_noise` |
| `BAYER` | `BYR4` | Raw CFA, native-endian `uint16` per photosite, top-down | `flat`, `black`, `white`, `dark`, `ramp`, `detail`, `noise` |

`asymmetric` uses three independent ramps so a channel swap cannot pass as correct.
The RGBA patterns combine independently generated RGB signals with controlled alpha values.
Bayer sources are generated as an RGB scene sampled through the SDK default CFA
(`BAYER_FORMAT_RED_GRN`, value 0); see the CFA limitation in section 5.

## 3. Running the export

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target format_corpus_export

# 128 px, all six qualities, every implemented source
build/tests/Release/format_corpus_export.exe `
  --out "$env:HYPERCINE_GOLDEN_DIR/format-f1/128" --width 128 --height 128 --quality all

# 1920x1080 canonical p12 sources, FilmScan 1 and FilmScan 3
build/tests/Release/format_corpus_export.exe `
  --out "$env:HYPERCINE_GOLDEN_DIR/format-f1/1080p" --width 1920 --height 1080 `
  --family RGB_444 --source RG48 --quality filmscan1,filmscan3

# verify a copied corpus (local or NAS)
build/tests/Release/format_corpus_export.exe --verify "$env:HYPERCINE_GOLDEN_DIR/format-f1/128"
```

`--family`, `--source`, `--pattern`, `--quality`, `--width`, `--height` and `--list` select or
preview the matrix. `--tier f1|f2` labels the corpus tier (default `f1`); the 4K F2 stage is
documented in [REFERENCE_CINEFORM_FORMAT_F2.md](REFERENCE_CINEFORM_FORMAT_F2.md). Raw corpora stay outside Git; only the tool, schema, tests and sanitized reports
are published.

## 4. Metadata normalization and reproducibility

The pristine encoder stamps a fresh GUID and wall-clock `DATE`/`TIME`/`TIMC` into every packet
([UPSTREAM.md](UPSTREAM.md) quirk 8), so the **raw** packet hash never reproduces. Normalization
zeroes only those four tag payloads; the packet length and every other byte are preserved.

Measured at 128×128 on Windows x64 MSVC 19.44 Release:

- `RGB_444` and `RGBA_4444`: the normalized packet hash and the decoded-output hash reproduced
  across repeated runs.
- `BAYER`: both a flat source and a detail/noise source changed. Both the normalized packet and
  the decoded output differed between runs. `OMP_NUM_THREADS=1` did not remove the difference.

This is upstream behaviour, not a bug in the export tool: `upstream/Codec/encoder.c` seeds the CRT
`rand()` state from a metadata CRC, and the Bayer path uses `rand()` dithering
([UPSTREAM.md](UPSTREAM.md) quirk 12). BAYER hashes are therefore per-run evidence and must not be
used as a cross-run byte gate.

## 5. Measured output formats, alpha and Bayer behaviour

**8-bit sources are not top-down RGB.** Measured by encoding a known asymmetric scene and
comparing the decoded 16-bit output: the SDK reads `RG24` as **bottom-up B,G,R** and `BGRA` as
**bottom-up B,G,R,A**. A top-down R,G,B `RG24` buffer decoded with the red/blue channels swapped
and green at ~0 dB PSNR; a top-down `BGRA` buffer decoded vertically flipped (~5–11 dB). The
exporter stores these two sources in the SDK's orientation, records that orientation in the
manifest, and flips those source rows only for the same-layout comparison
([UPSTREAM.md](UPSTREAM.md) quirk 13). After the fix, measured 128 px ranges are
`RG24` 42–51 dB and `BGRA` 52–54 dB per channel. `RG48`, `RG64` and `BYR4` are top-down.

The decoder does not offer every requested layout:

- `RGB_444` cases request `RG48` first and receive `RG48`; the 8-bit `RG24` source cases also
  decode to `RG48`, which is compared after normalizing both sides to 16-bit component values.
- `RGBA_4444` cases request `RG64` first, but the offered list for these samples starts with
  `b64a`; the actual output is `b64a` (A,R,G,B 16-bit). The tool maps that back to R,G,B,A for
  comparison.
- `BAYER` cases request `BYR4`; the offered list is `BYR2, BYR4, b64a, ...` and the actual output
  is `BYR4` (16-bit raw CFA).

**Alpha is not a straight 16-bit roundtrip.** The SDK applies an alpha encoding curve
(`upstream/Codec/frame.c`, `ConvertRGBA64ToFrame16s`): alpha 0 and 4095 stay at the extremes, and
intermediate values pass through a 12-bit curve that is expanded to 8-bit on decode. The manifest
therefore reports separate alpha PSNR, exact-match ratio and maximum absolute difference. Do not
describe RGBA output as mathematically lossless.

**Bayer raw output is not a linear roundtrip of the linear source.** The measured CFA PSNR is low
for the high-frequency Bayer patterns, while the packet still contains the source. This reflects
the SDK's log/coded raw path; the manifest reports the measured numbers without asserting a
transform.

**CFA variants are blocked.** `TAG_BAYER_FORMAT` could not be attached through the public encoder
metadata API (`CFHD_MetadataOpen`/`CFHD_MetadataAdd`/`CFHD_MetadataAttach`) in this static SDK
build: the attempt crashed before `CFHD_EncodeSample`. Every exported BAYER case therefore uses the
SDK default CFA and does **not** prove RGGB/GRBG/GBRG/BGGR behaviour. This is reported as
`PENDING_SDK_CFA_METADATA`.

**A 1080p RG24 encoder crash excludes some 8-bit cases.** At 1920×1080, `CFHD_EncodeSample` crashes
the process for `RGB_444`/`RG24` with the `noise` pattern at every quality tested, and with
`checker` at FilmScan 3. Flat, gradient, primaries, edge, minmax and the lower-entropy patterns
return normally. The 1080p corpus therefore uses the canonical 16-bit `RG48` source; the 128 px
matrix still covers `RG24` with all patterns and qualities. The crash is recorded as an upstream
defect, not worked around and not hidden.

## 6. Measured corpus

The counts below are from the local run that produced this report. Exact hashes and per-case
numbers are in each corpus `manifest.json`; they are not copied into Git.

| Corpus | Cases | Exported | Encode failures | Decode failures |
| --- | --- | --- | --- | --- |
| 128 px, all sources and all six qualities | 222 | 222 | 0 | 0 |
| 1920×1080 `RGB_444`/`RG48`, FilmScan 1+3 | 16 | 16 | 0 | 0 |
| 1920×1080 `RGBA_4444`/`RG64`, FilmScan 1+3 | 14 | 14 | 0 | 0 |
| 1920×1080 `BAYER`/`BYR4`, FilmScan 1+3 | 14 | 14 | 0 | 0 |

Both geometries were verified with `--verify` after generation: 889 artifacts at 128 px and 65/57/57
artifacts in the three 1080p corpora, plus the corresponding `TRANSFER_SHA256.tsv` rows.

Measured same-layout PSNR ranges (all channels, all qualities and patterns in each corpus):

| Corpus | Source | Min–max PSNR |
| --- | --- | --- |
| 128 px | `RG48` | 28.6–1000 dB |
| 128 px | `RG24` | 28.6–60.2 dB |
| 128 px | `RG64` | 27.4–1000 dB (alpha), 72.5–77.9 dB (RGB) |
| 128 px | `BGRA` | 27.4–1000 dB (alpha), 52.1–53.6 dB (RGB) |
| 128 px | `BYR4` | 21.1–59.3 dB (CFA) |
| 1920×1080 | `RG48` | 42.5–1000 dB |
| 1920×1080 | `RG64` | 41.6–1000 dB (alpha), 72.1–78.2 dB (RGB) |
| 1920×1080 | `BYR4` | 34.9–1000 dB (CFA) |

`1000 dB` is the tool's marker for a mathematically exact match on that channel. The low end comes
from the `low` quality noise/checker patterns and from the constrained `alpha_noise` alpha channel;
FilmScan 3 is substantially higher. These are observations about the pristine SDK, not acceptance
thresholds for HyperCine's own codec.

## 7. Storage and transfer

- Create locally, run `--verify`, then copy to the authorized external corpus location. The
  repository publishes the variable name `HYPERCINE_GOLDEN_DIR`, never its value.
- Keep the NAS copy read-only and versioned by directory; do not overwrite the existing YUV
  corpora from issues #10/#24.
- Re-run `--verify` at the destination.
- Binaries, especially 4K/8K, stay on the external store and in ignored local worktrees; they are
  never committed. Only schema, generator, tests and documentation live in Git.
- The committed helper test `format_corpus_export_helpers` and the CTest smoke export use only
  synthetic packets or a 128 px single case inside the build tree.

## 8. Limits

- No claim that HyperCine's own encoder or decoder implements these modes.
- No visual or production fidelity claim. PSNR is reported only when source and decoded layouts
  describe the same components; otherwise the case says `comparable: false`.
- BAYER hashes are not reproducible across runs (section 4).
- CFA pattern variants and the pre-quantization stage oracle remain
  `PENDING_SDK_CFA_METADATA` / `PENDING_SDK_INTERNAL_STAGE`; no synthetic substitute is claimed.
- The 4K F2 stage is recorded separately in
  [REFERENCE_CINEFORM_FORMAT_F2.md](REFERENCE_CINEFORM_FORMAT_F2.md). 8K, stereo, interlace/GOP2,
  KEYING and uncompressed remain untouched.
