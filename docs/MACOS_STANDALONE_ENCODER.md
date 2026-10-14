<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Native SDK-free encoder vertical slice — issue #3

The experimental `hypercine_standalone` target produces real, independently
decodable CFHD packets on Apple Silicon. Its core uses C++17 and the standard
library only. `upstream/` is neither compiled nor linked nor present on its
include path. The existing SDK/CUDA targets, algorithms, and APIs are unchanged.
This is a functional fixed-profile slice, not SDK byte parity, a general-purpose
encoder, an acceleration milestone, or a claim of Adobe/Resolve certification.

## Dependency cut

| Existing component | Action | Remaining dependency / validation |
| --- | --- | --- |
| `canonical.h` frame generators | Reuse | SDK-free; copy input to explicitly aligned staging storage. |
| `cpu/wavelet_cpu.cpp` 1D scalar filters | Reuse | No SDK includes. Constant, affine, impulse and three-level vectors tested; complete reconstruction validated with external FFmpeg. Windows coefficient parity pending. |
| CUDA `hybrid_kernels.cu` frontend/pyramid | Extract conventions, keep existing code unchanged | CUDA types, device storage, inverted BGRA orientation, global midpoint state. New CPU frontend uses documented top-down/centered chroma instead. |
| `entropy_stages.cpp` token/VLC/pack | Replace SDK plumbing with small host packer | Existing `LoadEntropyTables`, band framing and swap helpers depend on encoder, transform, `vlc.h`, and bitstream types. New path has no such declarations. |
| `phase3a_encoder.cpp` quant/rate control | Replace with fixed FS3-Q2 experimental profile | Existing `SampleEncoder`, encoder/transform state, FSratelimiter and prior-frame size feedback remain in the original path. No dynamic rate control in this slice. |
| Phase3A sample assembly | Replace | New checked, big-endian tag/chunk writer; fixed frame number; no GUID, timestamp or SDK metadata allocation. |
| Table17 codewords | Extract fixed data only | 256 magnitude entries, seven run entries, band end from frozen `Common/table17.inc`; attributed in source and `THIRD_PARTY_NOTICES.md`. No internal SDK structures copied into the API. |
| SDK decoder / Windows oracle | Validation only | Not required to build or run the Mac encoder. Pending external validation. |
| FFmpeg | Optional separate validation executable | Never an encoder implementation or linked core dependency. |

## Profile and conventions

* Exactly 128x128, progressive, one frame / GOP, 10-bit YUV 4:2:2.
  `EncodeBgra128` accepts top-down BGRA8 with ignored alpha. The input pointer must
  be 16-byte aligned, positive byte stride >=512 and divisible by 16, and the
  supplied storage length must cover the last pixel. Invalid bounds throw before
  any pixel access. `Plane` uses 32-byte aligned RAII storage and an int16 stride
  padded to 16 bytes. Public stage types are experimental.
* BT.709 computer RGB to studio-range p10: Y nominally 64..940, Cb/Cr around 512.
  The implementation uses explicit 16-bit fixed-point coefficients, rounding
  once per luma pixel and once after box-averaging each horizontal chroma pair.
  CFHD channel order is Y, Cr, Cb. No vertical inversion or hidden range selection.
  This is deliberately documented separately from the CUDA frontend's orientation
  and cosited chroma convention; upstream RGB coefficient identity is unproven.
* Three separable 2-6 forward levels; lowpass sums adjacent samples, highpass
  uses the existing six-tap filter and one-sided border filters. Prescale is
  **0,2,0** for p10. At level 2, highpass taps use `(sample+3)>>2`, while horizontal
  lowpass uses `(a+b+3)>>2`. Native constant reconstruction caught and corrected
  an early mistaken 0,2,2 setting, which belongs to the 12-bit path.
* **FS3-Q2** is a fixed experimental profile: quality tag 6 (FilmScan3), lowpass
  divisor 1, **all 27 highpass divisors 2**, no prequant midpoint or rate control.
  It uses FS3's low-frequency divisor as a deliberately simple uniform setting;
  it does **not** reproduce the SDK's full per-band quant tables or dynamic
  FSratelimiter. Quantization uses the SDK-style unsigned reciprocal
  `floor(abs(v)*floor(65536/q)/65536)`, then restores sign. No lossless claim.
* Table17 (coding flag 1), cubic companding before magnitude lookup, sign bit
  0 positive / 1 negative, signed magnitudes clamped to +/-1023. Cubic inverse
  mapping uses the last index whose expanded magnitude is <= input magnitude.
  Zero runs use the largest available run repeatedly plus one-zero codes; this
  is legal but does not claim identical SDK composite-run token choices.
* Subbands are emitted coarse to fine: final LL then LH/HL/HH for each level,
  numbered 0..9 per channel. Widths are multiples of 8, with no implicit row-gap
  coefficients. Bands have end code `0x03114BA3`/26 bits and zero padding to a
  32-bit word. All payload words, tag/value pairs and channel sizes are big-endian.
  Size chunks count 32-bit words excluding themselves and are marked optional
  using the signed-negative tag convention. Channel index lengths exclude channel
  headers, matching the reference writer. The base frame wavelet is type 5;
  upper wavelets are type 3; spatial-count tag is 2 for three total levels.
* Lowpass coefficients are unsigned big-endian 16-bit values. Fixed headers
  carry dimensions, encoded format, color space, precision, progressive flags,
  quality, frame number 0, prescale and essential band metadata. No variable
  metadata needs masking for the standalone output.
* Capacity is checked before each entropy byte write. Each coefficient emits
  at most 27 bits (including sign), run codes cover >=1 zero and use <=13 bits,
  and the band end is 26 bits. `4*(N+1)` bytes safely exceeds the padded bound
  `4*ceil(31*(N+1)/32)` for N>=1. Tests exercise exact capacity, forced overflow,
  extreme signed values, and a full dense 128x128 band.

## Reproduce the native build

Measured on 2026-10-14, Apple M2 Pro, arm64, macOS 26.5.2 (25F84), Apple Clang
21.0.0 (clang-2100.1.1.101), CMake 3.31.5, Unix Makefiles. Ninja is not installed
and is not required. CMake >=3.25 is required for this new build path.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_BENCHMARKS=OFF
cmake --build build --parallel 8
ctest --test-dir build -C Release --output-on-failure
file build/hypercine_encode_smoke build/test_standalone
otool -L build/hypercine_encode_smoke
build/hypercine_encode_smoke flat build/flat.cfhd
build/hypercine_encode_smoke gradient build/gradient.cfhd
build/hypercine_encode_smoke canonical build/canonical.cfhd
```

Both binaries are `Mach-O 64-bit executable arm64`. The encoder links only
`libc++.1.dylib` and `libSystem.B.dylib`. The compile database contains only the
three standalone sources, CLI and tests, with no upstream or CUDA include path.
The existing Windows SDK reference workflow is unchanged. A proposed separate
SDK-free macOS/Windows/Linux workflow could not be pushed because the available
OAuth credential lacks `workflow` scope; it is not part of this PR. Native
standalone validation is local, while the existing Windows reference CI can
check regression after the branch is pushed.

## Independent decode and quality gates

If `ffmpeg`, `ffprobe` and Python 3 are available, the external decoder CTest is
registered automatically. Without them the five native unit/stage tests still
run and CMake explicitly reports that the external test is not registered.
To use specific validation binaries:

```sh
cmake -S . -B build \
  -DHYPERCINE_FFMPEG_EXECUTABLE=/absolute/path/to/ffmpeg \
  -DHYPERCINE_FFPROBE_EXECUTABLE=/absolute/path/to/ffprobe
cmake --build build --parallel 8
ctest --test-dir build -C Release --output-on-failure
python3 tests/validate_standalone.py --encoder build/hypercine_encode_smoke \
  --ffmpeg /absolute/path/to/ffmpeg --ffprobe /absolute/path/to/ffprobe \
  --output-dir build/samples
```

The validator wraps each **HyperCine-produced** raw packet in a small test-only
AVI without transcoding. FFprobe checks codec, dimensions and pixel format;
FFmpeg decodes one frame with `-xerror`. The test checks all 32768 p10 samples
against separately calculated, uncompressed YUV422 planes, requires >35 dB,
and requires exact neutral-flat reconstruction. The RGB check excludes alpha,
uses explicit BT.709 limited->full conversion with bicubic chroma reconstruction,
and requires >35 dB for flat and canonical. It also converts the uncompressed
reference planes to RGB to isolate subsampling loss from codec loss.

The local independent validator is unmodified FFmpeg n8.0, commit
`140fd653aed8cad774f991ba083e2d01e86420c7`. It was built outside the repository:

```sh
git clone --depth 1 --branch n8.0 https://github.com/FFmpeg/FFmpeg.git /tmp/hypercine-ffmpeg-validator
cd /tmp/hypercine-ffmpeg-validator
./configure --disable-everything --disable-doc --disable-network \
  --disable-autodetect --disable-asm --disable-audiotoolbox --disable-videotoolbox \
  --enable-ffmpeg --enable-ffprobe --enable-decoder=cfhd,rawvideo \
  --enable-demuxer=avi,mov,rawvideo --enable-muxer=rawvideo \
  --enable-encoder=rawvideo --enable-protocol=file \
  --enable-filter=scale,format --enable-swscale
make -j8
```

Only the rawvideo output encoder is enabled in this validation binary: CFHD is
**decoder-only**. None of these sources or libraries is shipped with HyperCine.

## Actual results and limits

Release stage suite: **6/6 PASS**, no skips. The stages are color (including
bounds, alignment and padded stride), wavelet (three-level constant, affine and
hand-calculated impulse vectors), quantization (all int16 values), VLC/capacity,
complete sample framing/determinism (10 repeat encodes per pattern), and external
decode. The full SDK reference tests are **NOT RUN** in this SDK-free configuration;
they remain separate Windows gates rather than fake passes or skipped CTests.

Debug with AddressSanitizer + UndefinedBehaviorSanitizer: **6/6 PASS**. Reproduce
in `build-sanitized` with the same standalone flags, `-DCMAKE_BUILD_TYPE=Debug`,
`-DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wpedantic'`
and `-DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'`, then build and run the
full CTest suite. One pre-existing unused-variable warning in `wavelet_cpu.cpp`;
no new source warnings or sanitizer findings. No performance claim or benchmark.

| Input | Packet bytes | YUV p10 PSNR | Max p10 error | Source RGB PSNR |
| --- | ---: | ---: | ---: | ---: |
| Flat128 | 3444 | exact | 0 | 42.11 dB |
| GradientChecker | 25604 | 60.73 dB | 5 | **31.60 dB** |
| Canonical, seed 0x1337BEEF | 15416 | 61.27 dB | 4 | 40.52 dB |

**Quality limit:** GradientChecker does not meet a >35 dB original-RGB threshold.
Its high-frequency colored XOR edges lose horizontal chroma information in
4:2:2. This result must not be represented as a universal FilmScan3 RGB quality
pass. The uncompressed-422 baseline is already only 31.73 dB before any wavelet or
quantization. The JSON measures this separately; YUV
coding quality and canonical RGB quality pass the stated gates. The profile is
experimental and limited to the tested input contract.

[Machine-readable evidence](validation/macos_standalone_2026-10-14.json) includes
packet and decoded-frame SHA-256 values and the subsampling baseline. These are
Mac/FFmpeg measurements, **not Windows SDK golden fixtures**. Raw samples,
validation JSON and decoded frames are reproducible under `build/samples`.

Windows SDK oracle, Windows CPU regression, CUDA regression, Adobe Premiere /
After Effects, and DaVinci Resolve are **pending external verification**. No
bit-identical-to-SDK or commercial-application compatibility assertion is made.
`upstream/` is unchanged in both worktree status and the branch diff.

## Windows oracle handoff and later Metal boundary

On Windows/x86, build the unchanged SDK reference path separately. Generate the
same three `.cfhd` packets with this standalone target; decode each packet using
`CFHD_PrepareToDecode` / `CFHD_DecodeSample` with aligned input/output buffers,
active metadata `TAG_PROCESS_PATH=0xffff`, fixed pixel format and explicit matrix /
range. Check 128x128 and compare p10 planes first, then RGB with declared stride
and orientation. Run sequentially with a fresh decoder before threaded stress;
repeat output hashes to expose documented SDK decoder races/dithering. Preserve
all mismatches and the raw decoded values rather than accepting whichever
orientation/hash happens to match.

For SDK-encoded comparisons, independently parse and retain the original packet,
then normalize only variable metadata fields (GUID, date/time, frame identifier)
into a **separate comparison copy**. Compare headers, quant/prescale and each
lowpass/highpass payload separately. This profile's fixed uniform Q2 and chroma
sampling differ from the dynamic SDK path, so whole-packet or coefficient byte
identity is not an acceptance claim. First match parameters and conversion
conventions before attempting any coefficient/payload identity comparison.

A later Metal backend can replace `ConvertBgra128` / `ForwardLevel` and produce
the same aligned, signed `Plane` / `WaveletLevel` values. Quantized subbands and
checked host VLC/framing remain the host boundary. Do not pass SDK encoder or
transform structures through that API. Metal kernels, NEON, a new decoder, larger
resolutions and production rate control are outside issue #3's implementation.

## Subsequent Windows oracle — issue #5

The original measurement snapshot above predates the Windows oracle. The stacked
issue #5 branch adds genuine SDK decoding of all three unchanged Mac-produced
packets, rather than relying on the four legacy regression tests. Windows x64
MSVC CTest **6/6 PASS** under the documented `TAG_CPU_MAX=1` setting, and a real
Windows GoPro-encoded control also independently FFmpeg-decodes on Mac. See
[the oracle report](WINDOWS_STANDALONE_ORACLE.md) and its run-linked JSON for
returned API statuses, unmasked pixels, hashes and per-plane differences.
The first unrestricted-worker attempt failed with actual unstable channel
outputs; no default multi-worker SDK safety claim is made. The known original-RGB
GradientChecker quality limitation and pending CUDA/Adobe/Resolve gates remain.
