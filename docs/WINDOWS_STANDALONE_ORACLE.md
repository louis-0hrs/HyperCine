<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Standalone CFHD Windows SDK oracle — issue #5

Refs #3, #4, #5. This branch is stacked on `codex/macos-sdkfree-encoder`
(`a0518c0`). It adds tests and frozen independent-encoder fixtures, without
changing SDK, CUDA or standalone codec math. SDK compatibility remains **pending**
until the actual Windows test runs; native Mac FFmpeg acceptance is independent.

## Inputs and reproduction

`tests/fixtures/standalone/manifest.json` attributes all fixtures, byte lengths
and SHA-256 values. Three `.cfhd` packets are produced on Apple M2 Pro using PR #4
and match its committed evidence exactly. `.p10` and `.bgra` are FFmpeg n8.0
(`140fd653aed8cad774f991ba083e2d01e86420c7`) decodes; `.input-p10` is the independent
uncompressed reference from `validate_standalone.py`. Binary fixtures are
MIT OR Apache-2.0; they contain generated synthetic images, no third-party media.

The native standalone build registers `standalone_oracle_fixtures`, which verifies
all 12 fixture hashes and regenerates all three packets for byte identity. The
existing external-decode test still genuinely decodes all three. Reproduce:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_BENCHMARKS=OFF \
  -DHYPERCINE_FFMPEG_EXECUTABLE=/absolute/path/to/ffmpeg \
  -DHYPERCINE_FFPROBE_EXECUTABLE=/absolute/path/to/ffprobe
cmake --build build --parallel 8
ctest --test-dir build -C Release --output-on-failure
```

Windows uses the unchanged `CPU Reference CI` workflow. The SDK-enabled build
adds `standalone_oracle_fixture_hashes` and `standalone_sdk_decode_3_packets`,
linked exclusively to the pristine `CFHDCodecStatic` test oracle. No standalone
encoder or FFmpeg dependency is introduced into the reference codec target:

```powershell
cmake -B build-cpu -S . -DHYPERCINE_ENABLE_CUDA=OFF
cmake --build build-cpu --config Release
ctest --test-dir build-cpu -C Release --output-on-failure
.\build-cpu\tests\Release\test_standalone_sdk_oracle.exe `
  tests/fixtures/standalone build-cpu/oracle-manual
```

## Pixel contract and gates

For each fixed 128x128 packet the test opens **five fresh decoder instances for
each output format**, calls the real `CFHD_PrepareToDecode` / `CFHD_DecodeSample`,
and requires actual 128x128 format, padded 512-byte stride, success and nonzero
output. Input and output storage use aligned RAII vectors; decoder, encoder and
metadata handles have custom deleters. `TAG_PROCESS_PATH=0xffff`, explicit BT.709,
computer RGB and disabled SDK chroma filtering are applied with checked statuses.

YU64 is little-endian unsigned p16 `Y0,Cr,Y1,Cb`, top-down. The test converts it to
FFmpeg planar Y/Cb/Cr p10 by right shifting six bits, without hiding any pixel or
choosing a better orientation. It logs all per-plane PSNR/max errors, mismatch
counts and first differing coordinates/values. >35 dB overall p10 comparison
against independent FFmpeg is required; flat must be exact. BGRA is explicitly
bottom-up in the SDK, top-down in FFmpeg. Alpha is excluded. RGB metrics are
**diagnostic**, since FFmpeg uses bicubic chroma reconstruction and the SDK here
uses its declared unfiltered conversion; no decoder bit identity is fabricated.

Whole-output FNV hashes and the number of distinct outputs across repeated opens
expose SDK instability. Instability is reported rather than masked. An all-zero
invalid header must fail preparation. No arbitrary truncated-payload fuzzing of
the frozen legacy decoder is attempted.

The GoPro encoder also encodes the same canonical uncompressed p10 source using
YU64/FilmScan3. Its packet and SDK-reconstructed p10 planes are recorded. SDK
whole-packet identity is not expected: quantization and variable metadata differ.

## Binary evidence from existing Windows CI

The existing workflow suppresses passing test output and has no artifact upload.
With tests enabled on Windows, `standalone_oracle_evidence` is an additional
build diagnostic target (disable with `HYPERCINE_ORACLE_BUILD_REPORT=OFF`). It
prints genuine SDK statuses/metrics and base64 records for unmasked decoded
planes and the GoPro control packet. Report mode prints failures but does not
stop the build; the subsequent independent CTest is the actual pass/fail gate.
This preserves the existing workflow and lets a Mac obtain binary Windows
results without credentials granting workflow edits. There are no secrets or
user media in these generated records.

After Windows Actions completes, download one job's log and run:

```sh
gh run view RUN_ID --job JOB_ID --log > /tmp/windows-oracle.log
python3 tests/compare_sdk_oracle.py --log /tmp/windows-oracle.log \
  --ffmpeg /absolute/path/to/ffmpeg --ffprobe /absolute/path/to/ffprobe \
  --run-url https://github.com/louis-0hrs/HyperCine/actions/runs/RUN_ID \
  --output build/windows-oracle-comparison
```

The script strictly recovers all eight records, validates sizes, calculates
SHA-256 values and independently FFmpeg-decodes the **actual Windows
GoPro-encoded** control. Thus both decoders consume standalone and GoPro packets,
with explicitly different runtimes. It writes full p10 comparison evidence
before any failed quality assertion. It never pretends FFmpeg is GoPro.

## Current result

Initial harness submission: SDK decode **NOT RUN / pending Windows Actions**.
Apple M2 Pro / native arm64 / macOS 26.5.2 / Apple Clang 21.0.0 / CMake 3.31.5:
Release full configured CTest **7/7 PASS**; Debug ASan+UBSan full configured CTest
**7/7 PASS** (no findings). The SDK harness passes a separate C++17 Clang syntax
check against actual SDK headers, without claiming native ARM SDK linkage.
All three packet and FFmpeg plane hashes match PR #4's evidence. Only the existing
unused `half_h` warning in the reused scalar wavelet source was observed.
Windows full reference CTest, SDK statuses and triangular control results must
be filled from actual CI, with run links. CUDA, Adobe and Resolve are NOT RUN.
No throughput/latency optimization or performance milestone is claimed.
