<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Experimental Metal FilmScan3 frontend — Issue #7 P1

Source base: `518ee87204c64c1946904c12a84ea672ca406235` (`origin/main`,
2026-10-15). Isolated branch: `codex/macos-metal-filmscan3-p1`.
Prerequisites: dimensioned SDK-free API [#6](https://github.com/louis-0hrs/HyperCine/issues/6),
FilmScan3 parity [#23](https://github.com/louis-0hrs/HyperCine/issues/23), and
independent decoder gate [#5](https://github.com/louis-0hrs/HyperCine/issues/5).
CPU basis incorporates PRs #4/#22/#25/#26, Windows oracle #9, exporter #19,
and standalone CI #28. This is a functional P1 draft, with no speedup,
4K/8K, other quality profile, Premiere/Resolve, or release qualification claim.

## Architecture and arithmetic

`FilmScan3Encoder` uses an internal `detail::Frontend` for color conversion and
one forward wavelet level. Default construction and `FilmScan3Backend::Cpu`
use the original scalar mathematics. `FilmScan3Backend::Metal` requires a real
device, queue, compiled MSL library and all three pipelines; failure throws.
`MetalOrCpu` explicitly permits CPU fallback **only at initialization**.
A failed dispatch propagates and the encoder commits sequence state only after
assembling a complete packet. Fallback is tested independently through an
injected unavailable device/pipeline factory. GPU-required tests always create
the real Metal frontend and fail rather than skip or fall back.

Portable public headers expose a backend enum and an incomplete internal
frontend pointer, with no Metal/Objective-C types. `src/metal/filmscan3_metal.mm`
is compiled with ARC. Device, queue, pipeline, command and buffer ownership is
bounded by C++/ARC lifetimes and per-call autorelease pools. Independent encoder
instances should be used by different callers; concurrent use of one stateful
encoder is unsupported. The default-OFF `HYPERCINE_ENABLE_METAL` option requires
Apple and `HYPERCINE_BUILD_STANDALONE=ON`. CPU-only builds never enable ObjC++,
include Metal headers, or link Apple frameworks.

The actual `src/metal/filmscan3.metal` source is embedded at CMake configuration
and compiled by `newLibraryWithSource` on the device. Editing it triggers CMake
reconfiguration. This uses the native OS Metal compiler and does not require an
offline `xcrun metal` installation (that utility is absent on the test host).
All kernel arithmetic is integer; no float approximation is involved.

- CG709 reproduces negative floor division, intermediate signed saturation,
  explicit 16-bit wrapping, vertical input inversion, even-X chroma sampling,
  and Y/Cr/Cb plane order. Chroma is not averaged across pixel pairs.
- Horizontal/vertical 2-6 filters reproduce both six-tap borders and the
  separately rounded interior term. Prescale is 0/2/0. At prescale 2, highpass
  taps round individually while lowpass rounds the original pair sum.
- Plane storage uses the existing 32-byte allocator and strides padded to
  16 bytes. Input pointers, storage extent, pitches, indexing capacity, plane
  allocation budgets and Metal buffer limits are checked before dispatch.
  All output padding is initialized and included in comparisons.
- A horizontal dispatch and two vertical dispatches produce LL/LH/HL/HH.
  Dependent passes use separate command encoders and tracked shared buffers.
  Each level waits for completion before returning readable planes.
- Existing host quantization, rate control, Table17/VLC, metadata and framing
  code produces the packet. No reference coefficients, tables, headers or
  bitstreams are injected into the encoder. Golden metadata injection supplies
  only the four existing explicit variable metadata values.

The fixed-Q2 encoder, upstream SDK, CUDA sources and reference fixtures remain
unchanged. The FilmScan3 encoder domain remains **1920×1080 BGRA8, progressive
YUV422 p10, GOP1**, within the existing 2,097,152-pixel allocation budget.

## Hard gates and reference data

`test_metal_filmscan3` compares every stored int16 value (including padding) in
all three color planes and four unquantized bands at every L1/L2/L3 stage.
Its 39 pattern/geometry cases cover all eleven canonical archetypes, impulse,
alternating rows, asymmetric pixels, pitch padding and borders, at 128×24,
384×40 and 1920×1080. A further eight full-range signed-int16 wavelet cases
cover minimal six-tap dimensions, padded widths, saturation and prescale 0/2;
270 input rows also exercise 135-row output bands. Odd *input* wavelet heights,
invalid prescale and incomplete/misaligned storage are rejected. Repeated
encode/reset and repeated device/queue/pipeline creation are exercised.
Stage failures report `plane,level,band,x,y,cpu,metal`; packet failures identify
the first differing byte.

The separate `hypercine_encode_filmscan3_metal` validation tool uses the exact
same host CLI/writer as the normal CPU tool, with a mandatory GPU frontend
wrapped by a scalar stage auditor. For **each** 1080p frame, all three color
planes and all 36 unquantized bands (3 channels × 3 levels × 4 bands) compare
before returning data to quantization. No mismatch tolerance is permitted.

Existing `validate_filmscan3.py` additionally runs both executables on the
**same input files and metadata** and checks identical traces, all 30 quantized
coefficient hashes, and every raw full packet byte/SHA. This direct comparison
is independent of Windows metadata normalization. The existing parser checks
30/30 framed band payloads and complete normalized packet SHA against the
Windows manifest. Optional corpus gates verify actual SDK and CUDA artifact
SHA/length, parse their payloads, require SDK==CUDA normalized bytes, and compare
the generated bytes against those originals. With explicit matching metadata,
generated raw packets also equal the original SDK packets.

The NAS was used read-only at `/Volumes/DeDub/HyperCine/golden/windows/fs3/2026-10-14`.
Archives were independently hashed before extraction to `/private/tmp`:

| Archive | SHA-256 |
| --- | --- |
| 120-frame Tier C | `9667d078ca54dccaf4f849f0136a5a8aae674c2c5e603e29b6cfc225aab21b06` |
| Tier A | `fdc9cd0e881db417e23423c8cd3f9f1e27161f2c946e40edfa2bcb8b56d58dcf` |

The full 16-frame Tier A and 120-frame sequence run independently from initial
state, preserving each sequence's autonomous `FSratelimiter` evolution.
Golden archives are not committed. Reports retain per-frame input SHA, raw CPU
and Metal packet SHA, normalized SHA, quant/scale state and binary inventory.

SDK/CUDA bitstream agreement on these actual 1080p fixtures is distinct from
GoPro's **unquantized** internal stage dumps: the latter remain pending under
#23. The GPU stage oracle here is the autonomous scalar CPU. The upstream
128px SIMD/OOB issue #20/#21 is not reproduced; small stage cases compare
well-defined CPU mathematics only and are not SDK bitstream certification.

## Native validation

Hardware: Apple M2 Pro, 16 GPU cores, native arm64; macOS 26.5.2 (25F84),
AppleClang 21.0.0.21000101, macOS SDK 26.5, CMake 3.31.5, Python 3.9.6.
FFmpeg/ffprobe: existing local n8.0 validation build with actual CFHD/rawvideo
and AVI decode paths, no network or assembly. It is an independent decoder,
not linked into the codec. Hardware/toolchain and complete logs are retained
under [validation/metal-logs](validation/metal-logs).

| Configuration | Full CTest | Elapsed |
| --- | --- | --- |
| Release + actual Tier A/sequence + Metal + FFmpeg | **35/35 PASS**, zero skips | 174.66 s |
| Debug + Metal API/GPU validation + actual corpus + FFmpeg | **35/35 PASS**, zero skips | 684.76 s |
| RelWithDebInfo C++/ObjC++ ASan + UBSan + actual corpus + FFmpeg | **35/35 PASS**, zero skips, no sanitizer findings | 432.59 s |
| Metal OFF, existing CPU-only CI gate | **26/26 PASS**, zero skips | 81.68 s |

The final direct paired-CPU packet comparison and versioned SHA reports are
from Release. Debug and sanitizer full runs cover the same kernels and host
mathematics, including actual raw SDK packets, before that optional validator
comparison was added to their CTest invocations. The final Release suite was
rerun after the validator enhancement. Six existing CI guardrail unit tests
also pass. `git diff --check`, upstream status and upstream diff are empty.

Reports: [16 frames](validation/metal-filmscan3-tier-a.json),
[120 frames](validation/metal-filmscan3-sequence.json),
[FFmpeg](validation/metal-filmscan3-ffmpeg.json).
The actual-corpus reports show 16/16 and 120/120 raw CPU==Metal==SDK packets
with explicit matching metadata; SDK/CUDA normalized originals also compare
byte for byte. Independent FFmpeg p10 PSNR is 75.53/62.01/56.64/59.09 dB for
frames 0/3/6/13.

Debug runs enable `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`; logs confirm
**Metal API Validation Enabled** and **Metal GPU Validation Enabled**.
ASan and UBSan instrument both C++ and Objective-C++; OS GPU drivers and
external FFmpeg/Python are outside that instrumentation. Apple arm64
LeakSanitizer is unsupported, so `detect_leaks=0` is explicit. Separately,
`leaks --atExit -- ./build/test_metal_filmscan3` reports **0 leaks / 0 bytes**
after repeated resource acquisition/release. This is process-exit evidence,
not a long-running GPU-driver leak certification.

FFmpeg independently decodes Metal packets to 1920×1080 `yuv422p10le` and
checks decoded size and PSNR >35 dB for frames 0/3/6/13. Exact encoder hashes
are checked separately from this lossy quality gate. Existing fixed-Q2 frozen
packet and decoded hashes also pass.

Hosted Windows/MSVC, Linux and Mac CPU results for this PR are recorded on the
PR checks. Hosted GPU availability is not assumed; native Metal validation is
local to the real M2 Pro. Windows CUDA runtime was not rerun on this Mac.

## Timing and limits

A Release native run measures the existing complete CPU path and a complete
Metal-assisted path, using the same deterministic 1080p noise input and state.
One warmup precedes five measured encodes; initialization/MSL compilation,
input generation, file I/O and scalar stage auditing are excluded. Per-stage
allocation, upload/readback, synchronization, host quantization and entropy
are included in end-to-end measurements. No measured CPU baseline is borrowed
from a Windows machine.

| Complete encoder | Average latency | Min / max latency | FPS | MPixels/s |
| --- | --- | --- | --- | --- |
| Scalar CPU | 95.2504 ms | 93.5957 / 96.8440 ms | 10.4986 | 21.7700 |
| Metal frontend + same CPU host | 96.0739 ms | 94.1464 / 98.1150 ms | 10.4086 | 21.5834 |

This sample is about 0.86% slower end to end with Metal, within a small
five-frame exploratory measurement; it establishes no performance benefit.
The host remainder averages 85.339 ms. Across eight timed/reset GPU frames,
color execution totals 2.59717 ms, wavelet execution 6.592 ms, upload 31.8961 ms,
readback 7.33963 ms, submission 1.16146 ms and waiting 44.3019 ms.
Peak explicit shared buffers: 16,588,800 bytes; peak device allocations:
17,022,976 bytes. Whole-test peak process RSS: 100,761,600 bytes.
Raw measurement: [stages and timing](validation/metal-logs/stages-and-timing-release.txt).

The raw log also separates GPU color execution, GPU wavelet execution, CPU
upload/zero-fill/allocation, readback, command submission and waiting.
GPU duration overlaps host waiting and must not be summed with it. The reported
host remainder includes quantization/entropy/framing, plane allocation and
orchestration. Shared-buffer byte peaks and `MTLDevice.currentAllocatedSize`
are reported separately from process peak RSS. Apple Silicon uses unified
memory; these are not separate physical RAM/VRAM pools. The process RSS peak
also includes the earlier stage auditor and framework state.

The prototype reads each level back to the host and uploads its LL for the
next level. This deliberately simple P1 has material transfer/allocation/sync
costs and demonstrates no end-to-end speedup. Persistent GPU pyramids and
buffer reuse are future work requiring the same exactness gates.
`benchmark_codec` and `benchmarks/results/reference_windows_msvc.json` measure
the legacy SDK configuration and are not a runnable native arm64 SDK baseline.
No optimization improvement or acceleration milestone is certified here.

## Reproduction

Use an extracted, independently verified corpus and native Apple command line
tools. Set FFmpeg paths to actual installed executables. CPU defaults remain
unchanged if the Metal option is omitted.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_ENABLE_METAL=ON \
  -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=OFF \
  -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON \
  -DHYPERCINE_FFMPEG_EXECUTABLE=/private/tmp/hypercine-ffmpeg-validator/ffmpeg \
  -DHYPERCINE_FFPROBE_EXECUTABLE=/private/tmp/hypercine-ffmpeg-validator/ffprobe \
  -DHYPERCINE_REFERENCE_CORPUS_ROOT=/private/tmp/hc-metal-oracle/tier-a \
  -DHYPERCINE_FS3_SEQUENCE_ROOT=/private/tmp/hc-metal-oracle/sequence
cmake --build build --parallel 4
ctest --test-dir build -C Release --output-on-failure

# Direct stages, mandatory GPU device, and uninstrumented timing:
./build/test_metal_filmscan3
# All six GPU gates; full packet reports appear at build/metal-*.json:
ctest --test-dir build -C Release -L gpu-required --output-on-failure
# Native process leak check:
leaks --atExit -- ./build/test_metal_filmscan3
```

For Debug, use a fresh `build-debug` with `CMAKE_BUILD_TYPE=Debug` and run:

```sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  ctest --test-dir build-debug -C Debug --output-on-failure
```

For `build-sanitized`, use `RelWithDebInfo` plus both
`CMAKE_CXX_FLAGS` and `CMAKE_OBJCXX_FLAGS` set to
`-fsanitize=address,undefined -fno-omit-frame-pointer`, and
`CMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined`:

```sh
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build-sanitized -C RelWithDebInfo --output-on-failure
```

A separate `build-cpu` uses `HYPERCINE_ENABLE_METAL=OFF`,
`CMAKE_EXPORT_COMPILE_COMMANDS=ON`, the same external decode configuration,
and no corpus paths. Build it and run the unchanged CI gate:

```sh
python3 ci/test_standalone_gate.py
python3 ci/run_standalone.py --build-dir build-cpu --system Darwin --machine arm64
```

To select the production-sized experimental path in C++:

```cpp
hypercine::standalone::FilmScan3Encoder encoder(
    hypercine::standalone::FilmScan3Backend::Metal);
auto packet = encoder.Encode(aligned_bgra, byte_count, padded_stride);
```

No auto-merge or issue closure is performed. Review the isolated draft and
native evidence before considering a broader phase.
