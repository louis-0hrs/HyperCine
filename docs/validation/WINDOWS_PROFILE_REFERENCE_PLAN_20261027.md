<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Windows profile/reference test plan — 2026-10-27

This prepares W3/W4 acceptance for the separate profile owner. It adds no
profile implementation, changes no encoder selection and does not qualify a
production pixel oracle. The audited source baseline is
`44827e84f1658d67a506ecdd41d8d1c7ba7379c8`.

## Original-reference availability

All six regular qualities have retained original-SDK BGRA8 -> progressive
YUV422 p10 GOP1 references: 16 ordered 1080p frames and one 4K/8K frame each;
FilmScan2/3 additionally have 120-frame 1080p sequences. Historical complete
read-only integrity evidence is in
[the shared-corpus recheck](SHARED_CORPUS_RECHECK_20261026.md).
An 8K golden does not establish that the current SDK-free session accepts 8K.

Today the bound private root is reachable in native execution. Two initially
specified relative candidates were absent; the already-known relative segment
`golden/windows/qualities/2026-10-15` contains the FilmScan2 collections.
No root address, machine/user name or original media is published here.
The current manifests match the previously pinned digests:

| FilmScan2 collection | Frames | Manifest SHA-256 |
| --- | ---: | --- |
| `1080p-16f` | 16 | `d7044bb986e91e2dc4dcc109e5908e5de6197b606790d8b6fedbd0df948c20cf` |
| `sequence-120f` | 120 | `6ed7907589ab7e52fec6d588ba667b6d062cc9928316c1ba05ad64feac9d2792` |

Both record clean exporter source `26ee3e1ad155f9c6db09e809b3dfd4557677b200`,
unchanged SDK baseline `11574d0295771edccadd17af14af74a539f924c7`, deterministic
seed `0x1337BEEF`, BGRA8 top-down source, 7680-byte pitch, Rec.709 default,
quality enum 5, 10-bit coded YUV422 and GOP1. The portable state corpus is
qualified at its recorded 30 fps; arbitrary rates require separate references.
FilmScan2 starts at limiter 8, rather than FilmScan3's 4. Manifests bind source,
raw SDK packet and normalized packet hashes/lengths, all 36 quant/scale entries,
previous completed sample size and 30 band/coefficient/payload records per frame.

The transfer-list digest was freshly read and matches the pinned value below.
Today's bounded artifact check streamed exactly the first/last source, raw SDK
packet and normalized SDK packet in each collection: **12/12 length/SHA checks
PASS**. No media was copied. All 136 manifest rows have ordered frame indexes,
36 quantizers/scales, 30 band records and previous sample size matching the
preceding SDK packet length (zero initially). Manifest-recorded normalized
equality is 16/16 and 120/120; limiter transitions are 5 and 74 respectively.
These are fresh manifest/selected-byte checks, not a new full-corpus replay or
newly executed encode/decode calls.

| Collection/frame | Input SHA-256 | Original SDK packet SHA-256 |
| --- | --- | --- |
| 16f / 0 | `760ea8d69feb7f0d9bc12a2ebcc0d4ed037deea9c85391e289021b4ce4bd74b9` | `714b33c07efcfa473eda81326743159a630d80b9922f40ee9752b67132b7fd36` |
| 16f / 15 | `23625baa230e1cfacd541b35db625da798600a0d0a1f07b9fcf198087858c4c5` | `a2f47c6d8d90a3e2a72401d757df70c2c8c6f4f1da2ca6f6a03d74da9c6ef8e7` |
| 120f / 0 | `760ea8d69feb7f0d9bc12a2ebcc0d4ed037deea9c85391e289021b4ce4bd74b9` | `7ba5cb8ce63552e79c8a5c1d396799d007dee952ce006752cc9c3656cac271ed` |
| 120f / 119 | `9233ec025b568aec474214231b827ecc8d2d9ef8b832087868b8c2c8cf6fbc5b` | `feaa4b1216ab4573905dfa2a6d5d8103ea02c44364d23d959b1c248398da3bca` |

Both frame-zero normalized SDK packets have SHA-256
`b243933d685ae59667d418aa219b949d48a31e8357b358a95131769b2b0d9094`,
despite differing original raw packet hashes. This is why normalization masks
and raw provenance remain separate contracts.

## W3: one FilmScan2 adapter, separate owner lane

Reuse conversion, frontend/wavelet, scalar quantization, entropy and the existing
CFHD writer/session machinery. Reuse `CalculateQualityState` for the profile
differences; do not clone a second full encoder or silently substitute FS3's
rate transitions. The existing SDK-backed Phase3A six-profile path already
exists and is not this new SDK-free adapter.

The adapter's acceptance must separately execute the following contracts:

1. Before using expectations, verify the pinned transfer list SHA
   `2343a5efae40590de81cc87b275fce382cc59565f3ae4689f21cc83f48218205`, manifest
   hashes, complete artifact inventory and every used artifact length/hash.
   Wrong source/manifest/packet bytes must fail before encoding/comparison.
2. CPU first: replay all 16 frames and the independent 120-frame trajectory in
   order. Compare each complete sample, every quant/scale entry and every
   available quantized-band coefficient/payload fingerprint. Compare packet
   lengths, frame counters, limiter and previous completed packet size. An
   unavailable raw coefficient dump remains explicitly missing evidence;
   a fingerprint cannot be relabeled a raw element-by-element comparison.
3. Original variable GUID/DATE/TIME/TIMC metadata may only be compared through
   the existing parser-declared mask contract or exactly injected known values.
   Compare every remaining byte, including profile/framing metadata; never
   mask arbitrary differences or compare only a prefix. Preserve raw hashes
   separately from normalized-packet equality.
4. Reset after a multi-frame sequence must reproduce the fresh profile's
   initial state and packet with identical explicit metadata. Wrong profile,
   stale previous size, swapped/dropped frame, modified quantizer, altered
   final packet byte and absent coefficient hashes must make their own gate
   fail. Include invalid buffer/stride, capacity failure and failed-encode
   rollback followed by equality with a fresh session.
5. Only after CPU/reference acceptance, run the same source frames and checks
   on native CUDA. CPU==CUDA is a separate result from original-SDK equality.
   Missing GPU, CPU fallback or SKIP77 is not CUDA acceptance.
6. Independently decode the relevant original and candidate packets with an
   identified FFmpeg binary; record exact input/output hashes, format/extent,
   exit status and genuinely discriminating wrong-packet/byte-mutation
   controls. Structural decodability is distinct from qualified pixel parity.
7. Original SDK pixel repeatability is its own SHA-bound bounded fresh-process
   gate, using the Windows protocol. An unstable original decode blocks pixel
   oracle claims without relabeling correct packet/state comparisons as
   encoder failure. No best-of-three or retry-until-pass selection.

The independent evidence states remain PASS / FAIL / BLOCKED / INCONCLUSIVE;
an unavailable reference must never be manufactured from the adapter under test.
No FilmScan2 implementation or native owner-branch integration is accepted by
this plan alone.

## W4: RGB444/RG48 p12 preparation

Retained F1 references include 16 canonical RG48 1080p cases at FilmScan1/3,
plus the small-format matrix. Integrity is distinct from decoder correctness;
the bounded RGB FFmpeg observations in the shared-corpus recheck are explicitly
not a qualified pixel oracle. RGB444 is a separate format contract from YUV422.

Specify RG48 as packed R,G,B uint16 storage with top-down rows; coded p12 is
not 16-bit coding. Historical Windows exports use native-endian components.
New cross-platform prepared vectors must declare a serialized endianness and
verify it explicitly. Do not import RG24's bottom-up B,G,R8 interpretation.
Require checked extents and stride arithmetic, 16-byte alignment/pitch padding,
RAII buffers, deterministic asymmetric primaries/minmax/gradient frames and
redzone/padding preservation. Negative tests must reject null/short buffers,
negative or undersized stride, arithmetic overflow, unsupported precision and
unsupported geometry before codec/GPU work. Preserve individual channel values
and validate source row orientation before evaluating any codec loss.

Do not claim full-resolution-chroma or signed internal coefficient semantics
from the existing YUV422 calculator; its contract rejects that domain. Complete
GPU RGB444, new CFA semantics and a second codec engine remain outside this
preparatory lane until matching original stage/packet references are accepted.
