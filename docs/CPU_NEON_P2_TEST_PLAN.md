<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# ARM64 NEON 2-6 wavelet qualification plan

Preparation for [#70](https://github.com/louis-0hrs/HyperCine/issues/70),
parent [#65](https://github.com/louis-0hrs/HyperCine/issues/65).
Source audit: protected main `5c87391e82f307fbe7db0be6a53fd0edbdaad527`;
prospective P4 integration `373b0ecf5b4d64df81bd4c3ceebf9ef391b250a6`
([#73](https://github.com/louis-0hrs/HyperCine/pull/73)).

**Preparation only. Kernel implementation remains BLOCKED_BY_P4 acceptance.**
Passing hosted checks for #50/#73 do not accept the geometry API or authorize
starting the kernel. Recheck [#44](https://github.com/louis-0hrs/HyperCine/issues/44)
and [dispatch #62](https://github.com/louis-0hrs/HyperCine/issues/62) before
implementation. This document changes no codec, test, dispatch or GPU source.

## Existing seams and invariants

| Source | What the implementation must preserve |
| --- | --- |
| `src/cpu/cpu_isa.h`, `cpu_isa.cpp` | Canonical internal `CpuIsa` API. Hardware NEON and a validated implementation are separate requirements. Registry is currently empty; native Auto is scalar and forced NEON throws. |
| `src/cpu/wavelet_cpu.cpp` | Horizontal/vertical scalar forward filters; signed floor division, int32 intermediates and saturation at each stored int16 stage. |
| `src/cpu/standalone_frontend.cpp` | `ForwardLevel`: horizontal first, vertical second; special prescale-2 lowpass rounding. Plane stride is in int16 elements. |
| `src/core/filmscan3_backend.{h,cpp}` | Internal `Frontend::Forward` and per-instance `FrontendAccess` seam; three levels use prescales **0, 2, 0** for each Y/Cr/Cb channel. |
| `include/hypercine/standalone.h` | Aligned plane allocation, stride padded to eight int16 elements, band order **LL, LH, HL, HH**. |
| `tests/test_standalone.cpp` | Existing affine, impulse, prescale and geometry regression checks; these are seeds for coverage, not exhaustive SIMD qualification. |
| `tests/test_cpu_isa.cpp`, `ci/run_standalone.py` | Existing selection matrix and required CTest inventory. Neither currently invokes a NEON kernel. |

The standalone target currently does not link CPU ISA selection into its wavelet
calls. Merely changing `ValidatedCpuIsaBackends()` cannot integrate a kernel.
After P4 acceptance, review the internal call boundary explicitly; retain the
scalar functions as directly callable reference implementations. Start with an
isolated ARM64 translation unit under `src/cpu/` and direct internal test calls.
Keep `<arm_neon.h>` and architecture-specific compilation out of x86 builds.
No public backend enum or second ISA selector is needed for this plan.

The P4 candidate introduces `PlaneStorageFits`/`kMaxPlaneElements` and session
geometry, while legacy `ValidateGeometry` retains its narrower budget. Tests
must use the accepted FilmScan3 geometry path for 4K; do not globally relax the
legacy encoder cap. Re-audit this boundary at the accepted P4 SHA.

## Arithmetic oracle

For each six-tap neighborhood, compute expected values independently in int64
test code, with explicit mathematical floor and clamp to [-32768,32767].
Never derive expected coefficients by calling the candidate NEON routine.
Compare that oracle with both the scalar and candidate output, coefficient by
coefficient, including all intermediate horizontal and final four bands.

For a pair at offset `x`, scalar lowpass is `sat(s[x]+s[x+1])`. Highpass is:

| Position | Value before final saturation |
| --- | --- |
| First pair | `floor((5*s[0]-11*s[1]+4*s[2]+4*s[3]-s[4]-s[5]+4)/8)` |
| Interior | `floor((-s[x-2]-s[x-1]+s[x+2]+s[x+3]+4)/8)+s[x]-s[x+1]` |
| Last pair | `floor((11*s[x]-5*s[x+1]-4*s[x-1]-4*s[x-2]+s[x-3]+s[x-4]+4)/8)` |

The vertical formulas are identical along the row axis. Widen before additions
and multiplications; do not use wrapping or saturating int16 intermediates.
The largest absolute weighted border numerator is bounded by `26*32768+4`,
which fits int32. Preserve rounding for negative values around every multiple
of eight. Inverse transforms are outside this forward-kernel task.

Prescale 2 has two different rounding points: highpass taps use
`floor((s+3)/4)` individually, but horizontal lowpass uses
`floor((s_even+s_odd+3)/4)` on the original pair. Do not sum individually
rounded taps or saturate the original pair sum before division. The vertical
pass then uses ordinary stored int16 horizontal results. Test prescale 0 and 2
independently and in the full 0/2/0 pyramid.
For example, pair `[1,1]` must produce prescaled horizontal lowpass 1,
where summing separately rounded taps gives 2. Pair `[-32768,-32768]`
must produce -16384; saturating its sum before prescaling gives -8192.

Retain the hand-calculated impulse `[0,0,0,100,0,0,0,0]` with lowpass
`[0,100,0,0]` and highpass `[50,-100,-12,13]`. Add mirrored and transposed
cases to distinguish both borders and horizontal/vertical band routing.

## Deterministic test matrix after the start gate

| Layer | Required cases | Pass criterion |
| --- | --- | --- |
| Individual filters | Horizontal even widths 6 through 66, plus 126/128/130 and 1918/1920/1922; vertical even heights 6 through 66, with widths 1 through 33 and 127/128/129 | Every active low/high coefficient equals independent oracle and scalar; covers partial vectors, both borders and no-vector cases. |
| Value coverage | Flat -32768/-1/0/1/32767; alternating extrema; affine ramps; signed impulses at each border tap and vector/tail transition; deterministic noise | Zero coefficient differences, including saturated outputs and negative rounding ties. |
| Systematic tap sweep | Sweep all 65,536 int16 values through each of the six tap positions, with other taps fixed in separate zero/min/max/alternating contexts; evaluate first/interior/last formulas | Zero mismatches. This sweeps each tap exhaustively, not all possible six-tap combinations. |
| Two-dimensional planes | Even width/height pairs from 6/8/10/14/16/18/30/32/34/62/64/66, plus representative asymmetric planes; prescale 0 and 2 | All four bands, dimensions and horizontal scratch coefficients equal scalar. |
| Actual pyramid | Y and half-width Cr/Cb, three levels, 128x24, 128x128, 1920x1080; 3840x2160 only on accepted P4 | Every active coefficient at every level matches; preserve band order and 0/2/0 prescales. |
| Layout | 32-byte aligned base; minimal legal padded pitch and an extra 8 or 24 elements per row; independent source/low/high pitches | Identical active output for every layout; no source or output-padding writes. |
| Invalid storage | Null pointer at each raw-filter argument, invalid axis length, short/negative stride; Plane odd stride, truncated storage, invalid prescale and checked-size overflow | Existing status/exception contract, no output/state mutation at the validated frontend boundary; never call the raw scalar oracle with overflowing indexing or undersized memory. |

Use a specified uint32 xorshift sequence, seed `0x1337BEEF`, with shifts 13, 17,
5 and explicit uint32 wrap. Map low 16 bits to signed int16 via int32 arithmetic
(`u <= 32767 ? u : u-65536`); avoid implementation-defined unsigned-to-signed
conversion or library-dependent random distributions. Record pattern, seed,
dimensions, pitch, prescale, band and first differing coordinate on failure.

Maintain the required 16-byte row alignment. Odd logical widths belong in the
raw **vertical** filter tests, whose width need not be even; odd pitch is a
negative Plane-validation case. The raw horizontal/vertical C APIs do not
currently enforce the Plane alignment contract, so do not infer new rejection
behavior or broaden accepted production layouts from this test plan.

## Memory safety evidence

Allocate fixtures and scratch with the existing aligned allocator and RAII.
Check source immutability plus prefix/suffix and per-row output canaries.
Repeat with changed input padding to catch dependence on out-of-image samples.
Canaries detect writes; they do not establish absence of reads.

Use native ASan/UBSan with the smallest legal allocations, including final-row
tails. Where the harness can poison logical row padding safely, poison it;
otherwise disclose that limitation and review load bounds explicitly. Process
only complete interior vectors whose full six-tap loads lie within the active
row, then handle borders and tails without speculative out-of-row reads. No
allocator slack, next row or guard pixel may act as a valid filter tap.

The scalar low-level C functions have no byte-size arguments and use int row
index arithmetic. Geometry overflow/truncated-allocation cases therefore belong
at the bounded Plane/frontend boundary, before invoking either kernel.

## Packet, state and dispatch gates

1. Use separate scalar and candidate frontend instances over identical aligned
   BGRA input. Compare color planes, all intermediate bands, the 30 stored band
   coefficient/payload hashes, 36 quant/scale entries and complete sample bytes.
2. Run the existing 16-frame Tier A and 120-frame sequence manifest gates;
   separately run actual original-SDK packets when privately configured corpora
   are available. Freeze input hashes and metadata normalization. Manifest-only
   validation must not be reported as actual raw-packet replay.
3. On accepted P4, retain all 19 independent 4K FFmpeg p10 decodes and ordered
   identity of the sixteen changing sequence frames against independent-packet
   decodes. Preserve PSNR gates. Scalar-versus-NEON identity at 4K does not clear
   the missing original-GoPro 4K packet oracle (`REFERENCE_BLOCKED`).
4. Interleave separate 1080p and 4K sessions, Reset, padded input and repeated
   changing frames. After rejected input or deterministic injected failure,
   compare frame index, limiter/quantizer and next successful packet against an
   untouched scalar control. Do not provoke actual uncontrolled OOM.
5. Keep production registry NEON false while direct kernel tests run. Add
   actual dispatch-call evidence before enabling it; a synthetic capability
   matrix cannot prove the kernel ran. Test forced unavailable NEON rejection
   and Auto fallback independently of hardware detection. Revise P0's
   scalar-only native assertions only in the reviewed integration change.

The 128px upstream SIMD oracle is `UPSTREAM_ORACLE_UNDEFINED` under #21/#71.
Use the bounded mathematical/scalar oracle there and retain the negative SDK
regression; do not rerun an unsafe original SDK call or label that width as SDK
packet parity. 8K/12K/17K and Metal multiresolution remain separate gates.

## Native validation and performance sequence

Before starting implementation, recheck dependencies and choose the accepted
P4 base. Run the complete registered standalone CTest suite in native ARM64
Release and ASan/UBSan with required external decode. Include new test names
in the required CI inventory and its inventory regression tests. Run Windows
x64/Linux x64 hosted suites to verify compilation and scalar fallback; these
cannot qualify a native NEON kernel. Record exact SHA, compiler, architecture,
test count, skips and external corpus availability.

Only after zero coefficient/packet differences and safety PASS, measure scalar
and NEON on identical inputs, with at least five trials, separate warmup,
kernel-only and complete packet/session timings, median/p95, avg/min/max,
FPS, megapixels/s and peak memory. Measure and disclose allocation/copy costs.
Run `benchmark_codec` and compare the recorded Windows MSVC reference with its
hardware/platform limitations explicitly stated; do not present a Mac-versus-
Windows ratio as the NEON gain. The paired same-host scalar run isolates that
gain. A missing runnable canonical benchmark blocks performance qualification,
not an invitation to invent a throughput number.

Publish only portable commands, hashes and sanitized results. Use
`HYPERCINE_GOLDEN_DIR` for private corpus discovery; do not publish its value.
This plan introduces no new CMake flag, CLI option or claim that NEON works.

## Preparation baseline actually run (2026-10-16)

On the audited main source above, native macOS ARM64 / AppleClang 21.0.0:

| Gate | Observed result |
| --- | --- |
| Standalone CPU Release full CTest, required FFmpeg/ffprobe, Metal/CUDA/upstream disabled | **31/31 PASS**, 96.47 s; no skipped tests. |
| Actual original-SDK Tier A and changing sequence corpora | Both registered raw-packet tests PASS; private corpus locations omitted. |
| ISA native probe | NEON capability true; production Auto remains Scalar; forced NEON rejected by the existing test. |
| ISA synthetic matrices | 4,096 x86 probes and 1,024 selection configurations PASS. |
| CI inventory / privacy scanner unit tests | **6/6** and **13/13 PASS**. |

These results validate the current scalar baseline only. No NEON coefficient,
NEON packet, SIMD performance, 4K-on-this-branch or new sanitizer PASS is claimed.
The P4 #73 native/hosted results remain separately owned evidence, not reruns
performed for this preparation.

Portable reproduction from the repository root, with FFmpeg/ffprobe on PATH:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_ENABLE_METAL=OFF \
  -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=OFF \
  -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
python3 ci/test_standalone_gate.py
python3 ci/test_privacy_guard.py
```

The reported run additionally set existing CMake cache variables
`HYPERCINE_REFERENCE_CORPUS_ROOT` and `HYPERCINE_FS3_SEQUENCE_ROOT` to locally
verified corpora discovered privately. Configure these to reproduce the two
actual packet tests; without them, CMake does not register those tests and the
test count is lower. Raw configure/build/test logs remain outside Git.

Next action: review this preparation independently; after explicit P4 API
acceptance, rebase the implementation plan onto that accepted source and begin
the isolated kernel plus direct coefficient tests. This document is not that
acceptance decision.
