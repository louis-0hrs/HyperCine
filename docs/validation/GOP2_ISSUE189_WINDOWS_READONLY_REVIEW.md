<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Issue #189: Windows review of pinned reference bytes and representation fix

Reference parent: [#190](https://github.com/louis-0hrs/HyperCine/pull/190)
at `4bba30eeba106bdc9e96042d02694c026351a624`.
Reviewed representation fix: [#196](https://github.com/louis-0hrs/HyperCine/pull/196)
at `4a9f3f116f1d9202c63b997a971dc5b7199d0e7c`.
Candidate code remains `7ed4a82ed97598454c2680bd264dedf259e6e7dc`.

This review reads the existing private collections through the locally bound
`HYPERCINE_GOLDEN_DIR`. No encoder, collector or candidate decoder is run.
The machine-readable result is
[`gop2_issue189_windows_readonly_review_2026-10-19.json`](gop2_issue189_windows_readonly_review_2026-10-19.json).

**Historical disposition: `PINNED_REFERENCE_REVIEW_ACCEPTED_AT_4a9f3f1`.**
This 2026-10-19 review used the representation verifier at the exact #196 head
above; it did not execute #199's later row-length checks. The separate
[2026-10-20 reconciliation](GOP2_REFERENCE_STACK_WINDOWS_RECONCILIATION.md)
records verification of the existing snapshot with #199 applied.
The 164 Phase-A originals and 114 Phase-B original stages
rehash correctly; all 224 Phase-A and 180 Phase-B comparison records verify
with zero mismatches. Acceptance of #190's verifier requires incorporating
#196. The #2/#16 production decision remains separate.

## Review procedure

The two existing collections are read into an ignored local build snapshot,
using regular files only and rejecting reparse points. Every copied file is
hashed during its original read and rehashed after the local write; lengths
must agree. This avoids repeated network path-resolution overhead. Subsequent
verification uses that exact snapshot, not regenerated references. The result
records its file count, byte length and sorted inventory digest; the persistent
corpus binding remains unchanged.

For each geometry, the hardened Phase-A reference/comparison verifier and the
#196 Phase-B verifier check original files, candidate files, standalone JSON
and SHA sidecars, frame/stage coverage and report/index consistency. Private
manifests and both comparison indices must be byte-identical to the published
Git blobs, using `git show HEAD:<path>` to avoid checkout CRLF conversion.
All 180 Phase-B candidate/reference pairs are additionally compared
directly as byte arrays, independently of reported equality flags.

All 22 originally indexed archived tools are rehashed with their sidecars.
Each Phase-B toolchain fingerprint is matched to the actual archived extractor,
comparison executable, sources, observer index, pristine SDK archive and
candidate decoder/wavelet archives. The two observation patches are reproduced
in memory from frozen SDK sources and must equal the archived private copies.
The reviewed exporter/helper/observer sources must equal the archived sources
after the documented line-ending normalization. This checks identities; it does
not claim a new reproducible binary build or a replay of the original runtime.

## Native Windows verification

Windows x64, MSVC 19.44.35229, Python 3.10.9, Release:

- Full CPU CTest: **16/16 PASS**, zero failures/skips, 26.51 seconds.
- SDK-free GOP2 CTest subset: **2/2 PASS**, zero failures/skips.
- Optimized-Python methods: stage contract **14/14**, reference integrity
  **20/20**, CI inventory helpers **8/8**. Counts overlap the CTest entries.
- The three new representation methods against exact parent `4bba30e`
  reproduce **15 expected rejection failures**, zero errors; all pass at #196.
- FFmpeg/ffprobe are unavailable. The full SDK-free external-decode suite,
  unchanged CUDA suites, sanitizers and benchmarks were not rerun.

The representation fix is appropriate: internally consistent unsigned storage,
swapped Y/U/V, bottom-up rows, BT.601/full range or after-clipping capture labels
must not qualify these signed compact Y/V/U oracles. #196 preserves the actual
published indices while rejecting the reproduced incompatible contracts.

Existing validators can reproduce the file checks directly against the bound
corpus (their successful exit code 2 deliberately retains owner review):

```powershell
$a = 'golden/windows/gop2/issue189-20261019-r1'
$b = 'golden/windows/gop2/issue189-20261019-phaseb-r1'
foreach ($g in @('1920x1080', '3840x2160')) {
    $reference = Join-Path $env:HYPERCINE_GOLDEN_DIR "$a/$g"
    python -O -B tools/gop2_reference_inventory.py verify --relative "$a/$g"
    python -O -B tools/gop2_reference_inventory.py verify-comparison `
        --relative "$a/candidate-7ed4a82-r2/$g" --references $reference
    python -O -B tools/gop2_stage_contract.py verify --reference $reference `
        --stages (Join-Path $env:HYPERCINE_GOLDEN_DIR "$b/$g/original") `
        --candidate (Join-Path $env:HYPERCINE_GOLDEN_DIR "$b/$g/candidate")
}
```

## Acceptance boundaries for #2/#13/#16

| Contract | Review disposition |
| --- | --- |
| Original 1080p/4K packets, frame 0/1, named output/resident stages | PASS: 164 original artifacts rehashed, 224 comparison records reverified |
| 51 pre-inverse bands and six signed planes per geometry | PASS at #196: 114 original stages rehashed, 180 pairs directly byte-equal |
| Representation verifier | #196 reviewed positively; integrate this child before accepting #190's checker |
| Alternate original SDK signed path | Scoped source/reference review accepted for these two packets and archived controls only |
| #2/#13/#16 production promotion | Owner decision remains required; this review supplies reference evidence only |
| #160 original SDK default threading | Outside this bounded single-worker reference qualification |

The signed oracle comes from original `TransformInverseSpatialToV210` strips
before the packer, with original signed16 saturation retained. It is not a
signed view of default packed YU64. The archived observation contract requires
18 alternate/default packed-output checks, 12 archived frame checks and 198
resident checks per geometry, with nine observations per band/row. Those are
original-run controls inspected here, not newly executed checks. Packed parity
is a bounded control, not a general theorem about unclipped intermediate values.

Scope remains two temporally asymmetric YUY2 **8-bit source** inputs encoded by
the original SDK into progressive FilmScan3 **10-bit-coded** FIELDPLUS/GOP2,
BT.709 video range, 1920x1080 and 3840x2160, both frames. The SDK-to-candidate
channel mapping is Y/V/U to Y/U/V. The 224 Phase-A and 180 Phase-B comparison
records cover distinct named stages; they do not establish arbitrary format,
source-depth, quality, matrix/range or default-worker correctness.

Keep #189 open and #190 Draft until #196 is integrated and the #2/#16 owner
records the scoped reference decision. This report authorizes no phase/main
merge, promotion, tag or release. Frozen `upstream/`, reference collections,
existing owner branches and production sources remain unchanged.
