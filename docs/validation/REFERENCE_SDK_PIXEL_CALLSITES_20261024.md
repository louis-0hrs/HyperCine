<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Remaining original-SDK pixel callsites — audit and determinism matrix (2026-10-24)

Status: test-only evidence. Issue: #120 (contract items 1 and 4).
Base: protected `main` `cabdf7097e50d3d666acefe4d7a1a3866b33bc8e`.
Branch: `codex/issue120-sdk-pixel-audit-20261024`.

The merged fail-closed stack (#206–#210, #211) quarantined `test_phase2a`–`test_phase2d`
only. This note enumerates **every other** site where the frozen original GoPro SDK
decodes a packet, states what each site asserts, and separates strict
encoder/bitstream gates from signals derived from reference-decoder pixels.
No gate is changed here and no oracle is newly qualified.

## Method

- Read-only source inspection at `cabdf70` of every `CFHD_DecodeSample` call in
  `tests/`, `src/core/` and `benchmarks/`.
- Each site is classified by what it does with the decoded pixels, not by what it
  prints.
- One CPU-only Windows x64 host (EPYC-class guest VM), MSVC 19.44.35229.0,
  CMake 3.31.12, FFmpeg 9.0.2. No GPU, no private corpus, no `upstream/` change.

## Inventory

| # | Site | Decoded input | What the site asserts | Classification |
| --- | --- | --- | --- | --- |
| 1 | `tests/test_phase2a.cpp:125` | synthetic host/device packet | encoded-byte/coefficient/entropy identity and geometry strictly; pixels through `tests/reference_oracle.h` | **Aligned** — `SKIP_RETURN_CODE 77`, `reference-oracle` label (`tests/CMakeLists.txt:127`) |
| 2 | `tests/test_phase2b.cpp:215`, `test_phase2c`, `test_phase2d` via `src/core/phase2c_encoder.cpp:300`, `phase2d_encoder.cpp:251` | same class | same class | **Aligned** — `tests/CMakeLists.txt:135`–`153` |
| 3 | `tests/test_standalone_sdk_oracle.cpp:56`, gate at `:213`–`:224` | three pinned 128×128 packets, five fresh instances per format | storage/decode/geometry/pitch/non-zero strictly; decoded pixels through `tests/reference_oracle.h` | **Aligned** — `SKIP_RETURN_CODE 77`, `reference-oracle` label (`tests/CMakeLists.txt:212`–`215`); the pinned `.p10` comparison is recorded, never gated (follow-up below) |
| 4 | `tests/test_filmscan3_windows_decoder.cpp:35`, gate at `:46` | two Mac-produced 1080p packets, two decodes each | output identical across two decodes, else thrown as failure; output non-zero | **Not aligned** — instability is detected but mapped to FAIL, not to a blocked gate |
| 5 | `tests/test_phase3a.cpp:147`–`154` (CUDA-only) via `src/core/phase3a_encoder.cpp:3455`–`3463` and `:3438` | every frame of a 300-frame encoded sequence | per-frame `PSNR >= 35 dB` and a frame stamp read out of decoded pixels | **Not aligned** — reference-dependent strict gate; the CUDA lane (#16/#187) owns its registration (`tests/CMakeLists.txt:171`) |
| 6 | `tests/test_yuy2_stability.cpp:118` | one encoded sample, eight decodes | **encoder** masked-bitstream identity (`:142`–`154`); pixel hashes recorded only (`:178`–`193`) | **Aligned** — encoder-strict, pixels diagnostic |
| 7 | `tests/test_codec_roundtrip.cpp:116` | seven patterns | roundtrip success, bitstream hash; PSNR printed only (`:191`–`194`) | **Aligned** — pixels diagnostic |
| 8 | `tests/test_entropy_parallel.cpp:56` (CUDA-only) | one sample per variant | decode status only | **Aligned** — no pixel assertion |
| 9 | `tests/test_avi_sample.cpp:93` | upstream sample AVI, all frames | decode status only; per-frame hashes printed | **Aligned** — status-only |
| 10 | `benchmarks/benchmark_codec.cpp:280,289`, `benchmark_phase1c.cpp:212`, `benchmark_phase3a1.cpp` | measurement inputs | not registered as CTest release gates | Out of release scope |
| 11 | `src/core/phase3a_encoder.cpp:757`–`769` | — | mutates an ABI-private decoder prefix to force one worker before decoding | Flagged under the #144 boundary ("no ABI-dependent private decoder access as a production solution"); unchanged here |

Strict gates at every site — decode return code, geometry, pitch, bitstream and
coefficient identity — remain fatal. Nothing in this table is relaxed.

## The gates that were unaligned at audit time

**#3 `standalone_sdk_decode_3_packets`.** `docs/WINDOWS_STANDALONE_ORACLE.md:163`–`176`
records the first hosted run failing with unrestricted SDK workers while all API
calls returned success and correct geometry. The test now requests the documented
`TAG_CPU_MAX=1` mitigation (`tests/test_standalone_sdk_oracle.cpp:75`). The
defect at audit time was classification: if the mitigation ever failed, the same
reference instability was reported as an encoder-quality failure. Under the #120
contract that state is `BLOCKED_ORACLE`, not FAIL and not PASS. Migrated in the
follow-up below.

**#4 `test_filmscan3_windows_decoder`.** The two-repeat identity check is already
an instability detector. It is mapped to a thrown failure, so the same packet on
an unstable host produces a red gate rather than a blocked one.

**#5 `test_phase3a`.** Both the PSNR floor and the frame stamp are computed from
decoded pixels. This site is CUDA-only and belongs to the phase lane; changing its
registration from this issue would collide with #16/#187. Recorded here as an open
callsite, not edited.

## Pre-registered determinism and parity matrix

Protocol fixed before running: N=10 fresh processes per decoder per packet; SDK
runs the existing five-instance-per-format loop, FFmpeg runs one decode per
process; hashes cover the whole output buffer; parity is compared against the
pinned `.p10` reference. Inputs are the three committed 128×128 standalone
fixtures, whose packet bytes are fixed by `tests/fixtures/standalone/manifest.json`.

| Packet | Packet SHA-256 | SDK distinct YU64 / BGRA (50 decodes each) | FFmpeg 9.0.2 distinct (10 processes) | FFmpeg vs pinned n8.0 reference |
| --- | --- | --- | ---: | --- |
| flat | `236cacbac9c5f39a5fbd1a4b939619276ccfc622ac486bf5a2ba967bcf28392b` | 1 / 1 | 1 | byte-identical |
| gradient | `4086e8c7ec3cc0b287ab637a486354eaa89623d048ac9d79d290291099b767fb` | 1 / 1 | 1 | byte-identical |
| canonical | `9cf732846c1b9838c8f5d24f92db0182b82b5b6a05df63ffd5114fc1606db6fd` | 1 / 1 | 1 | byte-identical |

SDK p10 parity against the pinned FFmpeg reference (per plane, 10-bit):

| Packet | Y | Cb | Cr | max error |
| --- | ---: | ---: | ---: | ---: |
| flat | exact | exact | exact | 0 |
| gradient | 62.746 dB | 62.849 dB | 62.490 dB | 3 |
| canonical | 62.804 dB | 63.179 dB | 62.603 dB | 3 |

FFmpeg 9.0.2 reproducing the n8.0-produced reference bytes exactly is the useful
part of this matrix: for these three pinned packets the candidate independent
decoder is stable across versions and processes, and its output is what the
fixture manifest already binds.

**Cross-host observation, not a mechanism claim.** The hosted Windows CI record in
`docs/WINDOWS_STANDALONE_ORACLE.md:118` reports 62.7055 dB for gradient; this host
reports 62.746 dB for the same packet, the same pinned reference and the same test
code. The difference is in the SDK decode, not in the reference. It is consistent
with the per-host reference instability described in #120 and is not evidence of a
specific upstream mechanism.

## What remains unproven

- FFmpeg 9.0.2 is **not** thereby a qualified oracle. Qualification requires a
  reviewed adapter, a normalization contract for each output format, a bound
  packet SHA per gate, and negative controls. The matrix above covers three
  128×128 packets on one host and one `yuv422p10le` transform.
- No statement is made about 1080p/4K packets, GOP2, FIELDPLUS, BGRA conversion
  equivalence, or any GPU path.
- A stable decode of a wrong packet would still pass the parity comparison; the
  packet-identity gate remains `standalone_oracle_fixture_hashes`
  (`tests/CMakeLists.txt:202`), which is where encoder regressions must be caught.

## Recommended next actions

1. **#3** — **done** in the follow-up below: decode status, geometry, pitch,
   non-zero output and the fixture-hash gate stay strict; only the PSNR/stability
   signal is routed through `ClassifyDecodeOnlyReference`, so an unqualified or
   unstable reference exits 77 instead of 1. The CTest can now skip; the Draft PR
   remains the maintainer's merge decision.
2. **#4** — map "two decodes differ" to the blocked exit code instead of a thrown
   failure; keep everything else unchanged.
3. **#5** — hand to the phase lane; do not change from the #120 test-only branch.
4. **Oracle qualification** — if the pinned FFmpeg decode is to become a real
   oracle, the next step is the normalization/adapter contract and a negative
   control, not a wider sweep. The matrix above is its starting evidence.
5. **#11 (`src/core/phase3a_encoder.cpp:757`)** — the ABI-private prefix mutation
   is a separate boundary question under #144 and is untouched here.

## Follow-up (2026-10-24): callsite #3 migrated to the blocked state

- `tests/test_standalone_sdk_oracle.cpp:213`–`224` classifies the decoded-pixel
  half through `ClassifyDecodeOnlyReference` in `tests/reference_oracle.h:98`.
  A stable set of repeats against the pinned `.p10`/`.bgra` reference is
  `BLOCKED_ORACLE_NOT_QUALIFIED`; disagreeing repeats of the same immutable
  packet are `ORIGINAL_SDK_REFERENCE_NONDETERMINISTIC_QUARANTINED`. Both exit
  `77`. Neither is a pixel-accuracy PASS and neither is a stable quality failure.
- The PSNR and flat-exactness comparison is still computed and printed with the
  same thresholds, and the final line records `quality_measured`,
  `quality_meets_threshold` and `distinctOutputsMax` (YU64 or BGRA). It is
  evidence, not a gate, because the pinned FFmpeg decode is not an independently
  qualified oracle (see the matrix above and "What this matrix does not
  establish").
- Every strict signal is unchanged and still exits `1`: fixture storage/read and
  size, `CFHD_OpenDecoder`/`CFHD_PrepareToDecode`/`CFHD_DecodeSample` status,
  128x128 YU64/BGRA geometry and format, 512-byte padded pitch, non-zero
  output, the `Control()` encoder API path (`CFHD_OpenEncoder`,
  `CFHD_PrepareToEncode`, `CFHD_EncodeSample`, `CFHD_GetSampleData`) and the
  invalid-header rejection. A strict failure is reported as
  `ReferenceDecodeError` regardless of any earlier pixel observation.
- `tests/CMakeLists.txt:204`–`215` registers the test with `SKIP_RETURN_CODE 77`,
  the `reference-oracle` label, and a fail pattern restricted to
  `STRICT_FAILURE`/`Oracle harness error`. The old `SDK decode PASS:` output line
  no longer exists, so the former `PASS_REGULAR_EXPRESSION` was removed rather
  than left permanently unmatched.
- Deterministic SDK-free coverage lives in `tests/test_reference_oracle.cpp`
  (`TestDecodeOnlyReference`): stable repeats `77`, divergent repeats `77`,
  missing evidence `1`, and neither blocked state is a strict failure or
  release-allowing.
- Local Windows x64 CPU-only evidence at this revision: full Release CTest and
  the direct `77`/`1` controls are recorded in the PR handoff for the exact
  head; hosted CI is SDK-free and does not execute this callsite.
- `PASS` is unreachable at this callsite until an independent pixel oracle is
  qualified; that is the contract, not an oversight.
- Not changed: callsite #5 (`test_phase3a`, CUDA lane #16/#187) and callsite #4
  (`test_filmscan3_windows_decoder`, separate #120 child).

## Non-claims

Test-only documentation and evidence. No codec, encoder or decoder change; no
`upstream/` modification; no private corpus or storage location used; no GPU
result; no gate weakened; no protected-main merge, release or history change.
