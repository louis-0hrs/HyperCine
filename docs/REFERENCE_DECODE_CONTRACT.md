<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Reference decoder test contract — issue #120, portable first slice

Audited source: main `e87ef0258ba37f5c78f1557c59d9c7331df5521c`.
This change provides a test-only observation classifier and deterministic
synthetic vectors. It does not collect native Windows measurements, reproduce
#116, replace an original decoder, or quarantine any existing CTest.

## Current conflated gates

- `tests/test_phase2b.cpp`: the full-frame result checks encoder success,
  byte equality and coefficient mismatch count first. `DecodeSample` then
  checks original-decoder API success; `BestPsnr` has a separate 35 dB gate.
  All eventually return the same test failure. A PSNR-only failure therefore
  does not identify an encoder mismatch. No assertion is removed here.
- `tests/test_phase2c.cpp`: identity `id.ok` conflates coefficient, payload,
  sample-byte and decoded-pixel equality; the subsequent digest condition
  combines packet and decoded-image hashes. Pipelines also carry PSNR errors
  through a shared `ok` result. This executable unconditionally reaches 8K;
  it is unsuitable as a bounded 1080p diagnostic on a constrained GPU.
- `src/core/phase2c_encoder.cpp`: `VerifyPhase2CIdentity` folds decoded-pixel
  equality into `ok` even when complete normalized packets match.
- `tests/test_yuy2_stability.cpp`: masked packets remain binding, while repeated
  decoded-image hashes are diagnostic. Its 128x128 black sample is not a
  measured 1080p Windows positive control. Pixel storage in `DecodeOk` is a
  standard vector; a future diagnostic must use aligned output instead.
- `upstream/Codec/InvertHorizontalStrip16s.c`: inverse YUY2 conversion includes
  random rounding; `upstream/Codec/config.h` enables the threaded decoder.
  These explain why repeatability must be measured but do not prove the
  cause of the large divergences reported in #116. Frozen sources stay intact.

Existing differential work is #11 / PR #86; its decoder differences and
format/range requirements remain independent. FFmpeg is not presumed to be a
byte-identical GoPro replacement or an automatically accepted quality oracle.

## Executable classification

`tests/reference_decode/contract.py` accepts at most 150 observations, 30
process ordinals and five call ordinals per process. It requires one immutable
sample SHA-256, per-call sample/pixel SHA-256, decoder API code, an explicit
successful output-buffer/geometry check and PSNR (or explicit source equality).
It consumes observations; it cannot authenticate or generate them.

| Observation | Classification | Gate consequence |
| --- | --- | --- |
| Encoder packets differ, or any call identifies different sample bytes | ENCODE_BITSTREAM_MISMATCH | FAIL, takes precedence |
| Decoder returns error or successful API has invalid output | REFERENCE_DECODE_ERROR | FAIL |
| Identical packet, differing decoded-pixel hashes | REFERENCE_DECODER_NONDETERMINISTIC | UNRESOLVED, nonzero exit |
| Repeatable output, PSNR <= 35 dB | DECODE_QUALITY_FAILURE_STABLE | FAIL |
| Missing parity, hashes, output validation or repetition | REFERENCE_EVIDENCE_INCOMPLETE | UNRESOLVED, nonzero exit |
| Matching packets, valid repeatable outputs and PSNR > 35 dB | PASS | Only these observed gates pass |

PASS requires two calls in each of at least two independent processes. It
means observed repeatability, not proof of determinism on every execution.
Missing alternate-decoder work is explicitly UNAVAILABLE, never zero. Every
report retains `release_blocked=true` and `release_gate=UNRESOLVED`, including
observed PASS: this small classifier cannot authorize codec release. Its CLI
exit code is the scoped reference classification; the release field is separate.
The threshold is unchanged and cannot be lowered below 35 dB.

CTest `reference_decode_contract` tests the classifier using synthetic
observations, including precedence, unstable pixels, stable bad quality, API
errors, missing evidence, same-packet provenance, bounded calls, invalid/nonfinite
measurements and CLI nonzero behavior. A successful synthetic unit test does
not count as a successful native decoder run.

```sh
python3 -B tests/reference_decode/test_contract.py
ctest --test-dir build -C Release --output-on-failure
# For future measured evidence; this does not invoke any decoder:
python3 tests/reference_decode/contract.py evidence.json
```

## Remaining acceptance and handoff

Native Windows work remains required: a small CUDA-free SDK-linked generator
and re-decode worker, aligned 1080p YUY2 FilmScan3 buffers and 16-byte stride,
fixed seed `0x1337BEEF`, RAII handles, unmodified packet/source/code hashes,
per-call output pitch/size/format/status and PSNR, short fresh-process repeats,
and explicit first-divergence evidence. Measure actual repeat counts and
retain raw pixel files outside the Git tree. Bound subprocess time and avoid
8K or shared GPUs. Reuse #11's pinned independent-decoder tools on the exact
same packet with range/matrix/depth stated. No such runtime result is claimed
on this Mac. The original tests still fail as before when their gates fail;
#120 must remain open until its native and independent-decoder gates are met.

Native validation on macOS ARM64 / AppleClang 21 / CMake 3.31.5 / Python 3.9.6:
**14/14 synthetic unit methods PASS**; full SDK-free Release CTest with required
FFmpeg n8.0 / ffprobe external decode **29/29 PASS**, zero skips, 72.14 seconds.
This is existing standalone encoder coverage plus the new synthetic contract,
not execution of the frozen GoPro decoder on native ARM64 or Windows.
