<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# GOP2 signed-stage row-length binding

Issue #189 follow-up, based on representation child #196 at
`4a9f3f116f1d9202c63b997a971dc5b7199d0e7c` (reference parent #190 at
`4bba30eeba106bdc9e96042d02694c026351a624`). No reference export or decoder
implementation is changed.

## Reproduced defect and repair

`verify` rehashed stage artifacts and compared their index/sidecar copies, but
never required those lengths to equal the declared compact pitch times height.
The sealing command already checked original stage lengths; a verification
command must enforce the same invariant independently of how metadata was made.
For example, the published first 1080p luma band declares 480-byte compact rows
and height 135 (64,800 bytes). Consistently changing its observation and stage
height to 136 left the verifier's binding checks satisfied despite requiring
65,280 bytes. Hash agreement alone cannot validate that storage contract.

Two new rejection methods exercise four cases: changed band height with
synchronized metadata, changed signed-plane artifact length, and consistently
shortened comparison lengths for a band and a final plane. Exact parent:
4 rejection failures, 15 methods pass, zero errors (17 methods total). The
child rejects all four. A new positive method also validates full public
bindings for both 1080p and 4K.

The verifier now requires each original artifact and both comparison digest
lengths to equal the referenced stage's compact pitch times height, with
integer byte lengths. Existing byte hashing then binds these declared lengths
to actual files. This protects storage consistency; it does not authenticate
private bytes or the observer's runtime origin. Existing public indices and
private corpus are unchanged.

## Consequence for issue #144

A signed-plane adapter cannot safely infer row storage from matching hashes
without checking geometry against byte length. This child closes that specific
reference-verification prerequisite. It does not select a transform or qualify
inverse arithmetic. The two pinned GOP2 entropy/final-plane references remain
subject to alternate original-SDK path acceptance; they do not become the
first progressive GOP1 spatial-stage oracle by relabelling.

Milestone 2 still needs the explicit #16/#47 architecture decision required by
#144: exact base and owned adapter/test files, selected stage input/output
pair, signed rounding/overflow/boundary contract and aligned plane ownership.
The integrated synthetic graph is reused after that decision; no second
scheduler or production adapter is introduced here.

## Validation

Portable reproduction after the SDK-free Release build with required external
FFmpeg decode enabled (see the parent representation report):

```sh
python3 -O tests/test_gop2_stage_contract.py
python3 -O -B -m unittest discover -s ci -p test_standalone_gate.py
PYTHONOPTIMIZE=1 ctest --test-dir build -C Release --output-on-failure
```

Native macOS 26.5.2 ARM64 / AppleClang 21 / CMake 3.31.5 / optimized
Python 3.9.6 / FFmpeg n8.0: full SDK-free Release CTest **34/34 PASS**,
zero skips, 76.11 seconds, with required external decode. This includes all
four existing 1/2/4/16-worker synthetic graph tests. Stage methods: 17/17
PASS. CI inventory helpers: 8/8 PASS. Method counts overlap the full suite. No original SDK, CUDA, private-corpus,
sanitizer or benchmark execution; no upstream, public API or codec change.
