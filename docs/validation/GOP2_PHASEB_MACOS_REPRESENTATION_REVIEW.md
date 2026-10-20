<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Phase-B representation contract and independent macOS review

Reviewed reference parent: PR #190 at
`4bba30eeba106bdc9e96042d02694c026351a624`. This child repairs the
reference verifier only. Issue #189 and the four decoder architecture/review
issues remain open; no production decoder or frozen upstream change.

## Reproduced verifier defect

The stage verifier compared observation/index copies to each other without
checking the actual representation contract. Consistently relabelled copies
passed even with unsigned storage, Y/U/V instead of Y/V/U, bottom-up rows,
BT.601/full range, or a capture after clipping. Per-artifact representation
labels were also unchecked. Hashes do not make contradictory metadata valid.

Three new regression methods exercise 15 deterministic invalid contracts:
seven per-artifact labels, six observation-level labels and two capture
boundaries. Exact parent: 15 rejection failures, 11 existing methods pass,
zero errors. Corrected child: all 14 methods pass with optimized Python.
Require the existing signed little-endian compact rows, top-down Y/V/U,
BT.709 video range, retained native signed16 saturation, and the two exact
original SDK capture points. Valid public indices remain unchanged.
The fix validates declared semantics; it cannot authenticate a producer or
prove private bytes were really captured at the declared point.

Cross-host review sources: #180 `e45b26cf2943d1e8e9f9975d4baea35c63b5e03b`
and #192 `a6a22ad1f1e46eab128f47d28a5ee25061f16f06`.

## Five bounded issue results

- **#189:** regenerated both observer source patches from the frozen tree in
  an ignored private build directory. The complete generated observer index
  exactly matches the published index, including both original/instrumented
  source hashes and lengths. An independent metadata audit made 774 assertions
  over both geometries: three source fingerprints each, 114 original-stage
  lengths/representations, and 180 comparison reference/equality/channel
  bindings. No private binary was read or regenerated. The verifier defect
  above requires this child before accepting representation verification.
- **#160:** independently recomputed the new Windows #192 360-call ledger:
  120 unique calls per arm; corruption O=4, B=0, AP=1, all return codes zero,
  no child timeouts, all outputs valid. The #180 review verifier reproduced
  those counts, 60/60 matched B waits, three published helper hashes and its
  five self-test expectations. Windows protocol methods: 14/14 pass locally.
  This is analysis of existing measurements, not native Windows execution.
- **#161:** the older WINDOWS_UNVERIFIED and missing-Phase-B-oracles board
  statements are superseded by source-pinned owner evidence. However, the
  registered six band-0 publications are not met: independent ledger counts
  are 9..12 in every arm (O: 9/10/11/12 counts 5/37/69/9; B: 8/19/50/43;
  AP: 9/34/65/12). The existing #180 review already discloses this gap.
  Preserve the bounded admission-window evidence while requiring an explicit
  protocol correction; do not label all preregistered coverage satisfied.
  The polling reader adds no portable acquire/release guarantee and is not a
  production fix. No new sweep is needed to review these facts.
- **#144:** the two previously missing original GOP2 stage families now have
  available published indices, pending acceptance of the alternate original
  signed path. The source still restricts the integrated graph to Synthetic;
  it neither owns typed planes nor implements inverse arithmetic. A GOP2
  entropy-stage/final-plane pair is not a selected progressive GOP1 spatial
  kernel input/output pair. Next is owner selection of exact stage, base,
  typed aligned storage and signed rounding/boundary contract; no second DAG.
- **#47:** P7.A unsigned planar p10 remains distinct from these signed Y/V/U
  compact rows and from packed YU64. The 15 new representation rejections
  protect that distinction in the reference workflow. Newly available GOP2
  originals do not replace the matched progressive GOP1 packet/stage oracle
  required by the first Mac CPU route. P7.B still awaits #13/#16 acceptance
  and the owner-selected adapter; no decoder facade or implementation added.

## Actual new native validation

macOS 26.5.2 ARM64, AppleClang 21.0.0, CMake 3.31.5, Python 3.9.6,
FFmpeg n8.0. Full SDK-free Release CTest: **34/34 PASS**, zero skips,
76.01 seconds, optimized Python and required external decode. All four
synthetic graph worker tests and both GOP2 integrity suites are included.
CI inventory helpers: **8/8 PASS**. Counts overlap the full suite.
No original SDK, GPU, private-corpus, sanitizer or benchmark execution.
Raw build/test logs remain outside Git. Existing owner branches, reference
indices, corpus bytes and frozen upstream are unchanged.

Portable reproduction (FFmpeg and ffprobe on PATH):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_BENCHMARKS=OFF \
  -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --parallel 4
PYTHONOPTIMIZE=1 python3 ci/run_standalone.py --build-dir build \
  --system Darwin --machine arm64
python3 -O tests/test_gop2_stage_contract.py
python3 -O -B -m unittest discover -s ci -p test_standalone_gate.py
```

Next: review this child into #190, resolve #192 publication-coverage wording
against its existing ledger, then select the scalar-stage contract. No
protected-main merge, release or multi-phase issue closure is authorized by
these scoped results.
