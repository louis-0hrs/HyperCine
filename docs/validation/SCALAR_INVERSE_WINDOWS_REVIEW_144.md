<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# PR 203: native Windows original-stage review, 2026-10-20

**Corrected Windows working-tree validation: 6/6 original stage pairs byte-exact.**
The run authenticated all 30 pinned original artifacts and their sidecars, then
rechecked original artifact bytes after candidate execution. Full Release CTest
passed **31/31**, including `scalar_inverse_original_six_pairs`.

The historical baseline at exact PR source
`a085c43fd1ba63d555997abbef16e1de9f52040b` matched five of six pairs. Its
1080p SDK channel 2 (U) differed by 480 signed coefficients in the final two
240-wide output rows. First difference, zero-based `(x=0,y=268)`: candidate
**4163**, original **4164**. The corrected replay confirms those differences
are gone with the general band0 bottom-endpoint rule.

| Historical baseline | SDK component | Original equality |
| --- | --- | --- |
| 1920 x 1080 | ch0 Y | PASS |
| 1920 x 1080 | ch1 V | PASS |
| 1920 x 1080 | ch2 U | FAIL: 480 coefficients |
| 3840 x 2160 | ch0 Y | PASS |
| 3840 x 2160 | ch1 V | PASS |
| 3840 x 2160 | ch2 U | PASS |

Machine-readable hashes and lineage are in
[`scalar_inverse_windows_review_2026-10-20.json`](scalar_inverse_windows_review_2026-10-20.json).
No reference was regenerated, resealed or changed. No encoder was executed.
At the historical baseline, the requested stock replay exited 1; the corrected
replay passes with exact equality and no tolerance or expected-output change.
Keep PR 203 Draft and issue 144 open.

## Root cause and independent original control

In frozen `upstream/Codec/spatial.c::InvertSpatialQuantOverflowProtected16s`,
the first output pair does not advance `lowlow`. Each interior iteration
reads rows at offsets 0,1,2 from that pointer, then advances it once. Consequently
the bottom-border block starts with `lowlow` at input row **h-2**, not h-1.
Its coefficients `5,4,-1` and `11,-4,1` therefore read rows **h-2,h-3,h-4**.
The lowhigh-band pointer window instead correctly uses h-1,h-2,h-3.
Highpass correction still comes from the final row in both pairs.

The historical PR `vertical()` applied h-1,h-2,h-3 to both lowpass inputs.
The source proposal and original synthetic generator shared that assumption,
so their passing tests could not detect this original-specific pointer behavior.

Two independent controls support this diagnosis:

- A temporary aligned C++ probe links and calls the **actual archived original
  SDK primitive**, without recompiling SDK sources. The unchanged replay harness
  authenticates the same 30 artifacts and obtains **6/6 actual byte equality**.
  This PASS belongs to the original SDK control, not the PR candidate.
- A Python source model changes only the band0 bottom-border anchor to h-2.
  Using the committed independent horizontal operator model, it reproduces the
  original bottom two rows for **all six** pairs. No original output is used as
  a manufactured expected result.

The actual archived SDK library contains the `InvertHorizontalStrip16s` SSE2
path, including signed saturated add/subtract and arithmetic right shifts.
All 22 archived tools and their JSON/SHA sidecars were rehashed. Four original
reference-index files match the proposal's pinned hashes. The 30 selected
original artifacts plus their 60 sidecars were rehashed before and after the
control: **90/90 unchanged**. The original phase-B observer/build sources remain
bound to the archived tool identities; only decoder/wavelet observation copies
were overlaid in that historical build. The spatial primitive used here is
from the pinned pristine SDK archive.

## Correction applied in the existing PR worktree

The arithmetic owner preserved the original band0 bottom-border anchor inside
the HyperCine-owned wrapper, with a deterministic non-constant vertical
regression vector that distinguishes the two anchors. The independent operator
model and source contract now encode the correction. All original artifacts
and hashes remain unchanged; `upstream/` remains frozen.

Height 3 is rejected before writes because the original pointer expression
would address row h-4 = -1. Synthetic operator fixtures now use height 4 or
greater, and a fixture-independent 0,8,16,24 LL ramp distinguishes the
corrected lower endpoint from the former symmetric rule.

No second scalar implementation, production decoder support or merge approval
is added by this review.

## Native execution and reproducibility

Windows x64, MSVC 19.44.35229.0, CMake Visual Studio 17 2022, Python 3.10.9,
C++17 Release. The corrected scalar target rebuilt successfully. The direct
replay reported `ORIGINAL_SIX_PAIR_EQUALITY_PASS` for six cases and 30 original
artifacts. Full Release CTest passed **31/31** in **36.42 seconds**. The focused
arithmetic, rejection, vector-generation and replay-contract tests passed.
FFmpeg/ffprobe are unavailable locally, so external-decode tests were not
registered. This Windows run did not execute sanitizers, GPU tests or benchmarks.

The original corpus remained read-only. Its 30 artifacts and sidecars were
authenticated before candidate execution, and original artifact bytes were
rechecked afterwards. No encoder was run and no expected output was changed.

### Archived original source and dispatch provenance

The phase-B reference manifest binds the SDK to commit
`11574d0295771edccadd17af14af74a539f924c7` and upstream tree
`837528ef9e9a55c416e9592ecb54e1c8f1ff9cf8`. The pristine SDK static library is
2,780,282 bytes with SHA-256
`1609cc9ca056d63e5e562957994ddca49bbbdee34198c05602e2c91b1a858496`, matching
the `original_sdk_collector.lib` entry and sidecar in the authenticated Windows
corpus.

The recorded native Phase-B toolchain is Windows x64, MSVC 19.44.35229, Release.
Its CMake build record uses Visual Studio 17 2022 and x64. The archived
`spatial.c` compile command uses `/std:c11`, `/O2`, and `/TC`, with no `/arch`
override and no `_PROCESSOR_*` command-line definitions. Frozen `config.h`
therefore selects `_PROCESSOR_DISPATCH=0`, `_PROCESSOR_GENERIC=0`,
`_PROCESSOR_PENTIUM_4=1`, and `_XMMOPT=1`: the compiled Pentium-4 SSE2 path,
without runtime CPU dispatch. Archived horizontal disassembly records 10
`paddsw`, 6 `psubsw`, and 8 `psraw` instructions.

The decoder call at `upstream/Codec/decoder.c:13045` invokes the active
`TransformInverseSpatialQuantLowpass` implementation. In
`upstream/Codec/wavelet.c:5770`, scale 0 and level >=4 enter
`InvertSpatialQuantOverflowProtected16s`; its implementation is in
`upstream/Codec/spatial.c:21114` and calls `InvertHorizontalStrip16s`. This
ties the six-pair source path to the archived source revision, x64 Release
library hash and compile-time ISA selection. It does not qualify a different
production SDK binary or the full GOP1/GOP2 decoder.

Machine-readable corrected-run hashes and provenance are in
[`scalar_inverse_endpoint_windows_2026-10-20.json`](scalar_inverse_endpoint_windows_2026-10-20.json).

Private corpus paths and fixture contents are excluded from this portable
report. The original corpus was read-only throughout; `upstream/` remains
unchanged.
