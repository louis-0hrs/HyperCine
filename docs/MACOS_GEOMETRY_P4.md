# FilmScan3 CPU geometry P4 — design and qualification

Source baseline: `2a5e94ff6120a7053621c6ab4dcb020c7934e6d4`; issue #44.

## Audit and design (before implementation)

The legacy `ValidateGeometry` and fixed-Q2 `Sample` budget stay at 2,097,152
pixels / 16,793,600 bytes. Raising that global constant would also silently
expand legacy allocations. Instead, CPU FilmScan3 gets an explicit per-session
geometry constructor and its own 8,294,400-pixel budget. Existing constructors
retain 1920×1080 semantics and encoder object layout. Session geometry is fixed;
Reset resets rate state only. The new constructor selects CPU only.

| Boundary | Existing behavior | Planned bound / proof |
| --- | --- | --- |
| Frame dimensions | width / height <=4096; width %128, height %8, height >=24 | Same legal wire geometry; independent FS3 pixel budget; 8K rejected |
| Plane constructor | padded stride times height <= legacy pixel cap | Separate 8,294,400-element allocation cap; <=16,588,800 bytes per plane |
| Forward / frontend validation | shared legacy plane cap | Same plane cap; prescale 0,2,0 and six-tap minimum unchanged |
| BGRA storage | aligned, positive padded pitch; checked last row extent | Preserve checked `(SIZE_MAX-row)/(height-1)` guard; bottom-up CG709 Y/Cr/Cb |
| PackHighpass | 4*(N+1) reserve; <=26 bits/coefficient and end/pad | Separate plane cap; <=33,177,604 bytes reserve; unsigned run <=8,294,400 |
| FS3 Writer::Check | 8*legacy pixels+16384 | Session `8*pixels+16384` <=66,371,584 bytes; below 24-bit chunk limit |
| Writer::End | 24-bit word count | Subtraction-based patch bounds; reject >0xffffff words |
| Writer::Record | 24-bit size, fixed small metadata | Explicit size guard; checked bounded padding |
| Writer::Patch / channel index | uint32 bytes | Check before narrowing; whole sample <=66,371,584 < UINT32_MAX |
| NextLimiter | fixed 1920*1080*3*10/8 | Session pixels, integer division then float conversion/division, double 1.5 then float |

All products are bounded before use: dimensions <=4096, padded plane stride
<=4096, sample/entropy sizes <2^27, indices <8,294,400. These fit even 32-bit
size_t and signed int indexing. No global 8K allowance or new chunk variant.
`upstream/Codec/quantize.c` computes the integer 3-channel 10-bit numerator
before float division and then compound division by double 1.5. Preserve that
rounding order; 1080p original packets are the regression oracle, not runtime
rate-control input. GUID/DATE/TIME/TIMC/UFRM and all coefficient/table records
remain constructed exactly as before. State commits only after packet assembly.

Conservative live CPU allocation estimate: <=8P bytes color+pyramid/output
bands, <=4P temporary transform buffers, <=4P entropy, <=16P writer capacity
plus <=4P caller BGRA and allocator overhead: <36P+1MiB, about 286 MiB at 4K.
The budget concerns one encode, not arbitrary caller-retained output history or
unbounded concurrent encoders. Measure process peak RSS separately on the
16GB M2 Pro. Allocation failures propagate without committing frame state.

## Qualification status

Native validation is complete; see results below. 4K SDK
byte matching requires a genuine original-GoPro corpus with identical inputs,
parameters, state and metadata. 8K remains PENDING_8K until independent SDK
samples, chunk feasibility and host resource qualification exist.

## Implemented API and resource contract

```cpp
using namespace hypercine::standalone;
FilmScan3Encoder hd; // existing ABI/layout, always 1920x1080
FilmScan3Encoder uhd(FilmScan3Geometry{3840,2160}); // CPU only
// Optional output limit can only REDUCE 8*width*height+16384.
FilmScan3Encoder bounded(FilmScan3Geometry{3840,2160,1024*1024});
auto packet = uhd.Encode(aligned_bgra, available_bytes, padded_positive_stride);
uhd.Reset(); // same geometry and output budget; frame/rate state starts over
```

The geometry descriptor is copied into an immutable internal CPU frontend;
no data members were added to the public encoder. A consumer compiled against
the exact baseline public headers and linked with the new library passes the
existing API test (`old-api-consumer.log`). No geometry mutation API exists;
construct another encoder to change resolution. The returned descriptor is a
copy. Existing backend constructors retain the implicit 1080p geometry;
there is no new public Metal geometry constructor.

`sample_capacity_bytes=0` selects the derived bound. A nonzero value above the
bound throws at construction; a smaller bound can cause `length_error` during
packet assembly. This is a real Writer capacity error, tested after one
successful flat frame with subsequent noise, followed by a correct retry.
The test also simulates `bad_alloc` after the full CPU transform and causes a
late framing rejection. Those injections verify rollback and RAII; they do
not claim that the machine was driven into physical OOM. Invalid 8K geometry
fails at construction before allocating input/plane/entropy buffers.

The only edit in `standalone_encoder.cpp` changes the shared `PackHighpass`
plane-storage check to the separate plane cap; legacy Sample capacity and
`ValidateGeometry` stay unchanged. The shared plane validators check positive
stride/height and division-based capacity before multiplying, including
mutated public Plane descriptors. All new products are evaluated after their
validated constant bounds; no unchecked size_t/int narrowing is introduced.

## Reproducible gates and evidence

Configure an SDK-free Release build with `HYPERCINE_BUILD_STANDALONE=ON`,
`HYPERCINE_BUILD_UPSTREAM=OFF`, `HYPERCINE_REQUIRE_EXTERNAL_DECODE=ON`, and
explicit FFmpeg/ffprobe paths if needed. Optional actual corpus roots are
`HYPERCINE_REFERENCE_CORPUS_ROOT` and `HYPERCINE_FS3_SEQUENCE_ROOT`.
Run `python3 ci/run_standalone.py --build-dir <build> --system Darwin
--machine arm64` for the complete registered suite. The CI inventory now
requires `filmscan3_geometry` and `filmscan3_geometry_external_decode`.
The existing four-host standalone workflow includes these automatically.
The actual 120-frame CTest now injects original metadata as the 16-frame test
already did, so both require complete raw packet identity.

The C++ geometry test exports genuine Flat, GradientChecker, Noise and a
16-frame Noise/HorizontalGradient/Checkerboard sequence generated from seed
`0x1337BEEF`, with unique frame identity stamps in the sequence. It checks all
30 subband extents, three dimensions, nine levels, channel byte indices and
24-bit chunk bounds; it also exercises invalid/misaligned/truncated input,
pitch overflow, padded pitch, Reset, distinct session state, output capacity
failure and retry. The export records quant[36], scale[36], coefficient FNV[30],
frame index, limiter and previous byte count.

The Python test hashes actual BGRA bytes and raw packets, parses and hashes
all 30 payload/framed bands, checks UFRM, then independently decodes all 19
packets with FFmpeg to actual `yuv422p10le`. It checks the native pixel format
and dimensions with ffprobe before conversion, checks exact output extent and
10-bit range, and requires aggregate p10 PSNR >35 dB against CPU conversion
planes. It additionally wraps and decodes all 16 GOP1 packets together,
verifying exactly 16 frames and the full decoded byte extent. Every sequence
frame must also match the SHA-256 of its independently decoded packet, in
order. The report records these 16 hashes and `sequence_pixel_identity`;
older reports without this field do not establish this additional gate.
The mandatory `filmscan3_geometry_validation` test rejects reordered,
duplicated, corrupted, truncated and extra frames using synthetic byte data.
Only FFmpeg's
CFHD decoder and rawvideo support are needed; no PSNR filter, null muxer,
pipe protocol or external Python package is required. Temporary inputs,
references and decoded images are removed after their hashes are recorded.

The follow-up review results and tested source hashes are recorded in
[`sequence-review.json`](validation/macos-geometry-p4/sequence-review.json).
The review also tests the original P4 tree combined with the then-current
main; this is CPU integration evidence, not P4 merge approval or Metal 4K
qualification.

Reports, native logs, JUnit and exact tested source hashes are under
`docs/validation/macos-geometry-p4/`. Process RSS is measured after the export
child exits (including its correctness/failure tests and retained samples),
separately for Release and sanitizer binaries. This is not a single isolated
buffer-allocation count. Sanitizer RSS includes quarantine/instrumentation.
No performance optimization or acceleration milestone is claimed.

## Separate reference and 8K gates

The read-only NAS FS3 directory at
`/Volumes/DeDub/HyperCine/golden/windows/fs3/2026-10-14` contains Tier A and
sequence-1080 archives. Neither supplies matching 3840×2160 or 7680×4320
original-GoPro samples. Accordingly 4K SDK raw-byte qualification is
**REFERENCE_BLOCKED**; 4K CPU/FFmpeg remains experimental. Premiere, Resolve
and original SDK 4K decoding were not tested here.

8K is **PENDING_8K**, not an accepted encode geometry. The current conservative
8*P+16384 sample bound would become 265,437,184 bytes at 8K, exceeding the
existing 24-bit word-count payload limit of 67,108,860 bytes. That does not
prove every real 8K sample is impossible; actual compression and the original
SDK wire behavior must first be established. A conservative 36P+1MiB live
allocation estimate is about 1.11 GiB before caller-retained history and
concurrency; no 8K allocator/RSS qualification has been performed. Existing
4096 dimension and 8,294,400-element caps deliberately reject it. A follow-up
must obtain SDK packets and identical inputs, independently decode them, audit
actual chunk sizes, then qualify bounded host allocations before enabling 8K.
Metal P5 remains a separate follow-up after P4 review; no GPU source, CUDA,
Windows decoder, upstream file or quality-state #29 source was edited.

## Native measured results (2026-10-16)

Apple M2 Pro, Mac14,9, arm64, 17,179,869,184 bytes RAM; AppleClang 21.

| Gate | Result | Full-suite elapsed |
| --- | --- | --- |
| CPU Release complete CI inventory + original corpora | 32/32 PASS | 217.77 s |
| CPU ASan/UBSan + original corpora | 32/32 PASS | 406.03 s |
| Metal Release + original corpora | 38/38 PASS | 289.20 s |

Suites overlapped on the host; elapsed times are correctness-run durations,
not throughput measurements. ASan runs with `detect_leaks=0` (Apple native
leak sanitizer unsupported) and UBSan with `halt_on_error=1`.

CPU Release export/test peak RSS is **242,860,032 bytes (231.61 MiB)**;
ASan/UBSan **439,615,488 bytes (419.25 MiB)**. The Metal-enabled build's new
geometry constructor still selects CPU, and reaches 247,300,096 bytes
(235.84 MiB). All 19 input, packet, coefficient, table, state, payload and
decoded hashes match across all three builds.

| Independent 4K initial frame | Packet bytes | p10 PSNR |
| --- | --- | --- |
| Flat128 | 604,172 | exact reconstructed p10 planes |
| GradientChecker | 4,240,660 | 59.84 dB |
| DeterministicNoise | 19,829,252 | 55.09 dB |

All 19 independently decoded packets exceed 35 dB (minimum **52.32135 dB**,
excluding the exact flat frame). The separate 16-frame container decodes to
exactly 16 frames / 530,841,600 raw bytes. Its autonomous limiters are
`4,9,5,10,15,11,14,19,15,18,20,16,19,20,16,19`; inputs and coefficients change
on every frame. Full raw SDK packets, 36 quant/scale entries and 30 coefficient
and payload identities remain unchanged for all 16+120 1080p frames.

Hosted CI status is recorded separately in `validation.json`; native results
do not imply a hosted pass before those jobs complete.

## Extended 12K/17K design and fail-closed audit (issue #48)

Issue #44 was expanded during implementation to cover future URSA Cine 12K
12288×8040 and 17K 17520×8040 as **audit targets only**. The explicit 4K CPU
implementation and its DoD stay unchanged. `FilmScan3Geometry` uses signed
int dimensions (all 16-bit CFHD wire values are representable) and size_t byte
budgets; it introduces no future 8K ceiling. The unchanged historical 4096
cap and the separate *currently qualified* 4K allocation cap are policy gates,
not the representation limit of the descriptor. Raising either requires a new
qualification; no caller can bypass them by supplying a larger byte budget.

| Research geometry | width %128 | LL3 chroma width (%8) | Conservative sample bound | Current result |
| --- | ---: | ---: | ---: | --- |
| 7680×4320 | 0 | 480 (0) | 265,437,184 | PENDING_8K; early rejection |
| 12288×8040 | 0 | 768 (0) | 790,380,544 | PENDING_12K; early rejection |
| 17520×8040 | 112 | 1095 (7) | 1,126,902,784 | PENDING_17K; early rejection |

17520 is divisible by 16, but the existing entropy coder requires each deepest
chroma row width to be divisible by 8. Its logical LL3/chroma width 1095 fails
that contract; blindly relaxing %128 would produce unsupported row semantics.
No band padding, display-width substitution or private entropy variant is added.
8040 is divisible by 8, yielding a final lowpass height of 1005; that fact alone
does not qualify either frame format. Width/height tags can represent these
numbers, while the enclosing 24-bit word-sized sample remains a separate limit.

The prospective 10-bit rate numerator P*3*10 exceeds signed int32 at 12K and
17K (2,963,865,600 and 4,225,824,000 respectively). The current path multiplies
size_t only after the qualified cap check, so these rejected geometries never
reach arithmetic, transforms, allocations, or state commit. Future work must
use checked multiplication/addition for BGRA, pitch extent, padded planes,
entropy, writer storage and rate metrics **before allocation**, independent of
resolution names; apply the existing wire limit before reserving memory. Do
not set the new API's ultimate limit to 8K or infer a larger legal sample from
host RAM. The conservative 36P+1MiB host estimates for 12K/17K are about 3.31
and 4.72 GiB per session before history/concurrency/instrumentation. They are
research estimates, not measured encode support on the 16GB host.

`large-geometry-audit.json` records real constructor acceptance/rejection,
unchanged independent session state, checked uint64 arithmetic and all bounds.
Reproduce without allocating large frames:

```sh
c++ -std=c++17 -O2 -Iinclude \
  docs/validation/macos-geometry-p4/audit_large_geometry.cpp \
  <build>/libhypercine_standalone.a -o /tmp/audit_large_geometry
/tmp/audit_large_geometry
```

Actual GoPro/FFmpeg compatibility, padding conventions, chunk acceptance and
measured allocator/RSS gates for 8K/12K/17K belong to separate issue #48. All
three remain rejected here; no high-resolution product or throughput claim.
