# Decoder differential audit (#11)

Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0; see LICENSE-MIT and LICENSE-APACHE.

This Windows audit sends the **same packet bytes** to the frozen GoPro SDK and
FFmpeg. It does not change decoder arithmetic or establish which decoder is correct.
Python uses only the standard library; FFmpeg is an external executable.

Build the CPU SDK configuration and run its complete suite:

```powershell
cmake -S . -B build -A x64 -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_BENCHMARKS=OFF
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Configure `HYPERCINE_GOLDEN_DIR` locally to the six-quality corpus root from
#24. Set `FFMPEG` and `FFPROBE` locally to the executable paths. Run:

```powershell
python tests/decoder_differential/compare.py --corpus "$env:HYPERCINE_GOLDEN_DIR" --sdk-dump build/tests/Release/decoder_sdk_dump.exe --ffmpeg "$env:FFMPEG" --ffprobe "$env:FFPROBE" --work build/decoder-differential --output build/decoder-differential-report.json
```

The corpus may use either the current tiered layout
(`<quality>/<tier>/manifest.json`, for example `low/1080p-16f/manifest.json`)
or the legacy `<quality>/manifest.json`. Every packet is checked against both
its frame record and artifact hash before decoding. Missing files, geometry or
hash mismatches, decoder failures, unexpected formats and invalid p10 values
are errors. No substitution or inferred PASS.

## Fixed inventory

- **1080p, full detail:** Low/FilmScan2/FilmScan3, frame indices 0/3/6
  (FlatBlack/HorizontalGradient/DeterministicNoise).
- **4K geometry probe:** Low and FilmScan3, frame 0.
- **8K geometry probe:** Low and FilmScan3, frame 0.
- **Stateful sequence:** FilmScan3 `sequence-120f`, one decoder instance across
  all 120 archived packets; frames 0/17/34/51/68/85/102/119 are compared.
- **Historical 128px:** standalone flat/gradient/canonical and the original
  GoPro-encoded control.

Each inventory packet is decoded three times by each decoder in fresh processes.
The stateful sequence arm runs twice; each sampled frame is also decoded
independently in a fresh process as a control.

## Measurement contract

FFmpeg receives a single-frame AVI (or the full multi-frame AVI for the sequence
arm) made by the existing `tests/validate_standalone.py` chunk wrapper. FFprobe
must identify the source as `cfhd`, expected dimensions and native
`yuv422p10le`; missing matrix/range/chroma tags remain missing. No color
conversion is requested in the baseline. Decoder settings are recorded.

GoPro YU64 output uses aligned storage and the SDK pitch. YU64 is interpreted as
top-down LE Y0/Cr/Y1/Cb, each unsigned sample shifted right six bits, without
rounding or orientation selection. The measured format is tightly packed
Y/Cb/Cr p10 in LE 16-bit containers. GoPro metadata: PRCS=65535, CLSY=2,
CLSR=1, CLSF=0, CPU_MAX=1. The self-test covers padded rows, orientation,
channel order, p16 extrema and padded BGRA tightening. Python tests cover byte
order, invalid lengths, upper bits, signed deltas, PSNR, percentile rank,
coordinates, group accounting, tier discovery, multi-frame AVI headers and the
FFmpeg select expression.

Delta means **GoPro minus FFmpeg**. Every sample is included, including borders.
PSNR uses peak 1023 for p10 and peak 255 for BGRA; `null` means zero MSE
(exact), not an unmeasured result. p95 is the nearest-rank 95th percentile over
all absolute errors, including zeros. The first 32 differences are in plane row
order. Histograms contain all observed signed values, with explicit zero-count
bins for -3 through +3. Each grouping partitions a complete plane: eight-luma-pixel
border (four chroma columns), x/y parity, FFmpeg code bins of 64 and a normalized
8x8 spatial grid. Grid cells are **not codec tiles**, and brightness bins are
code values, not inferred luminance.

Planes above 8,000,000 samples (4K/8K luma and 4K/8K chroma) keep exhaustive
overall, border/interior and parity statistics plus first differences, but omit
the per-sample code-bin and spatial groups. The omission is recorded in
`groups_omitted`; it is never silently inferred or labeled complete.

## Explicit range controls

On selected p10 cases the tool additionally decodes FFmpeg output with
`scale=in_range=full:out_range=full` and
`scale=in_range=limited:out_range=full`. It records each arm's SHA-256 and
whether it equals the baseline. This tests the measurement pipeline's range
handling; it is not a decoder-parity claim.

## BGRA common-conversion arm

The SDK is also asked for `CFHD_PIXEL_FORMAT_BGRA` (same metadata). FFmpeg
converts the p10 output with explicit BT.709 matrix, range and chroma-upsampling
variants (neighbor versus default). Each channel B/G/R/A is measured over the
full frame and the alpha channel is included because it is a real output byte.
This is a **conversion-path comparison**, not decoder parity: the SDK's cgRGB
policy and FFmpeg's swscale policy are not assumed identical. Results are
reported without choosing a "correct" converter.

## Stateful sequence arm

`sdk_dump --sequence` opens one decoder, calls `PrepareToDecode` on frame 0, and
calls `DecodeSample` for every archived packet in order, writing only the sampled
frames. FFmpeg decodes the same multi-frame AVI and a `select` filter emits the
same frame indices. The report records stateful and independent SDK hashes,
FFmpeg hashes, reproducibility, and per-frame p10 statistics. Rate-control state
that the sequence manifest does not model is not claimed.

## Output and failure behavior

Binary buffers and AVI files stay in the ignored working directory. The public
JSON contains hashes, settings and statistics, without corpus paths. A stability
failure writes its measured hashes then exits nonzero; pixel differences alone
are observations, not test failures or proof of a codec defect.

See `docs/FFMPEG_VS_GOPRO_DECODER.md` for the measured scope and remaining controls.
