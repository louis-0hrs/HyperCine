<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# RGB444 p12 band table: measured for all six profiles (#37)

Status: **MEASURED for the initial limiter / TRANSCRIBED for other limiter
values / PENDING_SDK_INTERNAL_STAGE for the pre-quant stage.**

This closes the "next action" recorded when the band contract was merged (#316):
derive the format-specific 12-bit quality table from original bytes and stop
treating the shared p10 YUV tables as a candidate. It does not implement an
encoder and does not claim bitstream equality.

## What was measured

`tests/reference_formats/test_rgb444_p12_quality.py` re-measures every
`sdk.cfhd` under a configured F1 root and asserts the result equals the pinned
table. Both corpora were verified against their own `TRANSFER_SHA256.tsv`
**before** any packet was read: `1080p-RGB_444` 64/64 entries and `128` 888/888
entries, 0 problems, read-only throughout.

112 RGB_444 packets were measured and produced exactly **six distinct tables**,
one per regular profile — no packet disagreed with its profile, and no profile
carried two variants:

| Corpus | Profiles | Packets |
| --- | --- | --- |
| `128/RGB_444/f1/128x128/RG48` | low, medium, high, filmscan1, filmscan2, filmscan3 | 16 each = 96 |
| `1080p-RGB_444/.../1920x1080/RG48` | filmscan1, filmscan3 | 8 each = 16 |

## The measured table

Per plane, in stream order: level 3 LH/HL/HH, level 2 LH/HL/HH, level 1 LH/HL/HH.

| Profile | Green | Red = Blue |
| --- | --- | --- |
| low | 64 64 48 · 64 64 48 · 512 512 768 | 64 64 48 · 64 64 48 · 1024 1024 1536 |
| medium | 48 48 32 · 48 48 32 · 256 256 384 | 48 48 32 · 48 48 32 · 512 512 768 |
| high | 32 32 24 · 32 32 24 · 128 128 192 | 32 32 24 · 32 32 24 · 256 256 384 |
| filmscan1 | 24 24 12 · 24 24 12 · 96 96 144 | 24 24 12 · 24 24 12 · 192 192 288 |
| filmscan2 | 16 16 8 · 16 16 8 · 32 32 48 | 16 16 8 · 16 16 8 · 64 64 96 |
| filmscan3 | 16 16 8 · 16 16 8 · 24 24 36 | 16 16 8 · 16 16 8 · 48 48 72 |

`B_SCALE` is `32/32/16`, `8/8/4`, `2/2/1` for all six profiles and all three
planes, and the lowpass declares quantization 1 with no `B_SCALE` tag at all.

## Two corrections to the merged #316 report

Both were found while building this table, by comparing the measurement against
the derivation rather than against the prose.

1. **Level-1 chroma is twice green, not three times.** The merged README stated
   "the Chroma planes (R, B) at level 1 carry three times the green value". The
   bytes say `2x` for every profile (`filmscan3` 24/24/36 green versus 48/48/72
   red). The figure was wrong in the report; the raw tag values in
   `docs/validation/rgb444-band-contract/report.json` were right.
2. **Levels 2 and 3 are identical across all three planes.** The "3x at level 1"
   wording implied a general luma/chroma split. There is none above level 1: the
   three planes carry the same values there, which is the signature of
   full-resolution chroma with a single finest-level chroma gain.

The p10 YUV calculator cannot be reused regardless, and the test now proves it:
`CalculateQualityState()` rejects `encoded_precision = 12` outright, and at its
own 10-bit precision its luma table differs from the measured RGB444 table for
every one of the six profiles.

## How it was derived, and what that part is worth

`src/core/rgb444_quality.cpp` is a transcription of
`upstream/Codec/quantize.c::QuantizationSetQuality()` restricted to the branches
these packets exercise: progressive, GOP 1, frame 0, full-resolution chroma,
precision 12, `rgb_quality` 2. It reproduces all six tables exactly.

The distinction that matters: **only the initial-limiter values are measured.**
The corpus contains first frames only, so the limiter-dependent part of the
arithmetic is unverified at every other limiter value. It is the transcribed
formula, and the code says so at the constant it lives in. `filmscan2` starts at
limiter 8 and `filmscan3` at 4; the other four start at 0.

Mutation probes confirm the pin is load-bearing. Each of the following was
applied to the implementation and made the C++ test fail, then reverted:
`chromagain` 8→6, initial limiter 4→0, initial limiter 8→4, applying the declared
scale at level 1, dropping the p12 `*4` on slots 4..6, dropping the GOP-1 copy
7..9 ← 11..13, filling the non-green planes from the luma table, and a one-step
change to any single pinned slot.

## What this settles and what it does not

| Question | State |
| --- | --- |
| Declared band quant/scale per profile and plane at the initial limiter | MEASURED (112 packets) |
| The p12 table is not the p10 YUV table | PROVEN (calculation and test) |
| Limiter dependence away from the initial value | TRANSCRIBED, NOT MEASURED |
| Signed pre-quant stage, transform, prescale | PENDING_SDK_INTERNAL_STAGE |
| Band framing and payload/entropy writer | NOT IMPLEMENTED |
| Encoder packet equality | NOT CLAIMED — no RGB444 encoder exists |
| Decoder pixel parity | NOT CLAIMED |

The values here are *what the encoder chose*, not *why*. They remove the table
question from the blocker list; they do not touch the internal-stage gate that
still blocks the encoder math.

## Next action

With the table fixed, the remaining encoder-facing work is the part these bytes
cannot answer: qualified signed pre-quant dumps from the F1 reference lane. Until
those exist, `PENDING_SDK_INTERNAL_STAGE` stays open and no RGB444 encoder claim
is made.
