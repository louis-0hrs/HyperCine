<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Extreme-resolution CineForm feasibility: C0

Issue [#48](https://github.com/louis-0hrs/HyperCine/issues/48), audited 2026-10-16.
HyperCine source snapshot: `2a5e94ff6120a7053621c6ab4dcb020c7934e6d4`.
Upstream reference baseline: `11574d0295771edccadd17af14af74a539f924c7`.

**Decision: continue with bounded CPU 4K (#44) and Metal 4K (#45); native
12K/17K output remains unqualified.** The current writer cannot represent more
than 67,108,860 bytes of content in one sized chunk. This is a proven writer
limit, not proof that every legal CineForm sample must be smaller than 64 MiB,
nor proof that every 17K image would exceed it. No extreme-resolution frame was
allocated, encoded or decoded in this C0 audit. No production code changes.

## Geometry and format matrix

All sizes below use MiB = 1,048,576 bytes. Source dimensions are the explicit
workloads requested in #48; they are not measurements of a camera recording.
The p10/p12 columns describe **uncompressed host storage in 16-bit containers**,
not packed 10-/12-bit data, compressed sizes, or supported encoder inputs.

| Workload | Pixels | BGRA8 | YUV422p10le | RGB444p12le | RGBA4444p12le |
| --- | ---: | ---: | ---: | ---: | ---: |
| 3840×2160 | 8,294,400 | 31.64 | 31.64 | 47.46 | 63.28 |
| 7680×4320 | 33,177,600 | 126.56 | 126.56 | 189.84 | 253.12 |
| 12288×8040 | 98,795,520 | 376.88 | 376.88 | 565.31 | 753.75 |
| 17520×8040 | 140,860,800 | 537.34 | 537.34 | 806.01 | 1074.68 |

| Geometry | Current main BGRA8 → YUV422 p10 FS3 | Direct YUV422p10 input | RGB/RGBA p12 output | Original SDK encode/decode | Identical-packet FFmpeg decode |
| --- | --- | --- | --- | --- | --- |
| 3840×2160 | UNSUPPORTED_GEOMETRY | Not implemented | Not implemented | SDK_ORACLE_PENDING | REFERENCE_PENDING |
| 7680×4320 | UNSUPPORTED_GEOMETRY | Not implemented | Not implemented | SDK_ORACLE_PENDING | REFERENCE_PENDING |
| 12288×8040 | UNSUPPORTED_GEOMETRY | Not implemented | Not implemented | SDK_ORACLE_PENDING | REFERENCE_PENDING |
| 17520×8040 | UNSUPPORTED_GEOMETRY | Not implemented | Not implemented | SDK_ORACLE_PENDING | REFERENCE_PENDING |

These are the **audited main snapshot's** statuses. PR #50 is separate,
unmerged CPU 4K qualification; its results are not silently applied here.
`FilmScan3Encoder::Encode` fixes its transform and wire tags to 1920×1080
([writer](../src/core/filmscan3_encoder.cpp)). The generic fixed-quant encoder
also rejects these workloads: `ValidateGeometry` requires width divisible by
128, dimensions ≤4096 and ≤2,097,152 pixels
([frontend](../src/cpu/standalone_frontend.cpp),
[public bounds](../include/hypercine/standalone.h)). This is an implementation
budget, not a codec-standard limit. Bayer, stereo, BRAW import, audio and
container integration are outside C0.

## What the chunk source actually proves

Use the pinned source snapshot when checking the following locations:

* `src/core/filmscan3_encoder.cpp:33–55`: `Writer::Check` limits aggregate bytes
  to `8*kMaxPixels+16384` = 16,793,600 bytes, stricter than the wire chunk cap
  in this snapshot. `Writer::End` computes content words excluding its own
  four-byte tag, requires four-byte alignment and rejects values >`0xffffff`.
* `src/core/filmscan3_encoder.cpp:126–159`: the writer nests band `0x2000`,
  level `0x2100` and sample `0x2200` sizes. The sample size includes metadata,
  channel/level/band headers, payloads and trailers inside it. It excludes
  preceding sample headers/index and the size tag itself. Its content ceiling
  is `(2^24−1)*4` = **67,108,860 bytes**, four bytes below 64 MiB.
* `upstream/Codec/codec.h:368–395`: sized chunks carry 24-bit counts in
  four-byte units; the low eight tag bits supply the high count byte.
  The proposed huge flag / 256-byte unit tags are **commented out**. The nearby
  uncompressed-tag comment is not evidence of a functioning large-sample mode.
* `upstream/Codec/bitstream.c:2197–2278`, `SizeTagPush`/`SizeTagPop`: the SDK
  patches the same size representation. The `size > 0xffffff` branch contains
  only commented-out huge-chunk code. It neither returns an error nor supplies
  a working extension; the subsequent write retains only the low two size
  bytes without adding the high byte in that branch. A successful encode
  return alone must therefore not qualify an oversized SDK chunk.
* `upstream/Codec/decoder.c:2270–2305`, `ParseSampleHeader`: the parser combines
  unsigned low 16 value bits with eight tag bits and uses `chunksize*4` for
  sample traversal. This path supplies no evidence for a huge-unit extension.
* `upstream/Codec/codec.c:754–755` writes width/height tag values.
  HyperCine `Writer::Tag` explicitly rejects values above 65535. All requested
  dimensions fit that representation; this does not qualify transform,
  allocator or parser behavior at those dimensions.

An additional SDK-specific gate exists in `upstream/Codec/encoder.c:2052–2062,
2191–2210`: size limits start at 32768×32768 and are halved when
`BAYER_SUPPORT` is defined, regardless of the selected encoded format at that
point. `upstream/Common/macdefs.h:38` defines that macro. A width of 17520 can
therefore encounter a **16384 build-dependent SDK limit**, despite fitting the
wire field. The Windows oracle must record the actual preprocessor settings
and prepare/encode result; do not transfer a Mac source observation to every
Windows SDK build. The pristine native ARM SDK still has the x86-header build
barrier documented in [the CPU baseline](MACOS_CPU_BASELINE.md).

### Independent FFmpeg source cross-check

Reviewed [encoder](https://github.com/FFmpeg/FFmpeg/blob/3e6cd5e7603cb81b028b9c17530697b8986bf69a/libavcodec/cfhdenc.c)
and [decoder](https://github.com/FFmpeg/FFmpeg/blob/3e6cd5e7603cb81b028b9c17530697b8986bf69a/libavcodec/cfhd.c)
at commit `3e6cd5e7603cb81b028b9c17530697b8986bf69a`.

The encoder requires height ≥32 and width divisible by 16 (lines 249–256).
It writes 16-bit dimensions, pads coded height to eight rows, and uses
32-bit channel index entries. Its sample construction does **not** require
HyperCine's enclosing `0x2200` chunk. Thus the current writer's cap cannot be
declared a universal CFHD packet-size limit solely from these implementations.
Its highpass loop pads band rows to eight coefficients while maintaining run
state across rows, then pads the completed band to four bytes (lines 630–748).
The decoder reads dimensions as unsigned 16-bit values, recognizes large-tag
length syntax and skips small optional chunks (lines 385–418, 509–513).
None of these observations establishes 17K decode success, SDK parity or
application interoperability. Omitting size tags in HyperCine requires its own
GoPro/parser/application review and identical-packet tests; C0 does not do it.

Downloaded source SHA256, respectively:

```text
cfhdenc.c f2e2124a8cd7d3b8dd94df1708d63a378ab45360e2143c4e0ca0167d24774dda
cfhd.c    1603cce321e8bfe9ef5a7a272b62a3795d1de0daf442dbb9ddb671b2f528cf0c
```

## 17K band geometry is not the current entropy contract

| Geometry | Width mod 128 | L3 Y width | L3 Cr/Cb width | L3 height | Padded Cr/Cb stride (int16) |
| --- | ---: | ---: | ---: | ---: | ---: |
| 3840×2160 | 0 | 480 | 240 | 270 | 240 |
| 7680×4320 | 0 | 960 | 480 | 540 | 480 |
| 12288×8040 | 0 | 1536 | 768 | 1005 | 768 |
| 17520×8040 | 112 | 2190 | 1095 | 1005 | 1096 |

For 4:2:2 three-level transforms, L3 luma width is `width/8`, chroma width
`width/16`; **1095 describes chroma, not both luma and chroma**. At 17K neither
2190 nor 1095 is divisible by eight. `PackHighpass` currently requires that
divisibility and scans active width only
([entropy](../src/core/standalone_encoder.cpp)). Padded allocation stride and
coded width are separate concepts. Relaxing `ValidateGeometry` alone cannot
make this valid. Specify the virtual coded row gaps, cross-row zero-run state,
end marker and final 32-bit alignment against actual SDK output first. Preserve
global 2–6 boundaries and integer rounding; independent tile compression is
not one equivalent full-frame sample. Do not crop or silently round width.

## Memory: measured limits, estimated allocations

The optional [device query](../tests/geometry_extreme/metal_limits.m) ran on
Apple M2 Pro on 2026-10-16 without allocating frames. Observed values:

| Property | Bytes |
| --- | ---: |
| Physical memory | 17,179,869,184 |
| `maxBufferLength` | 9,534,832,640 |
| `recommendedMaxWorkingSetSize` | 12,713,115,648 |
| `currentAllocatedSize` at query | 65,536 |

The first sandboxed query had no device (exit 1); the normal host query
succeeded. These values are host observations, not promised limits on other
devices or a peak-memory measurement of an encoder.

The following is a **hypothetical geometry expansion of the current**
`src/metal/filmscan3_metal.mm::Prepare` / `Transform` layout, not an executable
claim. It retains 42 shared buffers: one BGRA upload, three color planes,
36 wavelet bands and two maximum horizontal scratch surfaces. Each int16 row
rounds up to eight elements (16 bytes). Production readback copies 30 bands
(L3 LL and nine highpass bands per channel); audit mode retains more.

| Geometry | GPU shared workspace MiB | Production host readbacks MiB | Caller BGRA + workspace + readbacks MiB |
| --- | ---: | ---: | ---: |
| 3840×2160 | 120.63 | 31.64 | 183.91 |
| 7680×4320 | 482.52 | 126.56 | 735.64 |
| 12288×8040 | 1436.84 | 376.88 | 2190.59 |
| 17520×8040 | 2049.01 | 537.65 | 3124.00 |

The subtotal excludes allocator/vector spare capacity, entropy temporary
`4*(band_pixels+1)`, simultaneous old/new sample vectors, caller-retained
output frames, CPU transform scratch, driver/command/pipeline storage and OS
memory. Unified memory means shared buffers consume system RAM; do not add a
fictional second VRAM copy. Do count actual host `memcpy` readbacks separately.
At 17K the largest planned buffer is the 563,443,200-byte BGRA upload, below
the measured individual-buffer limit. **Neither RESOURCE_EXCEEDED nor a 17K
PASS follows from this.** Workspace replacement builds `next` before releasing
the old pool, so peak admission must include both pools. Failure must preserve
the previous workspace and encoder state.

Tight 17K BGRA stride is 70,080 bytes, stride×height = 563,443,200, below
`UINT32_MAX`. All current candidate surface index products also fit 32 bits.
Arbitrary caller pitch, multiple slots and future larger geometries still need
checked products before narrowing to shader `uint`. The current Metal path
guards input pitch/indexing and `maxBufferLength` per allocation, but has no
aggregate preallocation/admission gate. Its zero-filling allocations must not
be used as an implicit RAM-availability test.

For #44/#45, validate in order: wire dimensions and profile geometry; checked
row bytes and pitch alignment; last-row span `pitch*(height−1)+row_bytes`;
per-plane/band bytes; total workspace and overlap; shader indices; per-surface
device limit; explicit total memory/output budget. Use `size_t`/`uint64_t`
checked multiply/add before converting to `int`, `uint32_t`, SDK sizes or wire
fields. Distinguish the chunk content count from total packet bytes. Fail
before allocations and roll back cleanly. Later pooling/streaming may shorten
band lifetimes, but global wavelet halos and global VLC state must survive.

## Reproducible C0 checks

The [arithmetic model](../tests/geometry_extreme/audit.py) emits the checked-in
[JSON report](validation/extreme-geometry-c0.json). It allocates no pixel
arrays, emits no CFHD packet, performs no hardware probe and has no third-party
dependencies. It models bounds, not actual GoPro/FFmpeg behavior.

```sh
python3 tests/geometry_extreme/audit.py
cmake -S tests/geometry_extreme -B build-geometry
ctest --test-dir build-geometry -C Release --output-on-failure
# Optional macOS device query, no frame allocations:
xcrun clang tests/geometry_extreme/metal_limits.m -framework Foundation \
  -framework Metal -o build-geometry/metal_limits
build-geometry/metal_limits
```

Four unittest cases cover 32-/64-bit arithmetic overflow, 24-bit chunk boundary
serialization/reconstruction, 17K active width versus padded stride, and exact
agreement of the recorded matrix with the model. The boundary test uses a
four-byte synthetic header, not a 64-MiB fixture. This standalone audit CTest
project deliberately leaves the root and P4/P5 build files untouched.

Validation on native macOS arm64, Apple Clang 21.0.0, CMake 3.31.5,
Python 3.9.6: C0 audit CTest **1/1 PASS** (four unittest cases).
The complete configured CPU standalone suite also passed **27/27**, including
its external-decode gates and fixture/hash checks (75.81 seconds):

```sh
# With a working ffmpeg and ffprobe on PATH:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_ENABLE_METAL=OFF \
  -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --parallel 4
ctest --test-dir build -C Release --output-on-failure
```

The local FFmpeg/ffprobe binaries were supplied through their CMake cache
paths; they report `n8.0`, source checkout
`140fd653aed8cad774f991ba083e2d01e86420c7`. This existing decoder build is
distinct from the newer source-only audit above. These tests cover the
existing small/1080p fixtures and accepted narrow geometries, **not extreme
resolution**. Metal execution, Windows SDK runtime, CUDA, Premiere and Resolve
were not tested. No acceleration milestone or performance claim is made.

## C1 probe design and stop conditions (not executed)

1. After P4 review, create a separate test-only Windows SDK harness using
   pristine `CFHD_PrepareToEncode`, `CFHD_EncodeSample`,
   `CFHD_PrepareToDecode`, `CFHD_DecodeSample`, with RAII handles and aligned
   pitched storage. Record source/binary hashes, compiler, macros, SDK errors,
   input format/orientation/range and exact output format. Start with a small
   control, then one 4K frame; 8K → 12K → 17K each require an explicit memory
   estimate, host budget, watchdog and preceding-stage success. No automatic
   escalation to huge allocations on prepare failure or timeout.
2. Use independent Flat, Gradient and Noise fixtures, GOP1 FilmScan3. Generate
   row-wise from explicit integer formulas/LCG seed `0x1337BEEF`, fixed channel
   order and row orientation. Pin the formulas and generator hash before
   comparing outputs. BGRA8-derived p10 and direct full-depth YUV422p10 are
   separate datasets. Stream input generation/hashing to disk; allocate a
   full SDK frame only after preflight, never all patterns/geometries together.
3. On success, retain exact packet bytes locally and record SHA256, byte count,
   every sample/channel/level/band extent, coded/display dimensions and first
   failing tag/offset. Validate all nested extents against actual bytes, even
   if SDK returned success. Reject wrapped size fields. Measure encode/decode
   time and process peak RAM with method; no fabricated VRAM value for CPU.
4. Wrap those **same packet bytes** in a minimal CFHD container using an audited
   test muxer; verify extraction SHA before decode. Feed a pinned FFmpeg `cfhd`
   decoder, one frame to explicit `yuv422p10le` rawvideo. Check return code,
   one decoded frame, actual dimensions, exact `4*width*height` output bytes,
   output SHA and selected first/last pixels, and capture the first diagnostic.
   Record decoder version/build flags and binary hash. `-f null` alone and an
   encoder-listing command are insufficient. Match GoPro output semantics
   before pixel comparisons (see #11); tolerance cannot hide format mistakes.
5. Measure compressed-size distribution separately for all three patterns.
   A flat-frame PASS does not qualify noise or the whole resolution. If a
   >64-MiB packet succeeds, document whether it omits sized wrappers or uses
   another **actually decoded** syntax. Obtain SDK plus independent decoder
   evidence before any writer change; Premiere/Resolve remain separate gates.

Keep capability and resource results independent: `SUPPORTED_BOTH`, `SDK_ONLY`,
`UNSUPPORTED_FORMAT_CHUNK`, `UNSUPPORTED_GEOMETRY`, `RESOURCE_EXCEEDED`,
`SDK_ORACLE_PENDING`, `REFERENCE_PENDING`. Mark native 17K unsupported only
after a qualified format/geometry failure; a host budget rejection is not a
format verdict. Explicitly labeled ROI/downconvert output can be a later
capability, never advertised as native 17520×8040 CFHD. C2 GPU/stress work waits
for P5 and separate resource review. No automatic issue closure or merge.
