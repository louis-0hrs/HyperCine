# FFmpeg versus GoPro decoder audit — 2026-10-18

Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0; see LICENSE-MIT and LICENSE-APACHE.

Refs #11, #10, #24 and the historical #5/#9 oracle. This is a bounded measurement
deliverable; **#11 remains open, CAUSE_UNKNOWN**. It does not qualify a new decoder
or change production rounding.

The [2026-10-16 measurement](#historical-measurement--2026-10-16) remains in
[`decoder_differential_20261016.json`](validation/decoder_differential_20261016.json).
The current run is a separate, immutable file,
[`decoder_differential_20261018.json`](validation/decoder_differential_20261018.json).

## Reproduction and provenance

Use [the fixed CLI and build instructions](../tests/decoder_differential/README.md).
The measured source base is protected main `e094682` plus the audit changes on
this branch; audit source SHA-256 values are embedded in the JSON.

Windows x64 guest VM on AMD EPYC 7601, 8 physical cores / 8 logical threads,
**AVX2 only**; MSVC 19.44.35229.0 Release; Python 3.12.10; external FFmpeg
9.0.2 (libavcodec 63.1.102, libavformat 63.1.102, libswscale 10.1.102), built
with GCC 16.2.0 by gyan.dev. The complete FFmpeg configuration is recorded in
the JSON. CUDA is not linked and no GPU work is involved. Original SDK baseline:
`11574d0295771edccadd17af14af74a539f924c7`.

The corrected private six-quality corpus (#24) is read through the locally
configured `HYPERCINE_GOLDEN_DIR`; no storage location or local path is
published. The corpus now uses a tiered layout
(`<quality>/<tier>/manifest.json`); the audit also accepts the legacy
`<quality>/manifest.json`. Every packet is verified against both its frame
record and artifact SHA-256/byte length before decoding. Thirteen inventory
packets and all 120 sequence packets passed that check.

## Inventory and controls

- Nine 1080p packets: Low/FilmScan2/FilmScan3, frame indices 0/3/6.
- Two 4K and two 8K packets: Low and FilmScan3, frame index 0.
- One stateful FilmScan3 sequence: one decoder instance across all 120 archived
  packets; frames 0/17/34/51/68/85/102/119 are compared.
- Four historical 128px packets: standalone flat/gradient/canonical and the
  original GoPro-encoded control.

Each inventory packet was decoded three times by each decoder in fresh
processes; the sequence arm was decoded twice, and each sampled frame was also
decoded independently in a fresh process. All 17 inventory cases and all eight
sampled sequence frames were stable for both decoders. All three sampled
geometries (1080p/4K/8K) have a contemporaneous range-control arm.

Both decoders output a common Y/Cb/Cr, 4:2:2, top-down, tightly packed
`yuv422p10le` numerical space. SDK YU64 LE Y0/Cr/Y1/Cb is shifted right six bits;
the unpacker uses the actual padded SDK pitch and performs no matrix conversion.
GoPro metadata: PRCS=65535, CLSY=2, CLSR=1, CLSF=0, CPU_MAX=1. FFmpeg uses
`-threads 1`; FFprobe identifies native p10 and the expected geometry. FFprobe
returns **no range, matrix or chroma-location field** for these packets; those
semantics are not inferred from missing fields. This is a comparison of the
declared decoder output pipelines, not proof that all internal color handling
is identical.

## p10 results

Delta is **GoPro minus FFmpeg**, peak 1023. No tolerance masks or excluded pixels.

| Packet | Max absolute delta Y/Cb/Cr | Plane PSNR Y/Cb/Cr (dB) |
| --- | --- | --- |
| Low 1080p flat archetype | 3 / 2 / 3 | 80.90 / 82.47 / 83.05 |
| Low 1080p gradient | 3 / 2 / 2 | 66.05 / 61.73 / 65.52 |
| Low 1080p noise | 3 / 3 / 3 | 62.91 / 62.93 / 62.93 |
| FilmScan2 1080p flat archetype | 3 / 2 / 2 | 81.62 / 82.62 / 82.95 |
| FilmScan2 1080p gradient | 2 / 2 / 2 | 65.48 / 62.18 / 65.47 |
| FilmScan2 1080p noise | 3 / 3 / 3 | 62.92 / 62.91 / 62.92 |
| FilmScan3 1080p flat archetype | 3 / 3 / 2 | 81.91 / 82.64 / 82.91 |
| FilmScan3 1080p gradient | 2 / 2 / 2 | 65.48 / 62.18 / 65.47 |
| FilmScan3 1080p noise | 3 / 3 / 3 | 62.92 / 62.91 / 62.92 |
| Low 4K flat archetype | 3 / 2 / 3 | 86.92 / 88.49 / 89.07 |
| FilmScan3 4K flat archetype | 3 / 3 / 2 | 87.93 / 88.67 / 88.93 |
| Low 8K flat archetype | 3 / 2 / 3 | 92.94 / 94.51 / 95.09 |
| FilmScan3 8K flat archetype | 3 / 3 / 2 | 93.95 / 94.69 / 94.95 |
| Historical 128px flat | 0 / 0 / 0 | exact / exact / exact |
| Historical 128px gradient | 3 / 3 / 3 | 62.75 / 62.85 / 62.49 |
| Historical 128px canonical | 3 / 3 / 2 | 62.80 / 63.18 / 62.60 |
| Historical 128px SDK control | 2 / 3 / 2 | 62.81 / 62.67 / 62.73 |

The 1080p/4K/8K and historical results reproduce the previous run's measured
bound and extend it to both new geometries. The 8K PSNR is higher because the
stamped mismatch region is a smaller fraction of a larger frame, not because
the arithmetic becomes more exact.

Every plane's mismatch fraction, mean/max/p95 absolute error, RMSE, PSNR,
signed bias and full signed histogram are in the JSON, together with the first
32 differences and border/interior, parity, reference-code-bin and normalized
8x8 spatial groups. Planes above 8,000,000 samples (4K/8K) keep exhaustive
overall, border/interior and parity statistics plus first differences, but omit
the per-sample code-bin and spatial groups; `groups_omitted` records that
reduction. It is not labeled complete.

## Stateful 120-frame sequence

One decoder instance called `PrepareToDecode` on frame 0 and `DecodeSample` for
every archived packet in order. FFmpeg decoded the same multi-frame AVI and a
`select` filter emitted the same indices. Both arms were repeated.

| Frame | Pattern | Max absolute delta Y/Cb/Cr | Plane PSNR Y/Cb/Cr (dB) | Stateful = independent |
| --- | --- | --- | --- | --- |
| 0 | FlatBlack | 3 / 3 / 2 | 81.91 / 82.64 / 82.91 | yes |
| 17 | FlatWhite | 2 / 2 / 2 | 81.35 / 82.57 / 82.60 | yes |
| 34 | HighDetailPhotographicSynthetic | 3 / 3 / 3 | 62.91 / 63.70 / 63.90 | yes |
| 51 | HighFrequencyPattern | 2 / 2 / 2 | 64.45 / 66.21 / 66.21 | yes |
| 68 | HardEdges | 2 / 3 / 2 | 65.81 / 64.67 / 65.02 | yes |
| 85 | LowDetailPhotographicSynthetic | 2 / 2 / 3 | 63.63 / 65.06 / 68.74 | yes |
| 102 | FlatWhite | 2 / 2 / 2 | 81.11 / 82.85 / 82.62 | yes |
| 119 | FlatBlack | 2 / 2 / 2 | 81.58 / 82.82 / 82.53 | yes |

For every sampled frame, the stateful SDK hash equals the fresh-process
single-packet decode of the same archived bytes, and both decoder hashes are
stable across repeats. This covers packet order and one long-lived decoder
instance, but it does not model rate-control state beyond the archived packet
order and does not claim sequence-level bit-exactness outside the sampled
frames.

## Explicit range controls

On each sampled geometry the baseline FFmpeg output was compared with two
explicit scale arms.

| Packet | Baseline equals `full→full` | Baseline equals `limited→full` |
| --- | --- | --- |
| Low 1080p flat | yes | no |
| Low 4K flat | yes | no |
| Low 8K flat | yes | no |

Thus the measured baseline is byte-identical to a no-op full-range scale, and an
explicit limited-range interpretation changes the bytes. This verifies the
measurement pipeline's range handling; it is not a decoder-parity claim.

## BGRA common-conversion arm

The SDK was also asked for `CFHD_PIXEL_FORMAT_BGRA` with the same metadata.
The **SDK BGRA buffer is bottom-up** relative to FFmpeg's top-down BGRA: the
audit samples both orientations and flips the SDK rows when that reduces the
sampled mean absolute difference. On all three BGRA cases the selected
orientation was `flipped_to_top_down` (sampled means 15.9/30.5/62.4 top-down
versus 0.6/1.3/1.4 flipped). This is a measured output-pipeline property, not a
codec-arithmetic conclusion.

After the orientation step, FFmpeg converted p10 to BGRA with an explicit
BT.709 matrix. Each channel B/G/R is reported; alpha is exact in every arm.

| Packet | Arm | Max B/G/R | PSNR B/G/R (dB) |
| --- | --- | --- | --- |
| Low 1080p flat | BT.709 limited→full | 4 / 4 / 3 | 47.89 / 62.14 / 47.91 |
| Low 1080p flat | BT.709 full→full | 20 / 19 / 20 | 25.88 / 25.24 / 25.88 |
| FilmScan3 1080p gradient | BT.709 limited→full | 4 / 4 / 3 | 45.87 / 43.93 / 45.91 |
| FilmScan3 1080p gradient | BT.709 full→full | 19 / 19 / 19 | 30.97 / 31.92 / 30.09 |
| FilmScan3 1080p noise | BT.709 limited→full | 5 / 4 / 5 | 44.94 / 43.87 / 45.28 |
| FilmScan3 1080p noise | BT.709 full→full | 19 / 19 / 19 | 31.91 / 32.18 / 31.88 |

The limited-range arm is much closer than the full-range arm on every case;
the SDK's effective BGRA range policy under these tags is therefore consistent
with a limited-range YUV interpretation. The mean signed bias is about +1 code
for limited→full. This does **not** identify which converter is correct and does
not establish decoder parity: the SDK cgRGB path and swscale use different
post-decode policies.

The neighbor and default-bilinear arms produced **byte-identical** output in
this FFmpeg build, both with and without an explicit 4:2:2→4:4:4 intermediate.
The chroma-upsampling step therefore remains **not separated** by this control;
a separate resampler or conversion implementation would be required. The
report records the arm hashes so this limitation is verifiable.

## Interpretation and remaining controls

This reproduces the small historical decoder differences with new invocations,
two additional geometries, a long-lived sequence instance and explicit
conversion controls. The p10 delta remains bounded at 2–3/1023 with a small
positive bias, but the current evidence still does not establish a GoPro or
FFmpeg defect. **The first differing IDWT or output stage has not been
measured**; rounding remains a hypothesis. First-stage tracing stays with the
source-forensics lane and must not be inferred from these output statistics.

Pending: first-IDWT-stage trace on the minimal historical control; FFmpeg
version matrix beyond 9.0.2; safe alternative SDK CPU limits; a working
chroma-upsampling separation; 8K full-detail code-bin/spatial groups; and
sequence coverage beyond the archived 120-frame corpus. No
CUDA/Metal/Vulkan execution, speedup, application interoperability or full
resolution-family qualification is claimed.

## Validation

The Python metrics suite covers packet provenance, endianness/bounds, p10 and
8-bit peaks, per-plane grouping, reduced-detail omission semantics, BGRA channel
metrics and orientation detection, multi-frame AVI headers, the FFmpeg select
expression and tiered manifest discovery. The C++ self-test covers padded YU64
unpacking and padded BGRA tightening. Exact native test counts and the full
suite commands are reported with the branch handoff; this document does not
convert a hosted or source-only check into a native PASS.

## Historical measurement — 2026-10-16

The earlier run measured 14 packets (nine 1080p quality/pattern cases, one 4K
probe and four historical 128px cases) with FFmpeg 7.1. It reported a maximum
absolute delta of 3/1023, all repeat hashes stable, and the same
`CAUSE_UNKNOWN` classification. Its complete machine-readable evidence remains
in [`decoder_differential_20261016.json`](validation/decoder_differential_20261016.json)
and is not overwritten by the current run.
