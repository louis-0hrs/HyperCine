<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Issue #189 Phase B — independent original SDK stages

**2026-10-19: the two previously missing stage families are now AVAILABLE for
both pinned packets; 180/180 new comparisons are coefficient-bit exact.**
Acceptance of the explicitly identified SDK extraction path and release remain
subject to owner review. No acceptance requirement was waived.

This extends [Phase A](GOP2_ISSUE189.md) in [Draft #190](https://github.com/louis-0hrs/HyperCine/pull/190).
Handoff: [#189](https://github.com/louis-0hrs/HyperCine/issues/189),
[#2](https://github.com/louis-0hrs/HyperCine/issues/2),
[#16](https://github.com/louis-0hrs/HyperCine/issues/16),
[#187](https://github.com/louis-0hrs/HyperCine/pull/187),
[#10](https://github.com/louis-0hrs/HyperCine/issues/10).

| New original oracle | 1920x1080 | 3840x2160 |
| --- | --- | --- |
| Pre-inverse, entropy-decoded coefficient bands | 51 AVAILABLE | 51 AVAILABLE |
| Unclipped final signed planes, frames 0/1 × Y/V/U | 6 AVAILABLE | 6 AVAILABLE |
| Materialized coefficient comparisons across both frame requests | 84/84 exact | 84/84 exact |
| Direct signed device-plane comparisons | 6/6 exact | 6/6 exact |
| New-stage comparison total | 90/90 exact | 90/90 exact |

The packet SHA-256 values are unchanged from Phase A:

- 1080p: `fab22de68bc73eaf99d204a5a569e21540dc346d2e72d1c76de9afe401b858f2` (897,552 bytes).
- 4K: `1ab74a2f6f72009d0216763bd4d2d820a8491d84b198064a1bf58ee1522f3804` (2,930,956 bytes).

No encoder was called. The 164 existing Phase-A indexed artifacts and their
manifests/sidecars were neither regenerated nor changed. The previous 224
comparisons and unchanged CUDA acceptance suites were not rerun.

## Extraction provenance and exact meaning of the stages

Frozen original SDK revision: `11574d0295771edccadd17af14af74a539f924c7`,
upstream tree `837528ef9e9a55c416e9592ecb54e1c8f1ff9cf8`.
`prepare_gop2_stage_sdk.py` verifies that tree and clean upstream contents, then
creates **new private copies only** of `Codec/decoder.c` and `Codec/wavelet.c`
under the ignored build directory. The copies use LF line endings. The two
resulting C objects overlay the matching objects in the pinned pristine SDK
static archive; all other SDK objects/headers are unchanged. The per-source
original/patched hashes, observer source, executable and library hashes are in
each Phase-B index. No instrumented original SDK source is committed to Git.

Only observation calls are inserted; no transform formula, flags, scheduling,
band-validity gate, clipping expression or existing branch is modified.
The harness links the original SDK and has no HyperCine decoder dependency.

The entropy observation is immediately after a successful `DecodeSampleSubband`
and **before inverse submission**. All 17 logical bands in each of three channels
are copied at this point. The first band uses the updated transform type rather
than its pre-initialization local value when identifying FIELDPLUS. This affects
only whether the observer records that first band, never SDK decoding.
These are SDK-decoded signed int16 coefficients, including native dequantization
and the YU64 output-context lowpass bias; they are not unquantized encoder planes.
Every record has channel, logical band, wavelet/band ownership, level, quantization,
prescale, dimensions, original pitch, compact pitch and frame association.

**Signed plane path qualification is explicit.** The default SDK YU64 path uses
the newer `ReconstructSampleFrameYUV422ToBuffer` route and bounded unsigned
row conversion. Merely reinterpreting its output would not produce an unclipped
signed oracle. Instead, after that original reconstruction has completed, the
observer calls the SDK's existing, unchanged `TransformInverseSpatialToV210`
routine on those original wavelets and scratch space. This alternate original
spatial path materializes actual signed int16 row strips. They are copied at
all three original packer call sites **before** `ConvertYUVStripPlanarToV210`.
No plane is reconstructed from candidate output or by a new scalar inverse.

For **every call and both frames**, the alternate path's normally packed YU64
must equal the just-produced default original YU64 byte for byte. Each geometry
passed **18/18** such checks. The default original output must independently
equal the already archived original bytes. This qualifies the named alternate
path for these two packets; it is not a general equivalence theorem for all
SDK inputs, clipping edge cases or output modes. Owner review must retain this
distinction when accepting the stage contract.

The captured full planes are little-endian signed int16, compact, top-down,
nominal p10 video-domain YUV, internal SDK order **Y,V,U**, before p10 range
clipping/output packing. Native signed16 saturation in inverse arithmetic is
retained. These files contain the original routine's values, not unclipping of
YU64, sign inference, extrapolation of clipped values or candidate-derived bytes.

## Stability and instrument neutrality

Native Ryzen 9 7940HS Windows x64, MSVC 19.44.35229, C++17 Release, original SDK
control identical to Phase A: one CPU allowed by process affinity, `TAG_CPU_MAX=1`
and native limit/capabilities 1, entropy pool 0, transform worker pool 1.
The control does not repair or qualify the SDK default-pool race in #160.
Upstream rounding/dither uses the same explicit `0x1337BEEF` seed.

Per geometry, three fresh decoder instances per output format, each with three
same-packet calls, require all default BGRA/YU64 frame bytes to remain exact.
The 51 entropy bands and every row of all six signed planes must be observed
three times per YU64 decoder instance and match across the three instances:
**nine matching observations per band or row**, with complete row coverage.
The callback is confined to this standalone single-decoder process; it never
throws across the C ABI and any callback error rejects the result.

Additional neutrality checks per geometry:

- 12 archived original frame comparisons (three instances × two formats × two frames).
- 198 archived original resident-buffer comparisons (three instances × 66 buffers).
- 18 alternate-original packed YU64 vs default-original exact comparisons.
- Original packet bytes re-read unchanged after every instance.

All passed in 1080p and 4K. The initial rejected probe attempts remain only in
ignored local scratch; only observer version 2's successful, sealed result is
published. Failure paths do not overwrite or qualify partial exports.

## Comparison against the unchanged candidate

The separate candidate checkout remains at original code
`7ed4a82ed97598454c2680bd264dedf259e6e7dc`, with production sources unchanged.
Only the external test target `gop2_stage_compare` is added to its build.
It compares each frame's 42 materialized entropy destinations against the new
pre-inverse original files and downloads the three final signed device planes
directly. SDK Y,V,U maps to candidate device planes Y,U,V via indices 0,2,1.
The signed comparison applies **no clipping, scale adjustment or packing**.

The entropy destination buffers remain unchanged by the candidate inverse:
its bindings use separate reconstruction destinations. This is an observation of
resident entropy destination data after completion, not a claim of a new
candidate pre-inverse instrumentation hook. The SDK side is captured before its
inverse. The other frame's final highpass input bands are explicitly listed as
not materialized for each request; all 51 unique original bands are covered on
their associated frame. No required original band was silently skipped.

Each new comparison records both hashes/lengths. A mismatch records channel,
wavelet/band, logical band, first differing coefficient, row/column and values.
The Python validator independently checks the full 90-case matrix and hashes all
actual and expected files before accepting the recorded equality decisions.
No threshold was introduced or relaxed. Results: **180/180 exact**.

## Artifacts, validation and reproduction

The new safe relative private corpus ID is
`golden/windows/gop2/issue189-20261019-phaseb-r1/`.
Each geometry has `original/` (57 new stage files, standalone JSON/SHA sidecars,
observer report and `phaseb_index.json`) and separately labeled `candidate/`
outputs/comparison report. Actual new tool binaries, matching sources and the
two instrumented C copies are retained privately under `tools/`. No archive or
binary is uploaded to Git. Phase-A storage remains read-only.
The two geometries contain 114 new original stage binaries in 350 original-side
files (166,391,923 bytes including metadata), plus 544 candidate-side files
(186,785,696 bytes): 894 new stage/comparison files, 353,177,619 bytes, excluding
the separately indexed private tools.
Published indices: `gop2_issue189_phaseb_1920x1080_2026-10-19.json` and
`gop2_issue189_phaseb_3840x2160_2026-10-19.json`.

```powershell
# First verify existing references; do not run the encoder/exporter again.
python tools/prepare_gop2_stage_sdk.py --root . --out build/gop2-phaseb-sdk-v2
cmake -S tests/reference_gop2/stages -B build/gop2-phaseb-build -A x64
cmake --build build/gop2-phaseb-build --config Release -j 2
# Only for missing stage exports; NEW_STAGE_DIR must not exist:
build/gop2-phaseb-build/Release/gop2_stage_extract.exe ORIGINAL_REFERENCE_DIR NEW_STAGE_DIR 1920 1080
# Configure the clean pinned candidate as described in Phase A, then:
cmake --build CANDIDATE_BUILD --config Release --target gop2_stage_compare
gop2_stage_compare.exe ORIGINAL_REFERENCE_DIR STAGE_DIR NEW_COMPARISON_DIR 1920 1080
# Seal using explicit --observer/--binary/--source and candidate provenance;
# verify requires the three private directories and needs no native execution:
python tools/gop2_stage_contract.py verify --stages STAGE_DIR `
  --reference ORIGINAL_REFERENCE_DIR --candidate COMPARISON_DIR
```

The validator exits **2** for a completed verified result with owner review
still required, and **1** for absent/invalid/changed evidence. A successful
verification never grants decoder promotion or release approval.

Full isolated native CPU Release CTest after this change: **16/16 PASS, zero
skips**, including 11 negative/shape/binding methods for the new stage contract.
The independent [#191](https://github.com/louis-0hrs/HyperCine/pull/191) verifier
fix (`52cb246`) was reviewed and integrated into the #190 test branch. Its
20 integrity methods pass natively on Windows; the new stage verifier also
binds raw report/index semantics, candidate/geometry identities, original stage
producer/input identity, actual equality and aggregate gate claims. Resealed
contradictory metadata and bool/float identity aliases are rejected.
Both GOP2 CTests are now mandatory in SDK-free standalone CI; the configured
Windows SDK-free GOP2 subset passed **2/2**, and CI inventory helpers **8/8**.
FFmpeg/ffprobe were unavailable, so this is not a rerun of the child's full
macOS external-decode suite. Both actual Phase-A indices (224 stored comparisons)
and Phase-B indices (180) were reverified with the hardened checkers without
running a native decoder or regenerating any reference. The verifier revision
and its source hashes are separately indexed by
`gop2_issue189_verifier_revision_2026-10-19.json`; original tool archives remain
unchanged. These schema tests do not substitute for the native stage run.
No performance claim or unchanged full CUDA-suite rerun is made.

## Handoff

The two Phase-A `MISSING` fields are now historical. The new stage indices mark
both families `AVAILABLE` and the measured stage comparison `PASS` for the pinned
1080p and 4K packets. The reference extraction work is ready for independent
owner review, especially the stated alternate original SDK signed-path contract.
Keep PR #190 Draft and #189 open pending that acceptance; keep #2/#16 promotion,
SDK #160 default-threading and release decisions separate. No phase/main merge,
tag or release follows automatically.
