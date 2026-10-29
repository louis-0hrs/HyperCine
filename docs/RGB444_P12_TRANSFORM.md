<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# RGB444 p12: the transformation stage

Status: **TRANSFORM_STAGE_PRESENT** / **PRESCALE_MEASURED** /
**PENDING_SDK_INTERNAL_STAGE unchanged** (see [RGB444_ENCODER.md](RGB444_ENCODER.md)).

This adds the level that #37 was missing between the pinned band table (#317) and
a packet writer: an actual signed spatial decomposition of a p12 RGB 4:4:4 frame.
It does not add an encoder. No CFHD packet, no metadata, no rate control and no
pixel-parity claim is produced here.

## What was measured, not assumed

The RGB444 p12 family is not the merged p10 YUV writer with a different table.
Two things differ before any band is quantized, and both are decided by the
*precision* rather than by the profile:

1. Every channel is full resolution — there is no luma/chroma split.
2. The SDK's prescale table for spatial transforms has a different shape at 12
   bits than at 10 bits.

The second point is the reason this stage exists, and it was read out of the
original bitstream rather than reasoned from source. `tools/rgb444_prescale_probe.py`
decodes `CODEC_TAG_PRESCALE_TABLE` (tag 83) with the same shift the frozen decoder
uses (`value >> (14 - i*2) & 0x3`) from all 24 compressed packets of
`upstream/Example/videos/CFHDsample444.AVI` (SHA-256 `ce018f56…8c5ea`):

```
#00 … #23   precision=12   raw=10240   table=[0, 2, 2, 0, 0, 0, 0, 0]
```

Every packet declares the same table. The 10-bit spatial table in the frozen
source is `{0, 2, 0, 0, …}` (`upstream/Codec/wavelet.c`, `SetTransformPrescale`).
So p12 carries **one more non-zero entry** than p10 does.

## How the extra entry is bound to a level

The frozen encoder stores the table in a per-wavelet array and reads the entry for
each spatial level when it decomposes that level
(`upstream/Codec/encoder.c`, `FinishFrameTransformQuant`:
`prescale = transform->prescale[index]`). The table itself does not name levels;
the array index does. The frozen tables are indexed from entry 1 for the first
spatial level, and the merged 10-bit writer — which is byte-exact against the
original SDK on 240/240 packets (#318) — prescales the second pyramid level only.
That validated convention is what binds the p12 table:

| precision | level 1 | level 2 | level 3 |
| :--- | :--- | :--- | :--- |
| 10-bit | 0 | 2 | 0 |
| 12-bit | 0 | 2 | 2 |

The last row is the stage this change adds. It is a measured declaration plus a
validated convention — **not** a decoded original frame. `PENDING_SDK_INTERNAL_STAGE`
therefore stays exactly as it was: no qualified dump of the SDK's signed pre-quant
stage exists, so nothing here may be called bit-exact.

## What the code does

`include/hypercine/rgb444_transform.h` and `src/cpu/rgb444_transform.cpp`:

- `PrescaleSequenceForPrecision(10|12, out)` returns the level-ordered sequence and
  returns **false** for any other precision. It never defaults to the p10 sequence;
  a silently reused 10-bit transform with a p12 table on top is precisely the defect
  this stage exists to prevent.
- `TransformRgb444P12(frame)` decomposes one canonical p12 frame into three
  full-resolution three-level pyramids, in stream order G/R/B, and records the
  prescale it applied plus a `transformed` completion flag.
- The source samples are taken as-is. The original RG48 encode path stores
  `value >> (16 - precision)` and removes no offset
  (`upstream/Codec/frame.c`, `ConvertRGB48ToFrame16s`), so a 0..4095 sample stays a
  positive signed value. Centring the data would be a different transform.

## Evidence

- **115/115** standalone CTests pass, 0 failed, 0 skipped (`g++-13`, Release,
  `HYPERCINE_BUILD_STANDALONE=ON`).
- `rgb444_p12_transform_contract` is registered (test #24 of 115) and added to the
  `ci/run_standalone.py` required set, so losing it fails the gate rather than
  quietly shrinking it.
- **Four mutants killed**, each rebuilt and re-run:
  1. p12 sequence collapsed to the p10 one → *"level 3 must be prescaled at p12"*;
  2. correct prescale record but level 2 decomposed unscaled → *"the pyramid must
     equal an independent decomposition with the recorded prescale"*;
  3. channel order red/green/blue instead of the stream's G/R/B → same assertion;
  4. 8-bit accepted instead of rejected → *"8-bit must be rejected, not defaulted"*.

Mutant 2 is the one that matters for review quality: without the independent
re-decomposition, a stage that recorded the right table and then transformed with
zeroes would have passed every other assertion.

## What this does not do

- No encoder, no packet, no `Encode` entry point, no CLI.
- No band quantization, no framing, no metadata, no rate state.
- No bit-exactness, production or SMPTE claim.
- The p12 band table from #317 is unchanged and still consumed nowhere by this
  stage; wiring the two together is the next step, and it needs the signed
  internal-stage oracle to be worth anything.
