<!-- Copyright (c) 2026 HyperCine Contributors. -->
<!-- Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# RGB444 encoder: P0 source-frame boundary

Issue [#37](https://github.com/louis-0hrs/HyperCine/issues/37) explicitly permits
API/test architecture before the original Windows F1 corpus exists. This slice
implements `hypercine::rgb444::Rgb444Frame`, an owning, immutable source-frame
snapshot, and a packed RG48 source importer. **BITEXACT_PENDING:** original F1
source/packet/decoded artifacts are now accessible and pinned; signed internal
stages remain **PENDING_SDK_INTERNAL_STAGE**. There is no RGB444 CFHD encoder,
packet exporter, quality adapter or decoder in this change. A successful source
import never reports codec compatibility.

## Source contract

`include/hypercine/rgb444.h` names source planes **red, green, blue**. Each plane
has the entire width and height; no chroma subsampling or alpha is present.
These are canonical source samples, not a claim about GoPro's internal channel
order or signed representation. Values are right-justified unsigned p12
`0..4095` stored as native-endian `uint16_t`, with top-down rows. No color
transform, range scaling, transfer curve or rounding occurs.

Each caller-owned plane supplies its own byte extent and positive byte stride.
The caller must keep real readable storage alive for the duration of `Copy`.
Addresses and row strides must be 16-byte aligned; strides must contain an
entire active row. The minimum extent is `(height-1)*stride + width*2`: final-row
padding is not required and padding values are not inspected. Read-only input
planes may alias; the returned planes always own separate storage.

`Copy` validates all three spans before any read or allocation. Multiplication,
addition and pointer-offset representability are checked. Every active sample
is checked for p12 range while copying; no clamping or masking silently repairs
invalid data. Every failure throws and RAII destroys any partial snapshot.
Constructing a replacement before assignment preserves an existing frame when
validation or allocation fails. Allocation failure was not fault-injected.

Returned vectors reuse the existing SDK-free 32-byte aligned allocator. Row
strides are rounded to 16 bytes and all destination padding is zero. Access is
read-only via `plane(Channel::Red/Green/Blue)`; invalid channels throw. As with
standard containers, moved-from objects should be assigned before reuse.

Storage is bounded independently of wire-format legality: positive dimensions
up to 4096, at most 2,097,152 active pixels, and at most 16 MiB across three
padded destination planes. 1920x1080 fits. 3840x2160 and larger frames are
rejected. Odd/small dimensions can be stored; this does not assert that they
are legal RGB CFHD encoder inputs. No existing YUV geometry limit is changed.

## Packed RG48 import

`Rgb444Frame::CopyRg48(Rg48View)` accepts packed **R,G,B** native-endian unsigned
16-bit components with top-down rows. It requires a 16-byte-aligned base address
and positive 16-byte-multiple pitch containing `width*6` active bytes. Its
minimum readable extent is `(height-1)*stride_bytes + width*6`; final-row
padding need not exist. Geometry, span overflow and pointer-offset
representability are checked before allocating or reading input. The returned
frame uses the same bounded, separately owned and deterministically padded
storage as planar `Copy`.

Each source component becomes `floor(value / 16)`, dropping the lower four
bits without rounding. This is an explicit reduction from full-range 16-bit
RG48 to the existing unsigned p12 **source** API. It preserves R/G/B labels
and row orientation. It does not perform the codec's G/R/B channel permutation,
signed wavelet mapping, color transform, transfer curve or range adjustment.
All `uint16_t` inputs are legal; unlike planar p12 `Copy`, values above 4095
are expected and reduced. An imported frame cannot recover the discarded bits.

The frozen `upstream/Codec/frame.c::ConvertRGB48ToFrame16s` reference reads
packed R,G,B and shifts by `16-precision` (four for p12), then stores G,R,B
internally. The importer implements only the declared source reduction, keeping
canonical R/G/B. No original SDK internal-stage dump was available to certify
the subsequent codec stages; source-byte replay must not be cited as one.

New C++ tests exhaust all 65,536 input values independently per channel, check
alignment boundaries, tight final rows, padded pitches, asymmetric channels,
zero output padding, source preservation, independent ownership, 1080p and the
storage edge. Malformed views, dimensions, extents, overflow and failed
replacement/retry are checked separately. Allocation-failure injection remains
in the separately reviewed #167/#176 child stack; this update does not absorb
those branches.

## Reuse and remaining oracle gates

| Component | P0 behavior / next required evidence |
| --- | --- |
| Aligned allocator | Reuse `standalone::AlignedVector`; no SDK or YUV codec linkage |
| `Plane` / `ForwardLevel` | Not applied to these unsigned source planes; signed RGB stage/prescale mapping pending F1 |
| `EncodingQuality` / `QualityState` | Existing calculator is explicitly p10 YUV422; no p12 table or state is fabricated |
| Metadata / writer / Table17 | No RGB packet/header or band framing emitted; qualify reuse against F1 bytes first |
| Internal G,R,B order / 40-band observation | Hypothesis from #17, not verified or encoded by this source API |
| Original SDK RGB output artifacts | F1 stored bytes verified by size/SHA; no new SDK decode invocation |
| FFmpeg RGB decode | NOT RUN: no RGB packet produced |
| CUDA / Metal / Vulkan | No adapter or kernel introduced |

Current dependency [#36](https://github.com/louis-0hrs/HyperCine/issues/36): F1
exporter PR #156 at `f17355f2f391410fc2cf27065e54dcffe1ae45e7` supplied real
RGB source, pristine SDK packet and native decoded bytes. The stored F1 snapshot
is pinned in `tests/reference_rgb444/rg48_f1_pins.json` and replayed with
`tests/validate_rgb444_rg48.py`. It covers all eight RG48 patterns at 128x128
and 1920x1080, FilmScan1 and FilmScan3: 32 cases, 128 artifacts and two original
manifest files. The replay checks exact sizes/SHA before invoking the importer,
compares every imported sample to integer division of the source value, and
checks three pinned canonical-plane hashes per case. Outputs have explicit
tight little-endian R/G/B plane domains. Each invocation requires a fresh
output file. Regression tests reject corrupt artifacts, missing/stale output,
wrong output extents/digests and malformed source geometry.

`REFERENCE_ARTIFACTS_PINNED / SOURCE_IMPORT_PASS` is the source boundary's
proof scope. The stored decoded bytes are lossy output evidence from the F1
exporter, not expected source planes and not a new macOS SDK-decoder run.
Internal-stage and bitstream equality remain pending. Do not infer either
from verified source or packet hashes.
External corpora belong outside Git, privately resolved through
`HYPERCINE_GOLDEN_DIR`.

Next encoder step: obtain qualified signed internal RGB stage dumps and then
qualify transform/prescale/channel/band/quant/framing semantics with first-stage
diffs. The F1 exporter explicitly reports `PENDING_SDK_INTERNAL_STAGE`; its
source/packet/decoded files do not contain those planes. Only then add a genuine
`Encode`/metadata/quality interface and CLI. The
existing p10 writer/36-entry tables must not be assumed valid for RGB p12.

One part of that list is no longer open: the **prescale** shape of the p12
spatial transform is measured from the original bitstream rather than inferred.
All 24 packets of the frozen RGB444 sample declare `CODEC_TAG_PRESCALE_TABLE`
`[0, 2, 2, 0, …]` at precision 12, against `[0, 2, 0, …]` for the 10-bit table in
`upstream/Codec/wavelet.c`. The stage that applies it is in
[RGB444_P12_TRANSFORM.md](RGB444_P12_TRANSFORM.md), together with the level
binding and its four mutants. The remaining semantics — transform, channel
mapping beyond the source-derived G/R/B order, band, quant and framing — are
still `PENDING_SDK_INTERNAL_STAGE`, so the signed-stage oracle is still the gate
before a packet writer.

## Tests and integration

The separate `hypercine_rgb444_frame` target is built in SDK-free standalone
mode. It does not link the YUV writer or `hypercine_quality_state`.
`rgb444_frame_storage` and `rgb444_frame_rejection` are registered with CTest
along with `rgb444_rg48_import`, `rgb444_rg48_rejection` and
`rgb444_rg48_replay_contract` and `rgb444_rg48_allocation_failure`;
all six are mandatory in `ci/run_standalone.py`,
including the existing Windows/Linux/
macOS standalone matrix. Missing or skipped registrations cannot certify this
slice. Existing fixture files and p10 math are unchanged.

For the optional actual F1 replay, configure
`-DHYPERCINE_RGB444_F1_ROOT="$HYPERCINE_GOLDEN_DIR/golden/windows/format-f1"`.
This adds `rgb444_rg48_f1_sources` to full CTest. The directory must contain
the existing loose `128` and `1080p-RGB_444` snapshots; no copy, archive,
regeneration or corpus overwrite is required. A configured missing or altered
snapshot fails. Hosted runners without the private corpus still execute the
exhaustive importer and replay-contract tests; they do not certify F1 replay.

Tests use deterministic asymmetric channels seeded with `0x1337BEEF`, min/max
samples, widths on both sides of alignment boundaries, top/down distinctions,
different per-channel pitches, exact last-row extents, guard sentinels and
invalid padding values. They verify every active output sample, full plane
dimensions, alignment, zero padding, source preservation and independent
ownership, including 1920x1080 and the storage-budget edge. Rejection tests
cover all three channels, truncated/null/misaligned storage, invalid strides,
arithmetic limits, p12 overflow in the last active sample, unchanged existing
frame after rejection, and successful retry.

Portable reproduction and measured native evidence are in
`docs/validation/rgb444-frame-p0/`. Parent #37 remains open and partially
implemented; this API/storage slice does not satisfy its packet-export DoD.
RG48 continuation evidence is in `docs/validation/rgb444-rg48-source/`.
The later RG48 allocation-failure qualification is recorded in
`docs/validation/RGB444_RG48_ALLOCATION.md`; the earlier evidence directories
retain their original dated scope and counts.

## p12 band table (measured, #37)

`include/hypercine/rgb444_quality.h` and `src/core/rgb444_quality.cpp` supply the
format-specific 12-bit band table. It is a table/report surface only: no
transform, no coefficient quantization, no packet writer.

The table is pinned from 112 original packets covering all six regular profiles
(16 each at 128x128 plus 8 each of filmscan1/filmscan3 at 1920x1080). Each
profile yields exactly one table, identical across the three planes at levels 2
and 3, with level-1 chroma at twice green. The shared p10 YUV calculator cannot
produce these values: it rejects `encoded_precision = 12` outright, and at its
own precision its luma table differs from every measured RGB444 profile. A test
asserts both facts so the separation cannot silently collapse.

Only the initial-limiter tables are measured. Other limiter values come from a
transcription of `upstream/Codec/quantize.c::QuantizationSetQuality()` and are
labelled as transcribed, not measured, because the corpus holds first frames
only. Measurement, corrections to the earlier band-contract prose and the
mutation probes are in `docs/validation/rgb444-p12-quality/`.

`PENDING_SDK_INTERNAL_STAGE` is unchanged: the signed pre-quant stage still has
no qualified dumps, so this table removes a blocker for the encoder math without
supplying any of it.
