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

Configure `HYPERCINE_GOLDEN_DIR` locally to the archived six-quality corpus root
from #24. Set `FFMPEG` and `FFPROBE` locally to the executable paths. Run:

```powershell
python tests/decoder_differential/compare.py --corpus "$env:HYPERCINE_GOLDEN_DIR" --sdk-dump build/tests/Release/decoder_sdk_dump.exe --ffmpeg "$env:FFMPEG" --ffprobe "$env:FFPROBE" --work build/decoder-differential --output build/decoder-differential-report.json
```

The fixed inventory is Low/FilmScan2/FilmScan3 at 1080p, frames 0/3/6
(FlatBlack/HorizontalGradient/DeterministicNoise), one Low 4K flat packet,
and the three 128px standalone fixtures plus the original SDK control.
Missing corpus files, packet length/hash mismatches, decoder failures, unexpected
formats and invalid p10 values are errors. No substitution or inferred PASS.

Each corpus packet is checked against both its frame record and artifact hash.
FFmpeg receives a minimal single-frame AVI made by the existing
`tests/validate_standalone.py::avi` wrapper. FFprobe must identify the source as
`cfhd`, expected dimensions and native `yuv422p10le`; missing matrix/range/chroma
tags remain missing. No color conversion is requested. Decoder settings are
recorded in the report; both decoders run in fresh processes three times.

GoPro output uses aligned storage and the SDK pitch. YU64 is interpreted as
top-down LE Y0/Cr/Y1/Cb, each unsigned sample shifted right six bits, without
rounding or orientation selection. The measured format is tightly packed
Y/Cb/Cr p10 in LE 16-bit containers. The self-test covers padded rows, orientation,
channel order and p16 extrema. Python tests cover byte order, invalid lengths,
upper bits, signed deltas, PSNR, percentile rank, coordinates and group accounting.

Delta means **GoPro minus FFmpeg**. Every sample is included, including borders.
PSNR uses peak 1023; `null` means zero MSE (exact), not an unmeasured result.
p95 is the nearest-rank 95th percentile over all absolute errors, including zeros.
The first 32 differences are in plane row order. Histograms contain all observed
signed values, with explicit zero-count bins for -3 through +3. Each grouping
partitions a complete plane: eight-luma-pixel border (four chroma columns), x/y
parity, FFmpeg code bins of 64 and a normalized 8x8 spatial grid. Grid cells are
**not codec tiles**, and brightness bins are code values, not inferred luminance.
Highpass/IDWT-stage attribution requires further reference work.

Binary buffers and AVI files stay in the ignored working directory. The public
JSON contains hashes, settings and statistics, without corpus paths. A stability
failure writes its measured hashes then exits nonzero; pixel differences alone
are observations, not test failures or proof of a codec defect.

See `docs/FFMPEG_VS_GOPRO_DECODER.md` for the measured scope and remaining controls.
