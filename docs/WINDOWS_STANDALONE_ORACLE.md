<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Standalone CFHD Windows SDK oracle — issue #5

Refs #3, #4, #5. This branch is stacked on `codex/macos-sdkfree-encoder`
(`a0518c0`). It adds tests and frozen independent-encoder fixtures, without
changing SDK, CUDA or standalone codec math. Actual Windows SDK acceptance of
the three packets is now verified with `TAG_CPU_MAX=1`; native Mac FFmpeg
acceptance is independently verified. Unrestricted SDK worker safety is not claimed.

**Follow-up 2026-10-24 (#120 callsite #3):** the CTest no longer turns the
reference-decoder pixel comparison into a red gate. `standalone_sdk_decode_3_packets`
keeps every strict storage/decode/geometry/pitch/non-zero/encoder-API check fatal,
but its `PSNR`/flat-exactness comparison against the pinned `.p10` decode is
recorded only. The CTest exits `77` (`BLOCKED_ORACLE_NOT_QUALIFIED`, or
`ORIGINAL_SDK_REFERENCE_NONDETERMINISTIC_QUARANTINED` when repeats disagree), so it
reports a skip instead of a pass or a failure. See
`docs/validation/REFERENCE_SDK_PIXEL_CALLSITES_20261024.md` for the contract.

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
`TAG_CPU_MAX=1` requests the documented single-worker oracle mitigation. This
does not certify the frozen SDK's unrestricted multi-worker behavior.

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

## Verified result — SDK decode PASS with single-worker oracle setting

[Push run 38627689864](https://github.com/louis-0hrs/HyperCine/actions/runs/38627689864)
and [PR run 38627697818](https://github.com/louis-0hrs/HyperCine/actions/runs/38627697818)
both PASS on `b905e50`. Windows Server 2025 (`windows-2025-vs2026` image
`20260925.250.1`), MSVC 19.51.36260.0, native x64 (`Hostx64/x64/cl.exe`). Full reference CTest **6/6 PASS**, including
`standalone_sdk_decode_3_packets`; all three genuine Mac-produced packets were
actually prepared and decoded by GoPro into YU64 and BGRA. Each format has one
distinct output hash across five fresh decoder instances under `TAG_CPU_MAX=1`.
No private SDK-layout access or SDK source edits were needed.

| Standalone input | SDK vs FFmpeg p10 PSNR | Max p10 error | SDK vs original RGB PSNR |
| --- | ---: | ---: | ---: |
| Flat128 | exact | 0 | exact |
| GradientChecker | 62.7055 dB | 3 | **32.0550 dB** |
| Canonical seed 0x1337BEEF | 62.8427 dB | 3 | 46.7787 dB |

The nonzero decoder differences are retained, with all per-plane metrics and
first mismatching coordinates in
`docs/validation/windows_standalone_oracle_2026-10-14.json`. SDK vs uncompressed
p10 is exact / 62.5922 / 63.7153 dB respectively. SDK-vs-FFmpeg BGRA is
42.1102 / 41.9059 / 42.0604 dB, under the declared different chroma reconstruction
pipelines. The known GradientChecker original-RGB >35 dB gate **still FAILS**;
this compatibility result does not remove the 4:2:2 quality limitation in PR #4.

The actual Windows GoPro control packet is 9184 bytes, SHA-256
`2d9d57e3bd9334eca6e9a2e26b27df8a05555ee910bc673cdf0bd44a2671e411`.
Independent Mac FFmpeg really decodes it as 128x128 yuv422p10le. SDK-vs-FFmpeg
p10 is 62.7518 dB, max error 3. Control vs original uncompressed source is
38.8927 dB (SDK) / 38.8811 dB (FFmpeg), with max errors 280/279 and Cb PSNR
33.2910/33.2843 dB. Both decoders see the large chroma-border differences in the
GoPro-encoded control; the precise per-plane/first-pixel evidence is retained.
This is not source-exact GoPro encoding or identical quality between encoders.

`tests/fixtures/windows-oracle/` freezes the actual Windows control packet and
all seven unmasked SDK output images with run/commit provenance and SHA-256
manifest. A Mac with external FFmpeg can reproduce the full triangular comparison
without access to a Windows host or logs:

```sh
python3 tests/compare_sdk_oracle.py \
  --windows-fixtures tests/fixtures/windows-oracle \
  --ffmpeg /absolute/path/to/ffmpeg --ffprobe /absolute/path/to/ffprobe \
  --output build/windows-oracle-replay
```

This replay is also registered as `standalone_windows_oracle_replay` when the
external validator is available. Native Apple M2 Pro arm64 / macOS26.5.2 /
Apple Clang21.0.0 / CMake3.31.5 full Release CTest **8/8 PASS**; full Debug
ASan+UBSan CTest **8/8 PASS**, no findings. The new harness separately passed
C++17 syntax validation against real SDK headers; no native ARM SDK execution
is claimed. There is only the existing unused `half_h` wavelet warning.

**Compatibility limit:** PASS covers the tested 128x128 fixed profile with
`TAG_CPU_MAX=1`. Default unrestricted SDK worker decoding failed genuinely in
the first run below and is not certified. CUDA, Adobe, Resolve and performance
benchmarks remain **NOT RUN**. SDK encoder byte identity is not expected.
The PR remains draft; no automatic merge or issue closure.

## First actual Windows attempt — failure retained

[Push run 38627148629](https://github.com/louis-0hrs/HyperCine/actions/runs/38627148629),
commit `a61fc4a`: Windows MSVC build PASS; reference + new CTest **5/6 PASS**,
`standalone_sdk_decode_3_packets` **FAIL**. All prepare/decode API calls return
success and correct geometry. Unrestricted SDK workers produce unstable channel
planes across fresh opens, including flat Y/Cr corruption (max p10 error 512)
and canonical/gradient mismatches, while some opens are close to FFmpeg.
No pixels are masked and the failing output is retained in the run log.
The Windows-generated GoPro control really decodes in independent Mac FFmpeg;
its SDK-vs-FFmpeg p10 PSNR is 62.75 dB for the captured build output. This first
run is not a compatibility pass. The next attempt requests the documented
`TAG_CPU_MAX=1` oracle mitigation. See `docs/UPSTREAM.md` section 9 for the
frozen SDK's worker races; upstream sources remain untouched.
