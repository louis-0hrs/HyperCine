<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# macOS decoder readiness — P7.A

Issue [#47](https://github.com/louis-0hrs/HyperCine/issues/47), 2026-10-16.
Base: `5c87391` (protected main snapshot; decoder snapshots are pinned below).
Scope: read-only architecture audit and independent public-fixture baseline.
**P7.A audit is ready for review; P7.B implementation remains gated by
#13/#14/#16. No native HyperCine CPU decoder is qualified by this report.**

## Consolidated review status (2026-10-18)

The P7.A audit from #83 and verifier fixes #136/#150 are consolidated on main
base `e094682266b75151ab1a6f3ed2741ca3088c627e`. The inventory below remains a
historical, source-pinned audit. Main now also contains the private synthetic
`decode_task_graph` from #155; it supplies scheduling infrastructure, not CFHD
parsing, inverse arithmetic or a native CPU decoder.

The public baseline remains an independent FFmpeg result for 24 packets and
24 YUV422p10 frames. Its compressed/frame/plane hashes are unchanged. Evidence
comparison rejects duplicate JSON keys, nonstandard constants and numeric type
aliases; frame geometry rejects booleans and noninteger values.

P7.B remains blocked by #13/#16 safety and shared-core integration plus qualified
matched original-SDK output semantics. #14 is closed, but that does not clear the
other gates. The matched 1080p packet discussed in #161 is available to its owners;
its original-SDK nondeterminism and output hash-domain reconciliation prevent it
from serving as an unqualified pixel oracle. No private corpus is required for
this public-fixture audit. No decoder implementation or Metal work is included.

The consolidation's native validation and portable commands are recorded in
[the P7.A integration report](validation/macos-decode-p7/consolidation.md).

## Frozen source inventory

These remote-tracking snapshots were read without checking out or changing their
branches. Findings apply to these revisions, not future PR updates.

| Ref | Exact revision | Relevant evolution |
| --- | --- | --- |
| `decode-phase3af` | `9eb9e3d5e39aceb4cf8b17083a83e431272a1aaf` | Decoder facade, parser and CUDA pipeline |
| `decode-phase3b0` | `7296f6f3b60ae4d9b902415f1a531f20f4ba70ad` | Profile/parser inspection baseline |
| `decode-phase3b1` | `f1d0fe4258ad70113a9de57e85ab9834d29312cc` | Interlaced GOP1 flag and parser handling added |
| `decode-phase3b2r` | `f433a7491b12b7e053fec0bab48b4ba784e7d3d9` | Same inspected facade/parser as b1; broader temporal research |
| `decode-phase3b2a` | `1d78fc183ea28c4268345521e83fb2bca3226381` | FIELDPLUS/GOP2 capability, frame selection, separate fieldplus parser |

The last revision is the detailed dependency reference below. Decoder files
are absent from current main's SDK-free standalone encoder target. These older
refs are architectural evidence, not an approved integration base.

## Dependency and build matrix

| Layer / files (relative to audited decoder ref) | Dependency / native ARM64 finding | Next gate |
| --- | --- | --- |
| `include/hypercine/decoder.h` | Standard C++ types and pimpl; `DecodeConsumerStream` hides a borrowed native handle. Output remains device-oriented. | Reuse approved future facade; specify host surface ownership explicitly |
| `src/core/decode_parser.cpp`, `decode_fieldplus_parser.cpp`, `decode_fieldplus.h`, `decoder_backend.h` | C++ tag/length/profile parsing; descriptors originate in `decode_entropy_cuda.h`. Header provides opaque CUDA typedefs when CUDA is disabled. | Separate portable parser/types through reviewed mainline seam; fuzz lengths/geometry/band bounds |
| `src/core/decode_dispatch.cpp` | Integer workload classification on band records; no reconstruction implementation | Preserve classification semantics; do not interpret CPU compilation as decoding |
| `src/core/decode_thread_pool.cpp` | Standard atomics/mutex/thread worker pool; public declarations still belong to historical decode pipeline headers | Reuse bounded task lifetime and join/error invariants |
| `src/core/decoder.cpp` | Facade owns `CudaDecoderPipelinePhase2E`; conditionally calls CUDA runtime APIs | Requires a real CPU reconstruction backend and approved backend lifecycle seam |
| `src/cuda/decode_*` `.cu` files | CUDA entropy/checkpoint/peak restoration, inverse wavelet and color kernels; native CUDA unavailable on Apple ARM64 | Scalar CPU exactness first; Metal is a later independent proposal |
| `src/cuda/decode_phase2e_stub.cpp` and other stubs | `Initialize` returns false; queued decode returns zero. `tests/test_decoder_cpu.cpp` explicitly expects `BackendUnavailable`. | Stub success tests establish fail-closed API behavior only |
| Decoder `src/CMakeLists.txt` | `HyperCine::Decoder` facade links `hypercine_wavelet`; non-CUDA archive still includes `cpu/upstream_wrapper.cpp` plus stubs | Split SDK-independent decode components after #16, not by copying a second parser |
| `cpu/upstream_wrapper.cpp`, immutable SDK | Legacy spatial/allocator headers and unconditional x86 intrinsics | Native ARM64 SDK build is blocked; no intrinsic emulation dependency proposed |
| `tests/decode_phase0_support.h`, later oracle helpers | GoPro SDK encoder/decoder and internal SDK class references, aligned test buffers | Generate/verify original SDK reference output on qualified supported platform |
| Main standalone target | Portable CPU encoder, optional encode-only Metal; returns before legacy SDK targets | Encoder tests can protect current main, but cannot prove decoder readiness |

Native compile-only experiment: extracted the exact b2a `include/hypercine/`
and listed core translation units to external scratch, then ran Apple Clang
21.0.0 `-std=c++17 -arch arm64 -fsyntax-only` without CUDA defines. All five
core `.cpp` files above passed. This establishes syntax/header portability,
**not linking, execution, sanitizer safety or a functioning CPU backend**.
Separately, `clang++ -arch arm64 -D_XMMOPT=0 -fsyntax-only
upstream/Codec/DemoasicFrames.cpp` failed at the x86-only intrinsic-header
barrier. Disabling `_XMMOPT` does not repair unconditional includes. Upstream
was untouched. No full legacy decoder build or CUDA test ran on this Mac.

## Real public fixture and independent decode

No video bytes were added or copied into this PR. Input is the existing SDK
example `upstream/Example/videos/CFHDsample422.AVI`, preserved with its upstream
provenance/licenses. Its SHA-256 is
`18baa60e401a9ebddddb5691d1c23c48ab1e26bc6f6cbc1aaa5b00869a2ccb44`.

The checked-in [baseline](validation/macos-decode-p7/public-sdk-fixture.json)
records all 24 compressed packet SHA-256 values, packet sizes/timestamps/flags,
and all 24 decoded frame SHA-256 values in order, plus separate Y/U/V plane
hashes and first/last samples. Probe result: CFHD, 1920×1080,
`yuv422p10le`, 24 frames, container rate `24000/1001`. Raw layout is top-down,
unsigned little-endian 10-bit samples in 16-bit words, Y/U/V order, tight
plane pitches 3840/1920/1920 bytes; each frame is 8,294,400 bytes. These are
**FFmpeg raw-output pitches**, not CFHD packet stride fields. All samples were
checked to be at most 1023; complete sequence output was 199,065,600 bytes.

Environment: native macOS 26.5.2 / ARM64; Apple Clang 21.0.0; independently
built FFmpeg n8.0 source revision
`140fd653aed8cad774f991ba083e2d01e86420c7`; `file` identified its executable
as Mach-O arm64. Decode explicitly selects `cfhd`, one decoder thread,
passthrough timestamps and `+yuv422p10le` (no automatic pixel conversion).
FFmpeg binary SHA-256: `862780cb3dd03217066625af7b21ed58230260b09280ed051553471f786c559a`.
The oracle uses a temporary raw file outside the repository, checks exact
frame count/size and deletes it afterward. Runtime is bounded to 120 seconds;
input and requested output frame count are fixed.

There are 23 distinct decoded hashes across the 24 ordered frames; the baseline
preserves the actual repeated frame rather than assuming every frame is unique.
No stream timecode tag or field-order value was returned by this probe.
Container frame rate is not claimed to be packet-derived. Keyframe flags do
not establish the codec's internal GOP/wavelet/profile/FilmScan quality.
The public example is therefore **not a qualified progressive FS3 GOP1
acceptance fixture**. Original SDK pixel output for these identical packets
in matching output semantics is unavailable here: `PENDING_MATCHED_ORIGINAL_SDK_OUTPUT`.
`PlanarYUV422Signed16`, SDK packed `YU64`, BGRA8 and FFmpeg unsigned planar
p10 cannot be compared as raw byte arrays. Range, chroma placement, rounding,
row order, signed offsets and precision require an explicit tested mapping.
No Windows/macOS pixel-parity claim follows from compressed-packet equality.

## Proposed host frame contract (design only)

After the safety/mainline gates, extend the approved shared API rather than
adding an independent Mac parser or public decoder now. A host `FrameDesc`
needs width/height, explicit unsigned YUV422p10-in-16-bit format, byte order,
range/matrix/chroma siting and top-down orientation. Each plane needs pointer,
valid byte count, pixel dimensions and padded byte pitch; checked multiplication
must validate allocation bounds. Keep internal signed coefficients separate
from display samples. Alignment is at least 16 bytes for codec surfaces;
FFmpeg's tight oracle layout must not be passed to a codec without checking
alignment/padding.

Output ownership should follow ticket/session lifetime: borrowed planes stay
valid until release, or an explicit owned-frame copy outlives the session.
Reset must drain work, invalidate old tickets and clear frame-dependent state;
thread-affinity/concurrent-call behavior and sticky failures need specification.
A future Metal surface must expose its own readiness/consumer synchronization,
with CPU vs device memory explicit. A borrowed CUDA event cannot become a
Metal completion token by reinterpretation. Metadata availability should be
per-frame and distinguish packet values from container timestamps.

First implementation acceptance target remains one original SDK progressive
1920×1080 YUV422 p10 GOP1 FS3 packet, then ordered/random/cold/warm sequences
of 16/120 frames. Require matched original SDK pixels, independent FFmpeg
triangulation, all 30 bands' quant/scale/coefficient behavior, malformed input
bounds, resource caps, ASan/UBSan and native/hosted cross-platform tests.
Interlaced, FIELDPLUS/GOP2, RGB/RGBA p12 and 4K/8K remain outside that first
CPU gate. Metal decoding is not implemented by existing encoder kernels.

## Reproduction and validation

From this audit branch, set local `FFMPEG` and `FFPROBE` variables to native
executables. Do not publish their machine-specific values.

```sh
cmake -S tests/macos_decode_portability -B build-p7 \
  -DFFMPEG_EXECUTABLE="$FFMPEG" -DFFPROBE_EXECUTABLE="$FFPROBE"
ctest --test-dir build-p7 -C Release --output-on-failure

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_ENABLE_METAL=OFF \
  -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON \
  -DHYPERCINE_FFMPEG_EXECUTABLE="$FFMPEG" \
  -DHYPERCINE_FFPROBE_EXECUTABLE="$FFPROBE"
cmake --build build --parallel 4
ctest --test-dir build -C Release --output-on-failure
```

The independent audit suite requires FFmpeg/ffprobe and fails if unavailable.
It compares the current compressed bytes and ordered decoded output to the
frozen baseline; changing the oracle version does not waive pixel mismatches.
Its plane-contract unit cases cover endian/pitch/order/endpoints, truncation,
extra data, odd/zero geometry and values outside 10-bit precision. No decoder
production files, root CMake registration, existing tests or upstream files
were changed. Full standalone encoder CTest and the separate audit CTest
both passed locally: **28/28 standalone** and **2/2 audit**, respectively
(76.31 s and 2.61 s). This audit contains no acceleration claim and
runs no codec performance benchmark.

Next action: review this P7.A artifact; after #13/#14/#16 approval, obtain a
matched original SDK pixel fixture and design the shared host backend seam.
Keep #47 open for P7.B.
