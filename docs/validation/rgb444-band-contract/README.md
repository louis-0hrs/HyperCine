<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Measured band contract of original RGB444 p12 packets (#37)

Status: **MEASURED** — declarations read from original bytes. This is not an
encoder, a transform model, a decoder, or a bitstream-equality claim.

Issue [#37](https://github.com/louis-0hrs/HyperCine/issues/37) is blocked on
`PENDING_SDK_INTERNAL_STAGE`: without pre-quant stage dumps the encoder's signed
wavelet/quant arithmetic cannot be qualified. What *can* be settled from bytes
that already exist is the band contract the original SDK actually wrote. That
is what this measurement records, and it corrects one assumption the encoder
design had been carrying.

## What was measured

`tests/reference_formats/rgb444_band_contract.py` walks each original `sdk.cfhd`
and reads, per channel, per wavelet level and per spatial orientation: band
width and height, `num_levels`, `wavelet_number`, subband index, `B_ENCODING`,
`B_QUANT`, `B_SCALE`, coding flags, payload byte length and payload SHA-256. It
also reads the sample header: `encoded_format`, `precision`, `num_channels`,
`num_wavelets`, `num_subbands`, `quality_l` and the prescale tag.

It does not entropy-decode, inverse-transform, reconstruct pixels or link
`upstream/`.

## Corpus verification

The F1 corpus was verified against its own transfer index **before** any packet
was read, per the import rule that an unverified index cannot vouch for files:

| Index | Entries | Verified | Problems |
| --- | --- | --- | --- |
| `1080p-RGB_444/TRANSFER_SHA256.tsv` | 64 | 64 | 0 |
| `128/TRANSFER_SHA256.tsv` | 888 | 888 | 0 |

Every entry was checked for byte length and SHA-256. The corpus stayed
read-only; nothing was written into it.

## Findings

### 1. Thirty bands, not forty

Every measured packet declares `num_subbands = 10`, so each channel carries one
lowpass band plus nine highpass bands, and a frame carries
`3 × 10 = 30` bands. The "40-band" figure recorded as a hypothesis from #17 is
not what these packets contain, and the encoder must not be built against it.

### 2. All three planes are full resolution

For every channel, level 1 is 960×540, level 2 is 480×270 and level 3 is 240×135
at 1920×1080. Each level halves both dimensions, and all three channels have
identical band geometry — the signature of three full-resolution planes rather
than a luma/chroma split.

### 3. `scale` matches the YUV per-level/orientation table

For all 16 measured 1080p packets the declared `B_SCALE` values are exactly
`2/2/1` (level 3), `8/8/4` (level 2) and `32/32/16` (level 1) for
LH/HL/HH. That is the same scale shape the YUV path uses.

### 4. `quant` does **not** match the YUV profile tables

This is the finding that matters for #37's encoder work. Measured green-plane
`B_QUANT`, flat pattern:

| Level / orientation | FilmScan1 | FilmScan3 |
| --- | --- | --- |
| L3 LH / HL / HH | 24 / 24 / 12 | 16 / 16 / 8 |
| L2 LH / HL / HH | 24 / 24 / 12 | 16 / 16 / 8 |
| L1 LH / HL / HH | 96 / 96 / 144 | 24 / 24 / 36 |

The Chroma planes (R, B) at level 1 carry three times the green value in both
profiles (filmScan1: 192/192/288; filmScan3: 48/48/72). So the RGB444 quality
mathematics is its own table, not the YUV 36-entry table with a different
channel mapping. Reusing the p10 YUV quantizers for RGB p12 would be wrong at
every level.

### 5. The two families present are exactly the two that were pinned

The 1080p corpus contains eight `filmscan1` and eight `filmscan3` packets, and
all 16 satisfy the RGB_444 intra p12 contract.

## Rejection behaviour is part of the contract

The 128 corpus contains three families. 222 packets were examined: 96 measured,
126 refused, and every refusal states the actual reason:

| Family | Packets | Reason |
| --- | --- | --- |
| `RGBA_4444` | 84 | declares channel 3; `RGB_444` has three planes |
| `BAYER` | 42 | declares channel 3; `RGB_444` has three planes |

A tool that measures the three-plane RGB_444 family must reject the other
families by name rather than index past its own domain. The first version of
this tool raised `IndexError` on those packets; that is fixed, and the
rejection reasons above are the observed output.

## What this does and does not settle

| Question | State |
| --- | --- |
| Band count and geometry of original RGB444 p12 | MEASURED |
| Declared per-band quant/scale for FilmScan1 and FilmScan3 | MEASURED |
| `scale` equals the YUV shape | MEASURED |
| `quant` differs from the YUV tables | MEASURED |
| Signed internal pre-quant stage | PENDING_SDK_INTERNAL_STAGE |
| Transform, prescale and channel-permutation arithmetic | NOT MEASURED |
| Encoder packet equality | PENDING — no RGB444 encoder exists |
| Decoder pixel parity | NOT CLAIMED |

The values here are *what the encoder chose*, not *why*. Reading `B_QUANT` does
not reveal how coefficients were produced, so this cannot substitute for the
internal-stage oracle.

## Next action

Use the per-profile `quant`/`scale` values above as the target of the RGB444
quality adapter — a format-specific 12-bit table, not the shared p10 YUV one —
and keep `PENDING_SDK_INTERNAL_STAGE` open until qualified pre-quant dumps
arrive.
