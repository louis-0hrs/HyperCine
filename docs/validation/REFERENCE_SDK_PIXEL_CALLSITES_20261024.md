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
| 3 | `tests/test_standalone_sdk_oracle.cpp:75`, gate at `:190`–`192` | three pinned 128×128 packets, five fresh instances per format | `PSNR > 35 dB` vs pinned FFmpeg decode, flat must be exact | **Not aligned** — a reference-decoder pixel signal is a hard CTest PASS/FAIL (`tests/CMakeLists.txt:204`–`209`); distinct-hash counts are printed but never classified |
| 4 | `tests/test_filmscan3_windows_decoder.cpp:35`, gate at `:46`–`66` | two Mac-produced 1080p packets, two decodes each | output identical across two decodes, else the blocked state; output non-zero | **Aligned** — divergence maps to exit 77 and never short-circuits the second fixture; strict checks stay fatal (follow-up below) |
| 5 | `tests/test_phase3a.cpp:147`–`154` (CUDA-only) via `src/core/phase3a_encoder.cpp:3455`–`3463` and `:3438` | every frame of a 300-frame encoded sequence | per-frame `PSNR >= 35 dB` and a frame stamp read out of decoded pixels | **Not aligned** — reference-dependent strict gate; the CUDA lane (#16/#187) owns its registration (`tests/CMakeLists.txt:171`) |
| 6 | `tests/test_yuy2_stability.cpp:118` | one encoded sample, eight decodes | **encoder** masked-bitstream identity (`:142`–`154`); pixel hashes recorded only (`:178`–`193`) | **Aligned** — encoder-strict, pixels diagnostic |
| 7 | `tests/test_codec_roundtrip.cpp:116` | seven patterns | roundtrip success, bitstream hash; PSNR printed only (`:191`–`194`) | **Aligned** — pixels diagnostic |
| 8 | `tests/test_entropy_parallel.cpp:56` (CUDA-only) | one sample per variant | decode status only | **Aligned** — no pixel assertion |
| 9 | `tests/test_avi_sample.cpp:93` | upstream sample AVI, all frames | decode status only; per-frame hashes printed | **Aligned** — status-only |
| 10 | `benchmarks/benchmark_codec.cpp:280,289`, `benchmark_phase1c.cpp:212`, `benchmark_phase3a1.cpp` | measurement inputs | not registered as CTest release gates | Out of release scope |
| 11 | `src/core/phase3a_encoder.cpp:757`–`769` | — | mutates an ABI-private decoder prefix to force one worker before decoding | Flagged under the #144 boundary ("no ABI-dependent private decoder access as a production solution"); unchanged here |

Strict gates at every site — decode return code, geometry, pitch, bitstream and
coefficient identity — remain fatal. Nothing in this table is relaxed.

## The three gates that were unaligned at audit time

**#3 `standalone_sdk_decode_3_packets`.** `docs/WINDOWS_STANDALONE_ORACLE.md:163`–`176`
records the first hosted run failing with unrestricted SDK workers while all API
calls returned success and correct geometry. The test now requests the documented
`TAG_CPU_MAX=1` mitigation (`tests/test_standalone_sdk_oracle.cpp:71`–`73`). The
remaining defect is classification: if the mitigation ever fails, the same
reference instability is reported as an encoder-quality failure. Under the #120
contract that state is `BLOCKED_ORACLE`, not FAIL and not PASS.

**#4 `test_filmscan3_windows_decoder`.** The two-repeat identity check is already
an instability detector. At audit time it was mapped to a thrown failure, so the
same packet on an unstable host produced a red gate rather than a blocked one.
Migrated in the follow-up below.

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

1. **#3** — split the two signals: keep decode status, geometry, pitch, non-zero
   output and the fixture-hash gate strict; route only the PSNR/stability signal
   through `ClassifyReferenceGate` so an unstable reference exits 77 instead of 1.
   This needs a maintainer decision because the CTest would then be able to skip.
2. **#4** — **done** in the follow-up below: "two decodes differ" maps to the
   blocked exit code instead of a thrown failure; everything else unchanged.
3. **#5** — hand to the phase lane; do not change from the #120 test-only branch.
4. **Oracle qualification** — if the pinned FFmpeg decode is to become a real
   oracle, the next step is the normalization/adapter contract and a negative
   control, not a wider sweep. The matrix above is its starting evidence.
5. **#11 (`src/core/phase3a_encoder.cpp:757`)** — the ABI-private prefix mutation
   is a separate boundary question under #144 and is untouched here.

## Follow-up (2026-10-24): callsite #4 migrated to the blocked state

- `tests/test_filmscan3_windows_decoder.cpp:46`–`66` classifies the two-repeat
  comparison through `ClassifyRepeatedDecodeStability` in
  `tests/reference_oracle.h`. Divergence prints
  `ORIGINAL_SDK_REFERENCE_NONDETERMINISTIC_QUARANTINED` with both decoded-buffer
  FNV-1a hashes.
- A divergent fixture does not return early. Both fixtures always run their
  full storage/read/decode/geometry/pitch/non-zero checks, and the exit code is
  decided once by `ClassifyFixtureBatch` (`tests/reference_oracle.h:137`).
  Strict failures dominate: a broken second fixture exits `1` even when the
  first fixture was unstable, so quarantine can never mask a strict error.
  The batch exits `77` only when every fixture was strict-clean and at least
  one repeated decode diverged.
- Every strict signal is unchanged and still exits `1`: fixture storage/read,
  `CFHD_OpenDecoder` / `CFHD_PrepareToDecode` / `CFHD_DecodeSample` return codes,
  1920x1080 YU64 geometry, 7680-byte pitch with 16-byte alignment, and non-zero
  output.
- `tests/CMakeLists.txt` registers the test with `SKIP_RETURN_CODE 77` and the
  `reference-oracle` label, so the lane inventory reports
  `REFERENCE_ORACLE_BLOCKED` rather than a red gate for an unknown reference.
- Deterministic SDK-free coverage lives in `tests/test_reference_oracle.cpp`
  (`TestRepeatedDecodeStability`, `TestFixtureBatchPrecedence`): stable batch
  `0`, unstable batch `77`, and unstable-first-plus-strict-second `1`. It does
  not depend on provoking an unstable SDK decode.
- Local Windows x64 CPU-only evidence at this revision: full Release CTest
  **23/23 passed** (128.0 s); the two committed fixtures decode stably with
  `FNV=d464ad2b5da58f8e` and `FNV=3c483e1bf890463`; a missing second fixture
  exits `1` after the first fixture's PASS line, and a missing first fixture
  still lets the second fixture run and PASS before the batch exits `1`.
- `PASS` at this callsite means "repeats agreed", not "pixels are correct". The
  site makes no pixel-quality claim.
- Not changed: callsites #3 and #5. #3 still needs the maintainer decision about
  a CTest that can skip; #5 belongs to the phase lane.

## Non-claims

Test-only documentation and evidence. No codec, encoder or decoder change; no
`upstream/` modification; no private corpus or storage location used; no GPU
result; no gate weakened; no protected-main merge, release or history change.
