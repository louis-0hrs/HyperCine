# Phase 2D fail-closed reference-oracle propagation (2026-10-20)

Status: Draft. Issue: #120. Test-contract change plus one condition in the
test-support `VerifyPhase2DMetadataSafety` helper; no encoder, decoder, kernel
or bitstream change. Parent contract and evidence:
`docs/validation/PHASE2A_FAILCLOSED_ORACLE_20261020.md` (Draft #208) and
`docs/validation/PHASE2BC_FAILCLOSED_ORACLE_20261020.md` (Draft #209). This
change is stacked on the #209 head and does not modify the #208 or #209 branch
files.

This note records the last Phase 2 pixel gate that still folded the frozen
original SDK's decoded pixels into an encoder result. It does not qualify a new
pixel oracle and does not make any pixel-quality gate green.

## Strict versus reference signal

`VerifyPhase2DUpstreamComparison` and `RunPhase2D` both combine encoder
identity with the original SDK's decoded pixels in a single boolean. The SDK
has returned different BGRA pixels for identical packet bytes, so that boolean
cannot distinguish:

- a strict encoder/bitstream difference (must fail),
- an original-decoder error (must fail),
- unstable decoded pixels (quarantined reference gate),
- a stable decode without a qualified independent oracle (blocked, never
  green).

The shared contract is `tests/reference_oracle.h`. CTest registers
`SKIP_RETURN_CODE 77` for `test_phase2d` under the `reference-oracle` label. A
skip means the strict stages passed and the reference pixel gate is blocked; it
is not a pass and must not be counted as pixel-accuracy evidence.

## `test_phase2d`

Strict and always fatal, before the reference classification:

- A/B/C byte identity for the same handle, for independent handles after
  volatile-metadata masking, and across all GPU kernel/graph modes;
- all 30 subband payload identities and the expected band count;
- the 4K Frame1Initial/SteadyState sample sizes and `FSratelimiter` values;
- slot-selection policy expectations;
- kernel-evolution launch/node counts and `byte_match_reference` for every
  study point;
- 4K Polling/Blocking/LowLatency sample SHA-256 equality and the CPU metric
  sanity checks;
- metadata-safety GUID, masking and deterministic-metadata outcomes.

Reference signal, diagnostic only:

- the decoded-pixel comparisons of payload-identical samples A/B/C;
- PSNR against the canonical source for those comparisons and for the three
  Polling/Blocking/LowLatency pipeline runs;
- the decoded-pixel comparison is no longer part of the strict predicate and
  `max_abs_pixel_diff`/PSNR no longer abort the remaining strict stages.

A failed original-SDK decode of a verified bitstream is
`REFERENCE_DECODE_ERROR` and fails. Divergent decoded outputs are
`ORIGINAL_SDK_REFERENCE_NONDETERMINISTIC_QUARANTINED` (exit 77). Stable decodes
remain `BLOCKED_ORACLE_NOT_QUALIFIED` (exit 77) because the original SDK is not
a qualified oracle.

## Metadata-safety helper

`VerifyPhase2DMetadataSafety` is a verification helper used only by this test;
it contains no encoder or decoder logic. Its internal `encode_one_clip` used
`Phase2DResult.ok`, which also folds in the original-SDK PSNR, so a metadata
check failed whenever the reference decoder returned a low-PSNR frame. It now
requires the strict bitstream match, a successful decode and a non-empty
sample. Measured on the host below, five otherwise identical metadata clips
reported PSNR 14.48, 17.05, 17.10, 18.14 and 20.41 dB with byte-identical
1181024-byte samples, which is the known reference nondeterminism rather than a
metadata defect. No bitstream, encoder or decoder path changed.

## Native evidence

Platform: native Windows x64 QEMU guest, AMD EPYC 7601 with 8 logical
processors and no SMT; Tesla M4 (compute capability 5.2), driver 582.78;
CUDA 12.8.93 with `sm_52`; MSVC 19.44.35229.0; CMake 3.31.12; Ninja 1.13.2.
Release build with tests, upstream SDK and CUDA ON.

- `test_reference_oracle`: passed.
- `test_phase2d`: exit 77, CTest `Skipped`. Part 1 reported
  `reference_decode=EQUAL` with PSNR 44.5539, 44.5824, 44.6758, 44.5823 and
  44.7031 dB for five of the six resolution/quant-state comparisons; the 1080p
  Frame1Initial comparison reported `reference_decode=UNSTABLE` at 17.5438 dB.
  All 30-subband, byte-size, slot-policy, kernel-evolution and 4K D2H SHA
  strict stages passed first. Final verdict:
  `ORIGINAL_SDK_REFERENCE_NONDETERMINISTIC_QUARANTINED`, observed decodes 21,
  distinct outputs 2, minimum PSNR 17.5438 dB.
- Full native CTest at the exact head: 25 registered, **21 passed, 3 skipped by
  design** (`test_phase2a` from #208, `test_phase2b` from #209, `test_phase2d`
  from this slice), **1 failed**. The failure is `test_phase2c` at the known
  strict 8K DeviceInput slots=3/streams=4 `QueueGpuEntropyFrame` M4 capacity
  gate, unchanged from #209 and neither masked nor claimed resolved.

Reproduction (repository-relative):

```
cmake -S . -B build-cuda -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_ENABLE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=52 \
  -DCMAKE_CUDA_COMPILER=<nvcc path>
cmake --build build-cuda
ctest --test-dir build-cuda -R "test_reference_oracle|test_phase2d" \
  --output-on-failure
```

## Non-claims and limits

- No independent pixel oracle is qualified; FFmpeg is not promoted to one.
- Phase 3 pixel gates are unchanged.
- No production encoder/decoder, CUDA kernel, CMake option, threshold or
  `upstream/` change.
- No retry-to-green, assertion deletion or blanket skip.
- Known 8K M4 capacity conditions in other tests are neither masked nor
  claimed resolved.
- No protected-main merge, tag, release or history rewrite.
