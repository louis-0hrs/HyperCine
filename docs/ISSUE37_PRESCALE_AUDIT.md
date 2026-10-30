<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Issue #37: the p12 prescale mapping, audited

Status: **PRESCALE_MAPPING_AUDITED** / **PENDING_SDK_INTERNAL_STAGE unchanged**.

This note covers one question only: are the prescale entries the RGB444 p12 stage
applies the ones the frozen original encoder installs and emits, and is each
entry bound to the right decomposition? It does not add quantisation, a packet
writer or a bit-exactness claim.

Verifier: `tools/rgb444_p12_prescale_audit.py`, registered as CTest
`rgb444_p12_prescale_audit`. It is fail-closed: a missing sample, a changed
sample hash, an unparsable table, a disagreeing table or a drifted binding all
exit non-zero. There is no skip path.

## What was checked, and what the numbers are

| Axis | Source | Result |
| :--- | :--- | :--- |
| Table the frozen encoder installs for a 12-bit **spatial** transform | `upstream/Codec/wavelet.c`, `SetTransformPrescale`, `CODEC_PRECISION_12BIT` branch | `[0, 2, 2, 0, 0, 0, 0, 0]` |
| Table the same function installs for 10-bit spatial | same function, `CODEC_PRECISION_10BIT` branch | `[0, 2, 0, 0, 0, 0, 0, 0]` |
| Table the frozen encoder actually **emits** in a 12-bit stream | tag 83 of all 24 packets of `upstream/Example/videos/CFHDsample444.AVI`, decoded with the decoder's own `value >> (14 - i*2) & 0x3` | `[0, 2, 2]` in every packet |
| Sequence this repo declares | `src/cpu/rgb444_transform.cpp`, `PrescaleSequenceForPrecision` (read by the verifier, not restated) | p10 `[0, 2, 0]`, p12 `[0, 2, 2]` |
| Entries compared | 24 packets × 3 level entries | **72/72 match**, 0 deviations |
| Transform type of the sample | tag 10 in every packet, against `TRANSFORM_TYPE_SPATIAL` in `upstream/Codec/wavelet.h` | 24/24 SPATIAL (`0`) |
| Sample identity | container SHA-256 | `ce018f56…8c5ea`, pinned in the verifier |

Two independent paths therefore agree: the source declares the p12 table with one
more non-zero entry than p10, and the only frozen p12 bitstream in the tree
carries exactly that table. A p12 stage that prescaled only the second level
would contradict both.

## The index trap, stated instead of hidden

`FinishFrameTransformQuant` (`upstream/Codec/encoder.c:8688`) starts with
`wavelet_index = 0`, then advances it by `num_frames` (which is 1 on this path,
`assert(num_frames == 1)` at line 8716) **before** the spatial loop, and reads
`prescale = transform->prescale[index]` at line 8745 with
`for (index = wavelet_index; index < num_spatial + 1; index++)`.

Read literally, the first spatial decomposition would take entry **1**, not
entry 0 — an apparent one-entry offset. The verifier derives and prints that
offset (`gop1_table_index_offset = 1`) rather than glossing over it, because the
next reader will hit the same line.

The offset is not resolved by reading harder, and this note does not pretend it
was. It is resolved by **measurement**: the merged p10 writer is byte-exact
against the original SDK on 240/240 packets (#318), and it applies the p10 table
`{0, 2, 0}` to decompositions 1, 2, 3 as `0 → 2 → 0`
(`src/core/standalone_encoder.cpp:160-162`). Two readings are possible for why:
either the SDK's loop skips the entry-0 slot that p10 never uses (`0` there, so
skipping it is indistinguishable), or the table is 1-based in intent. Both
readings produce identical p10 output, so the 240/240 result cannot separate
them — and that is exactly why the p12 binding must not be asserted as proven.

For p12 the two readings **do** differ: 0-based gives decompositions
`0 → 2 → 2`, 1-based gives `2 → 2 → 0`. The stage implements the 0-based
reading, which is the convention its p10 sibling demonstrably follows. That is a
defensible choice, not a measurement of the p12 path — the p12 path has no
byte-exact comparison to appeal to. Whoever wires the band table to this stage
must settle it with a p12 packet comparison, not with this document.

## Still missing: the signed pre-quant state

`PENDING_SDK_INTERNAL_STAGE` stays exactly as it was. What is needed, precisely:

1. **The values.** For one p12 RGB frame, the signed 16-bit coefficients of all
   three channels at each of the three spatial levels *after* the forward
   transform and prescale, *before* band quantisation. 3 channels × 10 bands ×
   (level geometry). Without them there is nothing to compare a coefficient
   against.
2. **Where they exist in the frozen code.** They are produced inside
   `TransformForwardSpatial` (`upstream/Codec/wavelet.c:2425`) — the `wavelet`
   argument, whose bands 0..3 are written by `FilterSpatialQuant16s`
   (`upstream/Codec/spatial.c:10026`) on the unscaled path and by
   `FilterSpatialV210Quant16s` (`upstream/Codec/spatial.c:12942`) on the
   prescaled path. The prescaled filter applies the ÷4 rounding per tap
   (`FilterHorizontalRow10bit16s`, `upstream/Codec/spatial.c:3669`, with
   `V210_HORIZONTAL_SHIFT == 2`), which is the arithmetic a p12 comparison has to
   reproduce exactly.
3. **Why this module cannot build them today.** It has no encoder-side dump
   hook, and the quantity that matters is not the pixel data but the post-filter
   coefficients that the frozen filters never expose: they write into
   `wavelet->band[]` and the encoder quantises in place on the next step. A
   dump therefore needs either a build of the frozen encoder with an
   instrumentation point after `TransformForwardSpatial` returns, or a
   reimplementation that is validated against such a dump — not against itself.
4. **Why it cannot be derived from the sample either.** The 24 packets carry the
   *quantised, entropy-coded* bands and the tag-83 table. Recovering pre-quant
   coefficients from them requires inverting the quantiser and the entropy
   coder, and the result would be an inference, not the SDK's own state.

Until one of those exists, `docs/RGB444_P12_TRANSFORM.md` keeps saying
`PENDING_SDK_INTERNAL_STAGE`, and nothing here may be called bit-exact.

## What this audit does not do

- No quantisation, no packet, no writer, no rate control.
- No p12 encode, so no p12 packet parity claim in either direction.
- No claim that the 0-based binding is *proven*; it is the convention the
  byte-exact p10 sibling follows, and the open alternative is stated above.
- No SMPTE VC-5, production or conformance claim.
