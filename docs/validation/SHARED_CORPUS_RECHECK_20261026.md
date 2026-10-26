<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Shared golden corpus — alternate-endpoint recheck, 2026-10-26

The user supplied an alternate endpoint after the original storage binding
failed. The alternate Windows corpus directory is reachable. Its private address
is not published; commands refer to `HYPERCINE_GOLDEN_DIR` only. All corpus
access was read-only. No original files were generated, copied, moved, overwritten
or deleted. Temporary FFmpeg containers/outputs were confined to the ignored
local build tree and removed afterwards.

This closes the **storage/integrity blocker** for the identified YUV quality,
F1 and FS3 collections. It does not qualify production decoder pixels or add
unmeasured formats to the capability matrix.

## Existing quality corpus

[Machine-readable evidence](windows-qualification-20261026/shared-quality-integrity.json).

The transfer list exactly matches the previously recorded SHA-256
`2343a5efae40590de81cc87b275fce382cc59565f3ae4689f21cc83f48218205`.
Every listed relative path was checked for containment/duplicates, then streamed
and compared for both byte length and SHA-256.

**PASS: 1,760/1,760 files, 5,970,674,259 bytes, 20 subcorpora.** The `_staging`
copy was not treated as another independent reference corpus.

The actual manifest profiles are BGRA8 → progressive YUV422 p10, GOP1, default
Rec.709 conversion. All six regular qualities have 16-frame 1920×1080 collections
and single-frame 3840×2160/7680×4320 collections; FS2/FS3 additionally have
120-frame sequences. The JSON retains per-corpus generator/source commits,
profile, recorded SDK baseline, run status, frame count, verified file count and
byte count. These are original-SDK/CUDA packet references; no missing native
decoded-pixel artifact is inferred from their existence.

## Complete F1 RGB/RGBA/Bayer collections

[Machine-readable evidence](windows-qualification-20261026/shared-f1-integrity.json).
The existing native `format_corpus_export --verify` was run against each complete
collection, using the retained exporter checkout
`f17355f2f391410fc2cf27065e54dcffe1ae45e7`.

| Collection | Cases | Verified manifest artifacts | Verified transfer rows |
| --- | ---: | ---: | ---: |
| 128 px, implemented sources, six qualities | 222 | 889 | 888 |
| 1920×1080 RGB444/RG48, FS1/FS3 | 16 | 65 | 64 |
| 1920×1080 RGBA4444/RG64, FS1/FS3 | 14 | 57 | 56 |
| 1920×1080 Bayer/BYR4, FS1/FS3 | 14 | 57 | 56 |
| **Total** | **266** | **1,068** | **1,064** |

All pass. The four data artifacts per case are source, original SDK packet,
normalized packet and SDK-decoded output: **1,145,792,576 bytes** in total,
excluding manifests/transfer lists. Per-case evidence includes quality,
geometry/stride, layout, source/sample/decoded hashes, recorded API statuses and
SDK version fields. This is a new integrity check of retained encode/decode
evidence, not 266 newly executed native encode/decode calls.

The earlier F1 limits remain: Bayer per-run dither, default CFA only, alpha curve,
missing internal signed-stage references, incomplete exporter source/binary
provenance, unavailable inputs and known hostile 1080p RG24 patterns. No F2
RGB/RGBA/Bayer collection was identified in the bounded alternate-root inventory;
the historical #165 report is not thereby reverified.

## Original FS3 16/120-frame archives

[Machine-readable evidence](windows-qualification-20261026/shared-fs3-integrity.json).
External manifests have identical parsed JSON to the frozen imported repository
manifests, while their raw text differs. ZIP members were streamed
directly without expanding the archives into another multi-GB directory.

| Archive | Hash/length-verified members | 1080p packet pairs parsed | Band pairs equal |
| --- | ---: | ---: | ---: |
| Tier A | 280 | 16 | 480 |
| Sequence | 480 | 120 | 3,600 |

Archive digests and every declared member hash/length pass. The existing
`reference_sample.verify_sample` additionally checks metadata-mask boundaries,
normalized hashes, band geometry, framed payload hashes and three lowpass
coefficient FNV hashes per encoder. All **136** 1080p SDK/CUDA normalized packets
and all **4,080** framed band pairs agree.

Tier A also retains **40 known 128×128 mismatch frames**. Their storage hashes
pass, but they remain excluded from encoder-oracle qualification. A good archive
hash never turns those historical failures into a codec PASS. The sequence's
three extra ZIP metadata members are recorded separately from its 480 declared
data artifacts.

## Bounded independent FFmpeg observations

[Machine-readable evidence](windows-qualification-20261026/shared-f1-ffmpeg.json).
One original 1080p FS3 sample per family was decoded in **three fresh processes**
using `n9.0.1-11-ge47273f4d9-20260831`. All nine calls succeeded; each case produced
one stable output hash. `ffprobe` reported the requested 1920×1080 extent and
12 raw coded bits for these particular packets.

| Bound case | Observed native format | Comparison with archived SDK output |
| --- | --- | --- |
| RGB444/RG48/asymmetric | gbrp12le | Converted to rgb48le; R/G/B PSNR 77.422/75.320/77.282 dB, max differences 16/31/48 |
| RGBA4444/RG64/alpha_ramp | gbrap12le | Converted to rgba64le; SDK b64a explicitly reordered A,R,G,B → R,G,B,A; PSNR 77.422/75.320/75.534/79.945 dB, max differences 16/31/32/27 |
| Bayer/BYR4/detail | bayer_rggb16le | No pixel comparison: no qualified native-Bayer layout/curve adapter |

RGB/RGBA outputs are **not bit-exact** with archived SDK pixels. No decoder is
declared correct merely because the PSNR is high or hashes repeat. These three
observations have no qualified discriminator/negative-control adapter and are
explicitly `oracle_qualified=false`; they do not extend the existing FFmpeg
allowlist or qualify all RGB/RGBA/Bayer patterns, geometries or alpha/CFA modes.

## Reproduction and remaining decisions

Privately bind `HYPERCINE_GOLDEN_DIR` to the Windows corpus directory, then run
the retained F1 exporter separately for `format-f1/128`, `1080p-RGB_444`,
`1080p-RGBA_4444` and `1080p-BAYER` with `--verify` (each full path is below
`format-f1`). For YUV, check the pinned transfer-list hash before streaming all
three-column `relative-path / length / SHA-256` rows. For FS3, verify ZIP digests,
all manifest-bound member bytes and the existing parser contracts.

The endpoint discovery and new integrity evidence supersede the earlier
`BLOCKED_SHARED_CORPUS_STORAGE` observation in the Windows briefing report;
historical local-subset records remain historical. Native CTest counts from that
report are unchanged. Production pixel-oracle review, F2 location/provenance,
CFA/stereo/internal-stage support and integrated-head qualification remain open.
