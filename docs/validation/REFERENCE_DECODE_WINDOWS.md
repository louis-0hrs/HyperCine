# Windows original-decoder repeatability, issue #120

Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0; see LICENSE-MIT and LICENSE-APACHE.

## Result and provenance

**REFERENCE_DECODER_NONDETERMINISTIC / release blocked.** Native Windows x64,
CPU-only original SDK, MSVC 19.44.35229, CMake 3.31.6, Python 3.10.9, FFmpeg 7.1
(libavcodec 61.19.100). No CUDA library is linked to the measurement worker.
No GPU job, 4K/8K run, production change, sanitizer qualification or performance
claim is included. This is an independent Windows CPU setup from the M4 report.
The actual CPU model was unavailable through the sandbox's CIM access and is
not inferred. No remote hardware was used.

The formal measurement used diagnostic sources at commit
`1f828c0acae07884969bd11b5f6fbca3a271fd21`, stacked on #128
`ae39cce531a2cfa5f14036f4c08fd595992b6b79` and #127. Original SDK baseline is
`11574d0295771edccadd17af14af74a539f924c7`. The
[formal evidence](reference_decode_windows_20261017.json) records SHA-256 of
the worker executable, measurement sources, AVI wrapper, packet, synthetic input,
every decoded buffer, and FFmpeg/FFprobe executables. Source hashes describe
actual checkout bytes, including platform line endings. Binary packet/output
files remain in the ignored build directory; no private corpus is required.

Each synthetic packet is encoded **once** from seed `0x1337BEEF`, packed YUY2 p8,
FilmScan3. Six fresh processes each open/prepare/decode/close five independent
decoder instances on precisely that packet. No `srand`, delays, CPU-affinity or
thread-count override is used. Input and output buffers are 16-byte aligned;
pitch is a multiple of 16. Both buffers outlive decoder teardown; the decoder is
explicitly closed before output inspection, matching the existing tests.
The worker preserves an immutable master and verifies
its per-call copy after decoding. No metadata normalization changes decoded input.
The original encoder stamps volatile metadata, so a later invocation can produce
a different raw packet SHA; all calls within a case use the recorded raw bytes.

## Formal measured matrix

| Case | Processes / calls | Distinct output hashes | Processes with internal divergence | Source PSNR min–max | Maximum byte delta vs first output |
| --- | --- | --- | --- | --- | --- |
| 128x128 seeded YUY2 | 6 / 30 | 30 | 6/6 | 7.801–48.040 dB | 255 |
| 1920x1080 seeded YUY2 | 6 / 30 | 30 | 6/6 | 54.108–54.131 dB | 1 |

All **60/60** calls returned numeric `CFHD_ERROR_OKAY=0` and valid geometry,
format and buffer sizes. First-call output hashes differed across all six fresh
processes in each case. Output sizes/pitches were 32,768/256 and 4,147,200/3,840
bytes respectively. Up to 25,177 and 454,184 bytes changed relative to the first
output. PSNR uses every packed p8 byte at peak 255, with no orientation search,
excluded pixels or threshold adjustment. It is a combined Y/Cb/Cr code metric,
not a source-RGB metric or proof of identical color semantics.

Exact raw sample SHA-256:

* 128x128, 29,044 bytes:
  `1012b9b16cd68a17c1e845b5d77590489c1b8de8f3d8ab5045167a77ddf20db9`
* 1920x1080, 3,304,184 bytes:
  `4d3fd35a57018e26f6013464369f4b65b89085779c7f26b60b441cbf2fa0d4b2`

The existing `tests/fixtures/standalone/flat.cfhd` was also attempted as a
128px positive control: two processes/five calls each, **10/10 API successes
but five distinct output hashes**. It is **not a stable YUY2 control** on this
setup. Its source PSNR is unmeasured, never assumed to pass. FFmpeg's repeated
outputs provide an observed stable independent-decoder control instead.

## Initial exploratory matrix is retained

The [initial observations](reference_decode_windows_exploratory_20261017.json)
are retained to avoid hiding the more severe 1080p outcome. Before the source
commit and actual-format/control reporting additions, six processes/five calls
per case produced **30 distinct hashes in both geometries**, with all six
processes internally divergent. At 1080p the source PSNR ranged
**9.644–54.132 dB**, up to **2,306,634 bytes** differed, maximum absolute delta
**234**. Packet SHA was
`d37825b72c0c10887aafde61777f2d166246f36b3e8934db694e2eeba0bfbf12`.
128px PSNR was 7.801–48.169 dB, max delta 255. All API statuses were zero.

This exploratory JSON records its own worker/source hashes; its `source_commit`
is the parent checkout with then-uncommitted harness files, **not** a claim that
those sources were present in #128. Some initial collection overlapped the short
CPU CTest run. The formal matrix above was collected separately after that suite,
at the exact diagnostic source commit. An
[intermediate matrix](reference_decode_windows_preteardown_20261017.json) at
`2b569356ada41c4e3100f132389ee4973be85e0d` is also retained (28/30 distinct
128px hashes, 30/30 1080p hashes). Both earlier workers inspected/wrote output
before closing the decoder; the final worker explicitly closes before inspection
and keeps both buffers alive through teardown. This removes that harness
lifetime/order confounder from the final result; earlier severe outcomes are
exploratory, not independent proof that teardown order is irrelevant.
Different raw packets and scheduling mean these
two matrices must not be merged into one fixed-packet repetition count. The final
small 1080p deltas do not disprove the earlier large deltas or establish a fix.

## Second-decoder triangulation and #11 relationship

Reuse `tests/validate_standalone.py::avi`, the same single-packet AVI convention
used by #11 / PR #86. Feed the exact raw packet bytes to FFprobe/FFmpeg, pinned by
reported version, executable hashes and library versions. Three sequential fresh
FFmpeg processes per packet use `-threads 1`, `-flags +bitexact`, one frame,
`-pix_fmt yuyv422`, tightly packed top-down p8. Both cases produced **one hash
across three runs**. FFmpeg source PSNR was 46.766 and 50.422 dB respectively.

FFprobe reports native `yuv422p10le`, 10-bit, with **range/matrix absent**. The
report leaves those fields UNSPECIFIED and records the p10-to-p8 output conversion.
No forced range/matrix is introduced. Formal first-GoPro versus FFmpeg differences
had max delta 140 at 128px and one at 1080p. FFmpeg is **not** accepted as an
oracle, nor assumed byte-identical. Independently generated packet comparisons
are unmeasured; no claim that all between-run packet changes are metadata-only
or that the original encoder is deterministic follows from these measurements.
This narrow YUY2 experiment does not replace #11's YU64-to-p10 matrix or its
first-inverse-stage investigation. FFmpeg 8 remains untested here.

## Source audit and classification boundary

* `tests/test_yuy2_stability.cpp`, `DecodeOk` and `main`: already separates masked
  packet equality from informational pixel hashes, but the historical decoded
  output uses a standard vector. The new worker uses aligned storage throughout.
* `tests/test_phase2b.cpp:178` requests **BGRA**, sets processing metadata, then
  `:343` fails on decode error and `:349` on source-RGB PSNR. Packet/entropy checks
  earlier in that test are independent. A PSNR-only failure is not a proven
  encoder mismatch.
* `src/core/phase2c_encoder.cpp:275` likewise requests **BGRA**, not YUY2.
  `:931` combines encoder coefficient/payload/sample gates, original-decoder pixel
  equality and source PSNR into `rep.ok`; `:1738` combines pipeline byte match,
  decode acceptance and PSNR. `tests/test_phase2c.cpp:48` and `:64` consume those
  combined gates. The existing pipeline byte comparator at `:1713` tolerates two
  differing bytes; that historical behavior is unchanged by this test-only PR.
* `upstream/Codec/config.h:263` defaults `_THREADED_DECODER=1`.
  `upstream/Codec/InvertHorizontalStrip16s.c:3770` defines
  `InvertHorizontalStrip16sToYUYV`; `:3872` onwards mixes CRT `rand()` into vector
  rounding. This is a source-backed hypothesis for small YUY2 differences,
  **not proof that every large divergence or BGRA failure has that cause**.

In particular, this YUY2 worker does **not** directly reproduce the BGRA output
path/processing metadata of Phase2B/C, nor locate the first differing inverse
stage. That remains explicit follow-up rather than a speculative production fix.

The existing #127/#128 classifier is consumed without changes. Packet mismatch
and decode API/invalid-output errors remain FAIL; stable bad quality remains FAIL;
divergent pixel hashes are UNRESOLVED/nonzero. Every call carries its packet SHA.
The worker does **not** compare independently encoded CPU/GPU/reference packets:
`encoder_packets_equal=null`, encoder comparison UNAVAILABLE, encoder gate
UNRESOLVED. Re-reading one immutable sample is not fabricated encoder parity.
Even stable good decoded output cannot produce a false encoder PASS here.
Alternate decoder absence is UNAVAILABLE; configured alternate failure aborts
with a generic error. No actual test failure becomes CTest success or SKIP.

## Reproduction and checks

```powershell
cmake -S . -B build -A x64 -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_BENCHMARKS=OFF
cmake --build build --config Release --parallel 8
ctest --test-dir build -C Release --output-on-failure
python -B tests/reference_decode/measure.py --worker build/tests/Release/reference_decode_worker.exe --work build/reference-measurement --output build/reference-measurement.json --processes 6 --ffmpeg "$env:FFMPEG" --ffprobe "$env:FFPROBE"
```

Set `FFMPEG`/`FFPROBE` locally to installed executables. The work directory must
be fresh; prior observations are not overwritten. Without both options, alternate
availability is explicitly UNAVAILABLE. Exit 1 means unresolved/failed observed
gates, **not** successful qualification. Exit 2 means measurement failure.

Native SDK-enabled Release full CTest: **12/12 PASS**, zero skips. The classifier
contains 15 synthetic unit methods; the new measurement unit suite contains three
(known byte/PSNR math, incomplete worker rejection, API-error and unavailable
alternate reporting). Existing tests and thresholds are unchanged. The worker is
a manual measurement executable; its unresolved report is deliberately not
registered as a passing CTest quality gate. No host/GPU sanitizer run is claimed;
#13 remains independent. CUDA Phase2B/C and M4 4K/8K remain untested in this PR.

## Remediation options and next action

1. An isolated original-decoder subprocess can contain/report API failures;
   these fresh-process results show it **does not guarantee stable pixels**.
2. A reviewed independent quality decoder could become an additional gate only
   after agreed format/range/tolerance acceptance and #11 review. This report
   does not authorize replacing the original gate with FFmpeg.
3. Repairing/qualifying the SDK-free decoder is separate reviewed work under the
   existing production branches and #13 safety gate.

Do not modify frozen upstream, lower PSNR thresholds, delete assertions, add
sleep/reseed workarounds or assume `_THREADED_DECODER=0` is buildable. Keep the
existing Phase2B/C failures and release block visible. **Next action:** review
this Windows evidence, then scope an immutable BGRA worker matching the existing
processing metadata and first-divergence tracing before choosing gate remediation.
#120 remains open; no main merge or release is authorized by this report.
