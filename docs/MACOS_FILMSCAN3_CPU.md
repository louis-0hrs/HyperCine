# Native FilmScan3 CPU integration — issue 23 follow-up

Copyright (c) 2026 HyperCine Contributors. MIT OR Apache-2.0.

This draft builds on PR #25 (`cf7a46d`), which is stacked on #22. It adds a
separate, selectable, SDK-free **autonomous FilmScan3 CPU encoder**. It does not
reuse original Windows sample headers, inject manifest quantization tables, or
merge the sibling Windows oracle PR #9. The older experimental fixed FS3-Q2
profile remains the default of the existing smoke tool.

## Evidence and limits

| Gate | Measured result |
|---|---|
| Tier A input SHA-256 | PASS: all 16 regenerated 1920×1080 BGRA buffers |
| Sequence input SHA-256 | PASS: all 120 regenerated buffers, in original order |
| Autonomous FSratelimiter, previous sample bytes, quant[36], scale[36] | PASS against both manifests, including the limiter bounds 0 and 20 |
| Full independently emitted Tier A samples | PASS: 16/16 normalized hashes and actual normalized bytes |
| Full independently emitted sequence samples | PASS: 120/120 normalized hashes and actual normalized bytes |
| Quantized coefficient FNV / independently framed band bytes | PASS: 480/480 Tier A and 3,600/3,600 sequence bands |
| Full original sample with explicitly injected variable metadata | PASS: 120/120 actual SDK samples byte-identical; originals untouched |
| Native FFmpeg decode | PASS: flat, gradient, noise and hard edges; decoded p10 PSNR >35 dB |
| Real Windows GoPro decoder, one worker | PASS: both native Mac fixtures, two fresh-decoder repetitions each; Windows CI 6/6 |
| Windows unquantized conversion/filter dumps | PENDING: not supplied; no unquantized intermediate identity claim |
| Other qualities, formats, 4K/8K and Metal | Outside this profile; no release or acceleration claim |

Current native results are recorded in `docs/validation/macos_filmscan3_tier_a.json`,
`macos_filmscan3_sequence.json` and `macos_filmscan3_sequence_injected.json`.
These distinguish hash verification from comparisons against real bytes. Passing
pixel-quality checks is not used as a replacement for bitstream identity.

The full encoder consumes only BGRA bytes, an aligned stride and optional typed
variable metadata. Its API has no reference sample, manifest, quant-table or
header input. Comparison code observes the emitted tables/state after encoding.
The exact 1080p input is checked by SHA-256 before every test invocation.

## Implementation and public boundary

`include/hypercine/filmscan3.h` exposes `FilmScan3Encoder`, immutable state
inspection, `Reset`, typed metadata, and separately callable conversion,
quantization and table generation. `ForwardLevel` remains independently usable
from `standalone.h`. CPU mathematics is in `src/cpu/filmscan3_frontend.cpp`;
quant/rate control and independent framing are in `src/core/filmscan3_encoder.cpp`.
There are no upstream includes or SDK/CUDA runtime/build dependencies in this
standalone target. Later backends can compare their buffers against these CPU
stages independently from entropy/framing; no Metal backend is added here.

The conversion promotes the validated CG709 arithmetic: per-product signed
multiply-high, saturation, explicit uint16 wrapping, inverted row traversal,
and even-pixel chroma selection. Quantization uses the default pre-emphasis
midpoint (`divisor/2-1`) and the integer reciprocal. Wavelet prescale is [0,2,0].
Common scalar wavelet divisions now spell out negative floor division in C++17;
the former arithmetic-shift results and all legacy Q2 packets are preserved.

The autonomous quantizer ports the applicable frozen `quantize.c` / `quantize.h`
FS3 progressive 10-bit GOP1 rules. It starts at limiter 4. Before each subsequent
frame it computes the compression ratio from **its own last completed sample
size**, applies the 4.5/5.5/6.5 and 3.0/2.5/2.0/1.5 thresholds, clamps state to
0..20 and the limiter used to scale the quant tables to 16. The low-frequency divisors and spatial
scales follow the upstream rules; the finer luma/chroma tables differ. Only
after a complete sample exists does the encoder commit its next state. Failed
storage validation or allocation does not advance the sequence.

Framing is independently written from the frozen CFHD tag grammar in `codec.c`:
raw three-channel size index, input/encoded format, version/quality/prescale,
size chunks, metadata records/free space, group extension, lowpass and highpass
headers, per-band quant/scale, trailers and patched channel/sample sizes. The
baseline-compatible version tag is 10.1.0. All size values are calculated from
actual newly generated data. No Windows header fragments are copied.

The initial frame counter accepts 65,535 samples per encoder instance; `Reset`
starts a new sequence. The initial encoder domain is deliberately **1920×1080, BGRA8, CG709 to YUV422
p10, progressive GOP1, quality 6**. Other geometries/formats/qualities are not
claimed. Stage conversion accepts the existing safe bounded geometry contract,
but this does not turn 128px SDK/CUDA output into a reliable oracle. Existing
FS3-Q2 128/256/1080 encodes remain separate. Windows quality #24 is not blocked
or modified; expansion to its other five normal qualities requires its actual
golden facets, not a changed quality tag alone.

## API and commands

One encoder instance represents one serial sequence. It owns no external SDK
handles and uses aligned RAII plane storage. The returned sample is an owned
compressed byte vector. Callers of external SDKs must copy it into their aligned
input storage as demonstrated by the decoder test.

```cpp
#include <hypercine/filmscan3.h>
using namespace hypercine::standalone;
FilmScan3Encoder encoder;
// bgra points to complete 16-byte-aligned 1920x1080 BGRA storage.
auto first = encoder.Encode(bgra, bytes, stride);
auto next = encoder.Encode(next_bgra, next_bytes, stride);
// next.state.previous_sample_bytes == first.sample.size()
```

Default metadata is deterministic for reproducible fixtures, with a valid fixed
GUID/date/time and a progressing 30fps timecode. Applications may supply their
own typed `FilmScan3Metadata` (GUID 16, date 10, time 8, timecode 11 bytes). The
encoder constructs the records, padding and frame-unique UFRM itself. Tests that
inject the original Windows **variable values only** label this explicitly;
raw identity there is additional evidence, not a claim that the original Windows
file was generated by the Mac. No original NAS file is changed.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF \
  -DHYPERCINE_REFERENCE_CORPUS_ROOT=/private/tmp/hypercine-nas-tier-a/extracted \
  -DHYPERCINE_FS3_SEQUENCE_ROOT=/private/tmp/hypercine-nas-sequence/extracted \
  -DHYPERCINE_FFMPEG_EXECUTABLE=/private/tmp/hypercine-ffmpeg-validator/ffmpeg \
  -DHYPERCINE_FFPROBE_EXECUTABLE=/private/tmp/hypercine-ffmpeg-validator/ffprobe
cmake --build build -j4
ctest --test-dir build -C Release --output-on-failure

# Explicit profile selection; the default remains fs3-q2.
build/hypercine_encode_smoke --profile filmscan3 bgra:input.bgra output.cfhd 1920 1080

# Real stateful sequence. No tables/header files are accepted.
build/hypercine_encode_filmscan3 /private/tmp/mac-fs3-output input0.bgra input1.bgra

python3 tests/validate_filmscan3.py \
  --probe build/hypercine_reference_probe --encoder build/hypercine_encode_filmscan3 \
  --corpus-root /private/tmp/hypercine-nas-sequence/extracted --sequence \
  --output-dir /private/tmp/mac-fs3-sequence \
  --report docs/validation/macos_filmscan3_sequence.json
```

For the additional unmasked comparison, add `--inject-metadata`. The encoder CLI
then receives only a fixed 45-byte set of variable metadata values per frame,
not any reference framing/table bytes. For separate pixel quality/decode checks,
pass `--ffmpeg /path/to/ffmpeg`; `--reference-yuv` on the encoder tool exports its
uncompressed p10 stages in Y/Cb/Cr little-endian planar order. A hash/state/byte
mismatch exits nonzero and names the first frame, stage/table or byte/tag word.

The 120-frame archive was copied read-only via Finder from
`smb://TRUENAS/DeDub/HyperCine/golden/windows/fs3/2026-10-14/sequence-1080`.
Expected actual archive: `reference-corpus-sequence-1080-tier-c-20261015.zip`,
317,566,878 bytes, SHA-256
`9667d078ca54dccaf4f849f0136a5a8aae674c2c5e603e29b6cfc225aab21b06`.
All 480 original SDK/CUDA files match the pinned sequence manifest. Original
metadata-normalized files are separate files; original raw samples are never
presented as normalized or modified. Large archives, source frames, sample
sequences and stage buffers remain outside Git.

## Sanitizers and the Windows decoder boundary

```sh
cmake -S . -B build-sanitized -DCMAKE_BUILD_TYPE=Debug \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined \
  -DHYPERCINE_REFERENCE_CORPUS_ROOT=/private/tmp/hypercine-nas-tier-a/extracted \
  -DHYPERCINE_FS3_SEQUENCE_ROOT=/private/tmp/hypercine-nas-sequence/extracted \
  -DHYPERCINE_FFMPEG_EXECUTABLE=/private/tmp/hypercine-ffmpeg-validator/ffmpeg \
  -DHYPERCINE_FFPROBE_EXECUTABLE=/private/tmp/hypercine-ffmpeg-validator/ffprobe
cmake --build build-sanitized -j4
ctest --test-dir build-sanitized -C Release --output-on-failure
```

Tests exercise the production API, signed midpoint vectors, initial golden
tables, live feedback, padded/aligned/invalid storage, strong failure-state
preservation, deterministic reset, all actual Tier A/sequence samples, raw
identity with explicitly injected metadata, and native FFmpeg p10 >35dB.
Legacy Q2 tests and the earlier diagnostic tests remain present.

Two small original Mac-produced samples (flat and gradient, 501,368 bytes total)
are committed under `tests/fixtures/macos-filmscan3/` with source/input/sample
provenance and SHA verification. These are generated by the autonomous API,
not the old reframe probe. Windows CPU CI builds
`test_filmscan3_windows_decoder` against the pristine GoPro SDK, sets actual
`TAG_CPU_MAX=1` through `CFHD_SetActiveMetadata`, and decodes each 1080p sample
into aligned YU64 twice with exact repeat equality. No packet bytes are patched
to force the worker setting. The SDK remains absent from Mac builds. A pending
or failed Windows run must not be relabeled as a decoder PASS.

The draft remains separate from Metal #7. Full unquantized Windows stage
identity is not inferred from matching quantized hashes. No other quality,
format, 4K/8K, application or acceleration milestone is released by these tests.

Validation of the final implementation: full Release CTest **25/25**, full Debug AddressSanitizer + UndefinedBehaviorSanitizer CTest **25/25**, including the actual 120-frame corpus. All **27** recorded legacy Q2 packet SHA-256 values remain unchanged. Source, binary, report and test-log fingerprints are recorded in `validation/macos_filmscan3_checks.json`.

Windows MSVC CI on code commit `a11fcdd548ef039d686489064c5719bc26582eb9` passed all **6/6** tests, including `test_filmscan3_windows_decoder` against the two original Mac production packets. [Run 38696144395](https://github.com/louis-0hrs/HyperCine/actions/runs/38696144395/job/116293046294) and `validation/macos_filmscan3_windows_decoder.json` record the result. This certifies those two GoPro decode vectors; the 120-frame actual SDK byte comparisons are the separate native Mac gate. The older per-encode reports retain their historical pending Windows status; this later CI evidence resolves that gate. Unquantized Windows stages remain pending.
