# Phase 2A fail-closed reference-oracle contract (2026-10-20)

Status: test-only Draft. Issue: #120. Parent diagnostic stack: #206 / #207.

This note defines and implements the fail-closed decision contract requested by
#120 for Phase 2 encoder tests whose pixel gate uses the frozen original GoPro
SDK. It does not qualify a new pixel oracle and does not make any pixel-quality
gate green.

## Why a separate contract is required

The original SDK returns `CFHD_ERROR_OKAY` with valid geometry while producing
different BGRA pixels for the same immutable packet across fresh processes.
The observed divergent outputs are real evidence, not an encoder-payload
difference. A boolean `ok` that combines encoder identity with decoded pixels
therefore cannot distinguish three states:

- the encoder/bitstream is wrong (must fail),
- the original decoder returned an error (must fail),
- the original decoder returned different pixels for equivalent bytes
  (quarantined reference gate, not an encoder pass and not an encoder failure).

## Decision contract

`tests/reference_oracle.h` is SDK-free and deterministic. It is exercised by
`tests/test_reference_oracle.cpp` on every platform.

| Decision | Exit | CTest | Release gate | Meaning |
| --- | --- | --- | --- | --- |
| `PASS` | 0 | Passed | allowed | Strict encoder gates pass, repeated decodes are identical, and a qualified independent oracle meets the threshold. |
| `ENCODE_BITSTREAM_MISMATCH` | 1 | Failed | blocked | Packet bytes, coefficients, entropy or parsed metadata differ. Always fatal. |
| `REFERENCE_DECODE_ERROR` | 1 | Failed | blocked | Decoder error, invalid geometry, or no decode evidence. Fail closed. |
| `ORIGINAL_SDK_REFERENCE_NONDETERMINISTIC_QUARANTINED` | 77 | Skipped | blocked | Equivalent packet bytes produced more than one decoded output. Diagnostic only. |
| `BLOCKED_ORACLE_NOT_QUALIFIED` | 77 | Skipped | blocked | Decodes are stable but no qualified independent pixel oracle exists. Never green. |
| `DECODE_QUALITY_FAILURE_STABLE` | 1 | Failed | blocked | A qualified oracle is available, decodes are stable, and quality is below threshold. |

CTest property `SKIP_RETURN_CODE 77` is registered for `test_phase2a`. A skip
therefore means "strict encoder gates passed, reference pixel gate blocked";
it is not a pass and must not be counted as pixel-accuracy evidence.

## Packet attribution rule

`CompareSyntheticPackets` compares two packets produced from the same synthetic
input:

1. Both parser views must be valid and bounded by the packet size.
2. Packet sizes must match.
3. All parsed metadata fields must match except the frame number.
4. Exactly one big-endian tag word for the parsed frame number must exist at a
   4-byte-aligned offset inside the parser-derived header region. The encoder
   stores this field through `PutTagPairOptional`, so the stored tag id is
   `0xffbb` (the optional-tag sign bit is part of the id). A missing,
   non-aligned or ambiguous segment fails closed.
5. Every header byte outside the 16-bit frame-number value must match.
6. Every payload byte from the parser-derived header boundary to the end must
   match.

No fixed prefix is skipped. A payload difference is always a strict failure.

## Integration in `test_phase2a`

Strict, always fatal:

- `RunPhase2` byte/coefficient/expansion identity for host full, host streamed,
  host compact and device-resident paths;
- zero host-to-device copies on the resident path;
- equal packet size;
- packet equivalence limited to the parsed frame-number field;
- decoded geometry (1920x1080 BGRA, pitch and buffer extent).

Diagnostic, never green:

- repeated in-process decodes (default 3, bounded 1..8);
- bounded fresh-process decodes through the test-only `--decode-hash` mode
  (default 3 per packet, bounded 0..8);
- decoded BGRA digests, PSNR and first differing pixel;
- pixel equality between the host and device packets.

The frozen original SDK pixel path is marked unqualified, so a stable run
classifies as `BLOCKED_ORACLE_NOT_QUALIFIED` rather than `PASS`. Observed
divergence classifies as `ORIGINAL_SDK_REFERENCE_NONDETERMINISTIC_QUARANTINED`.

## Synthetic acceptance cases

`test_reference_oracle` verifies:

- (i) byte-mismatched packets always fail, regardless of other signals;
- (ii) equivalent bytes with divergent decoder output are quarantined and are
  neither an encoder failure nor a pass;
- (iii) a stable bad image with a qualified oracle remains a real failure;
- (iv) an unavailable or unqualified oracle is explicitly blocked, and missing
  decode evidence fails closed;
- packet attribution for identical bytes, frame-number-only differences, the
  stored optional-tag id (`0xffbb`), payload differences, unrelated header
  differences, size differences, unparsable boundaries, non-word-aligned tag
  segments, ambiguous tag segments and missing tag segments.

## Native evidence

Platform: native Windows x64 QEMU guest, AMD EPYC 7601 with 8 logical
processors and no SMT; Tesla M4 (compute capability 5.2), driver 582.78;
CUDA 12.8.93 with `sm_52`; MSVC 19.44.35229.0; CMake 3.31.12; Ninja 1.13.2.
Release build with tests, upstream SDK and CUDA ON, benchmarks OFF.

- `test_reference_oracle`: passed in the CUDA build and in a CUDA-free Visual
  Studio build. The contract has no SDK, CUDA or platform dependency.
- `test_phase2a`: four consecutive native runs exited `77` and CTest reported
  `Skipped`. Every run reported
  `packet_equivalence=FRAME_NUMBER_HEADER_ONLY`, `encoder_strict_gates=PASS`,
  and
  `reference_oracle_decision=ORIGINAL_SDK_REFERENCE_NONDETERMINISTIC_QUARANTINED`.
  The bounded observation recorded 2, 3, 4 and 5 distinct decoded outputs
  across 12 decodes per run (3 in-process per packet plus 3 fresh processes per
  packet). One run measured 29.8863 dB on first decode; the others measured
  44.466 dB. A separate standalone `--decode-hash` run of a captured packet
  measured 8.73314645 dB. The large spread is the reference-decoder
  instability, not an encoder difference.
- Full native Release CTest at the exact source state: 25 registered tests,
  22 passed, 1 skipped by design (`test_phase2a`), 2 failed (`test_phase2c`,
  `test_phase2d`). Across reruns `test_phase2c` failed with decoder pixel
  identity/PSNR (`dec=0`, PSNR 10.49–15.02 dB) and once with an 8K
  `QueueGpuEntropyFrame` resource failure; `test_phase2d` failed with an
  upstream-comparison error or a metadata-safety clip encode failure. Their
  sources are unchanged by this diff; they are not claimed fixed or green.

Reproduction (repository-relative; no host-specific values):

```
cmake -S . -B build-cuda -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_ENABLE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=52 \
  -DHYPERCINE_BUILD_BENCHMARKS=OFF
cmake --build build-cuda
ctest --test-dir build-cuda --output-on-failure
```

The repeated/fresh decode counts are bounded by
`HYPERCINE_PHASE2A_DECODE_REPEATS` (default 3, maximum 8) and
`HYPERCINE_PHASE2A_FRESH_PROCESSES` (default 3, maximum 8).

## Non-claims and limits

- The original GoPro SDK is not qualified as a pixel oracle.
- FFmpeg is not promoted to an oracle by this change.
- `test_phase2b` and `test_phase2c` still use their existing pixel gates and
  are not claimed fixed here.
- No production encoder/decoder, CUDA kernel, CMake option or `upstream/`
  change.
- No threshold change, retry-to-green, assertion deletion or blanket skip.
- No protected-main merge, tag, release or history rewrite.

## Next action

Independent review of the contract and of the stacked Draft. After review,
either qualify an independent deterministic oracle on a pre-registered
same-sample matrix, or keep the pixel gate blocked and propagate the same
contract to `test_phase2b`/`test_phase2c`.
