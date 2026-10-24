<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Original Descale source and buffer review for #214

Research preparation, 2026-10-21, base main
`b51e5e58b6c53b19e92827a93c9ea40a7d6bed0f`.
The [six-case selection](validation/NEXT_INVERSE_STAGE_CONTRACT_144.json)
and its implementation gate remain unchanged. No new inverse arithmetic or
production decoder integration is introduced.

## Source findings

Read directly from the frozen source, including the large `spatial.c` that
could not be independently read during #214's earlier connector review:

| Question | Source evidence | Finding |
| --- | --- | --- |
| Spatial destination | `decoder.c:13060–13113` | FIELDPLUS index4 writes wavelet2 band0, doubling both dimensions. |
| Selected primitive | `wavelet.c:5751–5771` | scale>1, p10 inverse prescale selects `InvertSpatialQuantDescale16s`, descale2. |
| Default ISA selection | `config.h:50–51,104–117` | `_XMMOPT=1`, dispatch0, generic0, Pentium4=1 unless overridden. These defaults do not authenticate archived build flags. |
| Vertical interior | `spatial.c:22644–22825` | Active `#if 1` sign-extends signed16 to 32-bit SSE2 lanes, rounds/shifts in 32 bits, then packs with signed saturation. |
| Bottom anchor | `spatial.c:22602–22613,23020–23090` | LL remains one row behind during interior calculation; the additional XMM increment before bottom synthesis advances to h-1. This differs from scale0. |
| Horizontal stores | `InvertHorizontalStrip16s.c:1694–2162` | Aligned Pentium4 path uses signed16 saturating filter operations and doubling; first pair enters lanes as low16, final remainder doubles after extraction. Tail uses wide int accumulators and descale shift. |

The prepared probe extracts the two complete original functions and original
`AllocScratchBuffer` by exact signature from hash-authenticated files, retaining
the original GoPro license. It normalizes source line endings only. Original
saturation, rounding and alignment macros are extracted from pinned headers;
`buffer.h` is copied unchanged into external scratch. The harness supplies
`PIXEL=int16_t`, the two-int ROI, and explicit branch constants matching the
selection. There is no SSE emulation or replacement scalar kernel. The source
SHA emitted by preparation fingerprints this generated translation unit.

The helper compiles these functions as C++17, not the archived MSVC translation
unit. Compiler-defined signed shifts/casts and original negative left shifts
remain original behavior; cross-compilation does not resolve their portability
or qualify the historical compiler. An eventual own implementation must use
explicitly defined arithmetic as specified by #214.

## Resident buffer lifetime

The export in `tools/gop2_reference_export.cpp:140–180` copies wavelet4 LL and
wavelet2 band0 **after** the first `CFHD_DecodeSample` returns, **before** frame1
reconstruction and later repeat decodes. The copy owns compact bytes, not a
borrowed pointer. LH/HL/HH come from the separate pre-inverse phase-B observer.

The immediate next temporal consumer (`decoder.c:13170–13264`,
`wavelet.c:2333–2357`, Pentium4 `temporal.c:9402–9697`) passes wavelet2 low/high
as inputs and writes frame0/frame1 band0. Its stores target even/odd outputs;
it does not intentionally rewrite temporal input. This supports the proposed
resident-output interpretation, conditional on separate allocations and the
actual branch used. Spatial consumption likewise reads wavelet4 while writing
wavelet2. `GetWaveletThreadSafe` (`decoder.c:23294`) can reallocate the selected
wavelet, so this is not a general proof against aliasing, later reuse or races.

**Still required:** archived reference-owner confirmation of build flags,
selected ISA, allocation identities and observer ordering. Six successful
original-function replays would test whether the recorded inputs reconstruct
the resident outputs, but would not independently prove the entire decoder's
lifetime/scheduling correctness or every capture's execution history. Do not
reuse first-stage acceptance as this evidence.

## Bounded reproducible probe

The host must have the unchanged checkout and the existing loose corpus bound
to `HYPERCINE_GOLDEN_DIR`. Configure `HYPERCINE_REVIEW_SCRATCH` as a new external
scratch directory (must not already exist). Do not publish its value.

```sh
python3 tools/replay_original_descale.py --prepare-dir "$HYPERCINE_REVIEW_SCRATCH"
c++ -std=c++17 -msse2 -O2 -I "$HYPERCINE_REVIEW_SCRATCH" \
  tools/original_descale_probe.cpp -o "$HYPERCINE_REVIEW_SCRATCH/probe"
python3 tools/replay_original_descale.py --authenticate \
  --probe "$HYPERCINE_REVIEW_SCRATCH/probe"
```

Use a native x86_64 compiler/host. Windows requires an equivalent C++17 SSE2
build; native MSVC qualification has not run. Record compiler/version/flags,
probe checksum and generated-source checksum with the sanitized result.
No SDK library, encoder invocation or original reference regeneration occurs.
Preparation without `--authenticate` is public metadata/source inspection only.

Before executing the probe, the driver authenticates all 30 actual files,
JSON and SHA sidecars, packet identities, stage identities, compact dimensions
and lengths. It chooses phase-A for resident LL/output and phase-B for the
three highpass inputs. It sends copies through binary stdin; the probe owns
32-byte-aligned, guarded input/output/scratch buffers, checks input immutability
and guards, and returns compact signed16 bytes through stdout. Comparison uses
actual original output bytes, with per-case hashes, mismatch counts and first
signed `(x,y,actual,expected)` difference. Original bytes and sidecars are
rechecked after execution, including probe errors/timeouts. Missing bytes,
invalid source, incorrect output extent or any mismatch fail; no skip becomes
an original-equality pass. Temporary source/binaries remain outside Git.

## Actual evidence and handoff

- Public metadata/source audit: PASS, six cases, 30 bindings, four indices,
  seven selection source files plus four support/lifetime source hashes.
- Native macOS ARM64 Release required-FFmpeg full CTest: **43/43 PASS**,
  zero skips, 78.46 seconds.
- Six guard tests: PASS. Synthetic storage and mocked mismatches validate only
  the harness; they do not count as original numeric comparisons.
- AppleClang21 `-arch x86_64 -std=c++17 -msse2 -O2`: probe compiled successfully.
  Generated original-source SHA-256:
  `241d2dd1591f63793e74428e40e4e4448b52af289a61f47d69f976dae743a8dd`.
- Original private authentication: **30/30 artifacts and sidecars PASS** in
  the subsequent read-only mounted-corpus check, superseding the first
  unavailable-corpus result. Exact lengths, SHA-256, JSON stage/producer/
  representation and packet bindings, AVAILABLE status and SHA sidecars
  matched the selection at `08e23935b9880dc9d9d01374a55347a07f649eb3`.
  The canonical mount was unavailable again during publication recovery;
  this records the completed check, not a new authentication in that recovery.
  No files exported, moved, modified or resealed.
- Original SSE2 numeric replay: NOT_RUN. Native ARM64 host has no executable
  x86 runtime; cross-compiled executable is not a native execution result.
- Archived Windows compiler/ISA and allocation-owner acceptance: PENDING.
- SDK/GPU runs, sanitizers and performance benchmarks: NOT_RUN.

Publication follow-up: GitHub browser authentication restored; the isolated
checkout was recovered from its existing Git commit after temporary files
were removed. The six Python guard tests passed again on unchanged tool code.
The 43/43 full-suite and x86 cross-compile results above are the earlier native
runs in this session; full build products were not retained through recovery.

Next action: preferably the Windows reference owner (or a native Linux x86_64
owner, with separate Windows provenance signoff) with the existing corpus runs
this same-head original-function probe, reports six actual signed comparisons,
and confirms archived build/buffer provenance. A separate own-arithmetic Draft
can follow review of that evidence. #144/#47 remain open.


## Windows checkout checksum correction (#220)

Windows hosted CI exposed two harness failures before numeric execution:
checked-out source/index text bytes can differ from the pinned committed blobs.
The regression reproduces this with an actual Git `core.autocrlf=true` checkout,
verifies CRLF-only differences, and confirms the original pins still match the
canonical blobs. Source/support/index authentication now uses binary
`git --no-replace-objects cat-file blob <resolved-HEAD>:<validated-relative-id>`
with `GIT_NO_REPLACE_OBJECTS=1`. Paths reject traversal, absolute and ambiguous
spellings; arguments are passed without a shell. Missing or mismatching blobs
remain fatal. Regression controls commit source and index mutations and require
failure, then restore the original bytes and require success.

Original function extraction and the copied support header use committed bytes
as well. Prepared source is written as explicit UTF-8 bytes so its reported
SHA-256 identifies the actual file on Windows too. The private corpus continues
to use exact file bytes and unchanged sidecar/packet/output pins. These harness
checks provide no new original SSE2 numeric or archived Windows build signoff.
