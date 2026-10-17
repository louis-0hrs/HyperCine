<!-- Copyright (c) 2026 HyperCine Contributors. -->
<!-- Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# RGB444 encoder: P0 source-frame boundary

Issue [#37](https://github.com/louis-0hrs/HyperCine/issues/37) explicitly permits
API/test architecture before the original Windows F1 corpus exists. This slice
implements `hypercine::rgb444::Rgb444Frame`, an owning, immutable source-frame
snapshot. **BITEXACT_PENDING / PENDING_REFERENCE:** there is no RGB444 CFHD
encoder, packet exporter, packed-pixel adapter, quality adapter or decoder in
this change. A successful frame copy never reports codec compatibility.

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

## Reuse and remaining oracle gates

| Component | P0 behavior / next required evidence |
| --- | --- |
| Aligned allocator | Reuse `standalone::AlignedVector`; no SDK or YUV codec linkage |
| `Plane` / `ForwardLevel` | Not applied to these unsigned source planes; signed RGB stage/prescale mapping pending F1 |
| `EncodingQuality` / `QualityState` | Existing calculator is explicitly p10 YUV422; no p12 table or state is fabricated |
| Metadata / writer / Table17 | No RGB packet/header or band framing emitted; qualify reuse against F1 bytes first |
| Internal G,R,B order / 40-band observation | Hypothesis from #17, not verified or encoded by this source API |
| Original SDK / FFmpeg RGB decode | NOT RUN: no RGB packet produced |
| CUDA / Metal / Vulkan | No adapter or kernel introduced |

Current dependency [#36](https://github.com/louis-0hrs/HyperCine/issues/36): F0
capability probe exists in PR #82; F1 RGB source, original SDK sample and native
decoded bytes remain pending. Do not infer F1 from an F0 encode/decode count.
External corpora belong outside Git, privately resolved through
`HYPERCINE_GOLDEN_DIR`.

Next executable encoder step once F1 is supplied: pin source+packet+decoded
hashes and exact layout, verify canonical-to-SDK input mapping, then qualify
signed transform/prescale/channel/band/quant/framing semantics with first-stage
diffs. Only then add a genuine `Encode`/metadata/quality interface and CLI. The
existing p10 writer/36-entry tables must not be assumed valid for RGB p12.

## Tests and integration

The separate `hypercine_rgb444_frame` target is built in SDK-free standalone
mode. It does not link the YUV writer or `hypercine_quality_state`.
`rgb444_frame_storage` and `rgb444_frame_rejection` are registered with CTest
and mandatory in `ci/run_standalone.py`, including the existing Windows/Linux/
macOS standalone matrix. Missing or skipped registrations cannot certify this
slice. Existing fixture files and p10 math are unchanged.

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
