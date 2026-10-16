# FFmpeg versus GoPro decoder audit — 2026-10-16

Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0; see LICENSE-MIT and LICENSE-APACHE.

Refs #11, #10, #24 and the historical #5/#9 oracle. This is a bounded measurement
deliverable; **#11 remains open, CAUSE_UNKNOWN**. It does not qualify a new decoder
or change production rounding.

## Reproduction and provenance

Use [the fixed CLI and build instructions](../tests/decoder_differential/README.md).
The measured source base is protected main `5c87391`; new audit source hashes are
embedded in [the measured JSON](validation/decoder_differential_20261016.json).
Windows x64, MSVC 19.44.35229, Release; Python 3.10.9; external FFmpeg 7.1,
libavcodec 61.19.100. The complete FFmpeg version/configuration is recorded.
Original SDK baseline: `11574d0295771edccadd17af14af74a539f924c7`.

Nine original SDK packets from the archived #24 corpus were verified against
their manifests: Low/FilmScan2/FilmScan3, 1920x1080, frame indices 0/3/6. One
3840x2160 Low frame 0 adds an independent geometry probe. The other four cases
are existing 128x128 standalone flat/gradient/canonical packets and the original
GoPro-encoded control from `tests/fixtures/windows-oracle/control.cfhd`.
Each packet's identical unmodified bytes feed both decoders. Corpus manifests,
raw packet SHA-256, output hashes and three fresh-process repetitions per decoder
are recorded, with no original media or raw decoded images added to Git.

Both decoders output a common Y/Cb/Cr, 4:2:2, top-down, tightly packed
`yuv422p10le` numerical space. SDK YU64 LE Y0/Cr/Y1/Cb is shifted right six bits;
the unpacker uses the actual padded SDK pitch and performs no matrix conversion.
GoPro metadata: PRCS=65535, CLSY=2, CLSR=1, CLSF=0, CPU_MAX=1. FFmpeg decoder
uses `-threads 1`; FFprobe identifies native p10 and the expected geometry.
FFprobe returns **no range, matrix or chroma-location field** for these packets.
We do not infer those semantics from the missing fields. This is a comparison of
the declared decoder output pipelines, not proof that all internal color handling
is identical.

## Measured results

**14/14 cases decoded successfully; all 84 decoder invocations succeeded.**
Every decoder's three output SHA-256 values match for each case. All four newly
decoded historical SDK p10 outputs also match their existing archived SHA-256.
This establishes fresh-process stability under these controls; it does not prove
unrestricted SDK threading or long-lived decoder-instance stability.

| Packet | Max absolute delta Y/Cb/Cr | Plane PSNR Y/Cb/Cr (dB) |
| --- | --- | --- |
| Low 1080p flat archetype | 3 / 2 / 3 | 80.899 / 82.471 / 83.047 |
| Low 1080p gradient | 3 / 2 / 2 | 66.052 / 61.726 / 65.523 |
| Low 1080p noise | 3 / 3 / 3 | 62.908 / 62.925 / 62.931 |
| FilmScan2 1080p flat archetype | 3 / 2 / 2 | 81.624 / 82.623 / 82.954 |
| FilmScan2 1080p gradient | 2 / 2 / 2 | 65.480 / 62.185 / 65.466 |
| FilmScan2 1080p noise | 3 / 3 / 3 | 62.921 / 62.914 / 62.919 |
| FilmScan3 1080p flat archetype | 3 / 3 / 2 | 81.911 / 82.644 / 82.910 |
| FilmScan3 1080p gradient | 2 / 2 / 2 | 65.480 / 62.185 / 65.466 |
| FilmScan3 1080p noise | 3 / 3 / 3 | 62.921 / 62.914 / 62.919 |
| Low 4K flat archetype | 3 / 2 / 3 | 86.920 / 88.491 / 89.068 |
| Historical 128px flat | 0 / 0 / 0 | exact / exact / exact |
| Historical 128px gradient | 3 / 3 / 3 | 62.746 / 62.849 / 62.490 |
| Historical 128px canonical | 3 / 3 / 2 | 62.804 / 63.179 / 62.603 |
| Historical 128px SDK control | 2 / 3 / 2 | 62.806 / 62.669 / 62.727 |

Delta is **GoPro minus FFmpeg**, peak 1023. No tolerance masks or excluded pixels.
The public JSON contains exact mismatch counts/fractions, mean/max/p95 absolute
error, RMSE, PSNR, signed bias and full signed histograms per plane; first 32
differences; and disjoint border/interior, parity, code-bin and spatial-grid groups.
Coordinates use each decoder's common output plane; chroma x coordinates are
chroma samples, not luma pixels.

FilmScan3 noise gives the clearest spatial control: interior mismatches are
49.412% Y, 49.436% Cb and 49.419% Cr. Mean signed bias over each whole plane is
approximately +0.463 codes; p95 absolute error is one code. All four parity groups
have differences (whole-plane fractions approximately 49.28–49.44%). All 64
normalized spatial cells contain differences. Thus the discrepancy is not confined
to image borders, one pixel parity, or a single localized region. The first Y
difference is (0,0): GoPro=420, FFmpeg=421; Cb (0,0): 527 versus 526;
Cr (1,0): 528 versus 527. Positive aggregate bias does not imply every delta is
positive.

The corpus's `FlatBlack` archetype is **not a mathematically constant frame**:
`GeneratePhase3AFrame` calls `StampFrameIdentityBlocks`, adding identity blocks
and a detail band. Low 1080p and 4K have the same mismatch counts (16,010 Y,
5,726 Cb, 5,119 Cr), with the first differences shifted by 1080 output rows:
Y (12,972) versus (12,2052), Cb (4,976) versus (4,2056), Cr (76,976) versus
(76,2056). This observed localization and the truly constant historical flat
control's exact result should prevent mislabeling the stamped case as failure to
decode a uniform image. The numerical spatial map does not identify a particular
wavelet stage.

## Interpretation and remaining controls

This reproduces the small historical decoder differences with new invocations
and documents their distribution. A bit-identical packet comparison cannot
attribute these deltas to different encoder packet bytes. The current evidence
also does not establish a GoPro or FFmpeg defect. The positive sub-code-scale bias
is compatible with rounding/rescaling differences, but **the first differing IDWT
or output stage has not been measured**, so rounding remains a hypothesis.

Next action: trace the existing minimal historical SDK-control packet through
both reconstruction pipelines, capturing the first differing integer stage with
separate wrappers/instrumentation outside frozen `upstream/`. Reuse its recorded
SHA and original bytes; do not change production math to fit the other decoder.

Pending: native FFmpeg 8 comparison (the other locally available executable is
an older 2023 build); safe alternative SDK CPU limits; explicit range/chroma
controls; higher-precision output-stage separation; highpass coefficient
attribution; common-conversion BGRA analysis; 8K and changing sequence coverage.
No CUDA/Metal/Vulkan execution, speedup, application interoperability or full
resolution-family qualification is claimed.

Validation: full Windows SDK CPU CTest **12/12 PASS**, including five deterministic
Python measurement/provenance cases and the synthetic padded YU64 conversion test.
The separate SDK-free standalone CTest suite passes **29/29**, with mandatory
external FFmpeg decode gates enabled (126.82 seconds). CUDA is disabled in both builds.
