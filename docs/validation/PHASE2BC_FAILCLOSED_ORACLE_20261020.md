# Phase 2B/2C fail-closed reference-oracle propagation (2026-10-20)

Status: test-only Draft. Issue: #120. Parent contract and evidence:
`docs/validation/PHASE2A_FAILCLOSED_ORACLE_20261020.md` (Draft #208). This
change is stacked on #208 head and does not modify its files.

This note records the propagation of the fail-closed reference-oracle contract
from `test_phase2a` to the remaining Phase 2 pixel gates in `test_phase2b` and
`test_phase2c`. It does not qualify a new pixel oracle and does not make any
pixel-quality gate green.

## Strict versus reference signal

Both tests previously combined encoder identity and the frozen original SDK's
decoded pixels into one boolean result. The SDK has returned different BGRA
pixels for identical packet bytes, so that boolean could not distinguish:

- a strict encoder/bitstream difference (must fail),
- an original-decoder error (must fail),
- unstable decoded pixels (quarantined reference gate),
- a stable decode without a qualified independent oracle (blocked, never
  green).

The shared contract is `tests/reference_oracle.h`. CTest registers
`SKIP_RETURN_CODE 77` for both tests under the `reference-oracle` label. A skip
means the strict stages passed and the reference pixel gate is blocked; it is
not a pass and must not be counted as pixel-accuracy evidence.

## `test_phase2b`

Strict and always fatal, before the reference classification:

- GPU/CPU coefficient, token, codeword, bit-offset and payload equality for
  every checked highpass band at 128x128 and 4K;
- full-frame 1080p GPU entropy byte identity and zero coefficient mismatches;
- 1080p decoded geometry (1920x1080 BGRA, pitch and buffer extent) when a decode
  succeeds.

Reference signal, diagnostic only:

- bounded repeated in-process decodes of the same immutable 1080p packet
  (default 3, bounded 1..8 through `HYPERCINE_PHASE2B_DECODE_REPEATS`);
- decoded BGRA SHA-256 digests and best PSNR over the observed decodes.

A failed decode is `REFERENCE_DECODE_ERROR` and fails. Divergent digests are
`ORIGINAL_SDK_REFERENCE_NONDETERMINISTIC_QUARANTINED` (exit 77). Stable decodes
remain `BLOCKED_ORACLE_NOT_QUALIFIED` (exit 77) because the original SDK is not
a qualified oracle.

The 4K strict entropy-stage checks run after the 1080p classification, so the
blocked pixel gate does not hide a strict failure.

## `test_phase2c`

Strict and always fatal:

- coefficient equality, per-band payload length and byte equality, complete
  sample byte equality across consolidated/streamed/1/2/4/8-stream and GPU
  lowpass paths;
- sample SHA-256 equality between the CPU and GPU assembly paths;
- for every pipeline configuration, `bitstream_byte_match` and `decode_ok`;
- original-decoder error or incomparable decoded geometry.

Reference signal, diagnostic only:

- the CPU-versus-GPU decoded-pixel comparison of byte-identical samples; two
  unequal outputs are direct evidence of reference instability;
- PSNR against the canonical source for the identity and pipeline decodes.

The decoded-pixel SHA comparison is no longer part of the strict sample-digest
check. A verified bitstream with a low PSNR continues through the remaining
strict pipeline configurations and is classified once at the end. A verified
bitstream with a failed original-SDK decode is still a fatal
`REFERENCE_DECODE_ERROR`.

## Native evidence

Platform: native Windows x64 QEMU guest, AMD EPYC 7601 with 8 logical
processors and no SMT; Tesla M4 (compute capability 5.2), driver 582.78;
CUDA 12.8.93 with `sm_52`; MSVC 19.44.35229.0; CMake 3.31.12; Ninja 1.13.2.
Release build with tests, upstream SDK and CUDA ON.

- `test_reference_oracle`: passed.
- `test_phase2b`: exit 77, CTest `Skipped`. The 1080p observation recorded
  `decode_observation=3 distinct=2 best_psnr=44.466 dB` and
  `reference_oracle_decision=ORIGINAL_SDK_REFERENCE_NONDETERMINISTIC_QUARANTINED`.
  All 128x128, real-band and 4K strict entropy stages passed before that
  verdict.
- `test_phase2c`: the 1080p, 4K and 8K identity sections reported
  `reference_decode=EQUAL` with PSNR 44.4660, 44.5539 and 44.5823 dB and all
  strict sample byte/SHA identities passed. The 1080p slot=4 pipeline measured
  26.8765 dB; that reference signal was classified instead of failing the
  encoder gate, and the run continued. The later 8K DeviceInput slots=3,
  streams=4 configuration failed the strict `QueueGpuEntropyFrame` resource
  gate, which is a real failure and correctly remains fatal. That known M4
  capacity condition prevented this run from reaching the final blocked
  verdict; it is not claimed fixed, worked around or green.

Reproduction (repository-relative):

```
cmake -S . -B build-cuda -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_ENABLE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=52 \
  -DHYPERCINE_BUILD_BENCHMARKS=OFF
cmake --build build-cuda
ctest --test-dir build-cuda -R "test_reference_oracle|test_phase2b|test_phase2c" \
  --output-on-failure
```

## Non-claims and limits

- No independent pixel oracle is qualified; FFmpeg is not promoted to one.
- `test_phase2d` and the Phase 3 pixel gates are unchanged.
- No production encoder/decoder, CUDA kernel, CMake option, threshold or
  `upstream/` change.
- No retry-to-green, assertion deletion or blanket skip.
- The 8K M4 resource failure above is neither masked nor claimed resolved.
- No protected-main merge, tag, release or history rewrite.
