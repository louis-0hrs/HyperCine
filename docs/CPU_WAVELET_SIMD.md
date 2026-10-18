# Experimental x86 forward wavelet kernels (issue #69)

Copyright (c) 2026 HyperCine Contributors. Dual-licensed under MIT OR Apache-2.0.

This change adds explicit AVX2 and AVX-512 implementations of the existing
signed 16-bit horizontal and vertical forward 2-6 filters. It leaves the
public codec API, scalar implementation, production frontend selection and
`ValidatedCpuIsaBackends()` unchanged. `Auto` continues to select scalar.
The kernels are an isolated review deliverable, not production acceleration
registration. No upstream or GPU source is modified.

`src/cpu/wavelet_simd.cpp` is compiled for the baseline ISA. It validates
dimensions, strides and addressable extents before calling an ISA-specific
translation unit. Explicit unsupported, unbuilt or unknown ISA requests
return `HYPERCINE_STATUS_NOT_SUPPORTED` without running a target instruction
or changing output. The canonical `DetectCpuIsa` / `CpuIsaUsable` capability
boundary checks CPU and OS register-state support. The experimental seam's
compiled-kernel availability is separate from production validation status.

Each target file receives its own compiler flags; no global ISA flags are
added. AVX2 computes eight coefficients at once and AVX-512 computes sixteen.
Adjacent horizontal pairs use signed multiply-add into int32 lanes. Vertical
loads sign-extend int16 into int32. Arithmetic shifts implement exact signed
floor division, and only final stores saturate to int16. Lane narrowing
preserves sample order. Horizontal borders use the defined scalar one-sided
six-tap formulas; vertical borders are vectorized with those same formulas.
Incomplete vectors use bounded scalar tails. Logical row padding is never
needed as input. No small-width SDK undefined reads are reproduced.

The internal validation seam intentionally accepts unaligned pointers and
arbitrary valid element strides; its loads/stores are unaligned operations.
Production codec allocation/alignment contracts are unchanged. Input/output
planes must have separate, non-overlapping storage, as with the scalar filter.

## Validation

Native host: Windows x64, AMD Ryzen 9 7940HS, 8 physical cores / 16 logical
threads; MSVC 19.44.35229, Release. Both AVX2 and AVX-512 executed natively.

- `wavelet_simd_exact`: 2,174 cases per scalar/AVX2/AVX-512, 6,522 total.
  Compares every coefficient and output padding/canary, preserves source,
  poisons input padding, repeats calls and places exact input/output extents
  against inaccessible pages. Covers minimum geometry, every even width
  6..130, vertical widths with short tails, 128/256/1080p/4K planes, signed
  extrema, flat, impulse, alternating extrema, edges, small signed ramps and
  seeded full-range noise (`0x1337BEEF`).
- `wavelet_simd_packets`: the existing internal frontend injection seam
  runs 16 ordered full 1080p FS3 frames through separate scalar, AVX2 and
  AVX-512 sessions. Black/gradient/noise inputs compare complete packet bytes,
  all coefficient hashes, quant tables and persistent rate-control state.
  The first packet must also equal the preexisting frozen `flat.cfhd`.
- Native Release standalone suite with required FFmpeg decode: 30/30 PASS.
  Existing fixed packet hashes, SDK coefficient corpus and sequence manifests
  remain unchanged. Native CPU SDK suite: 11/11 PASS, including original SDK
  decode of the existing independent encoder fixtures.
- Native MSVC AddressSanitizer: both new CTests PASS, with AVX2 and AVX-512.
  Local MSVC does not supply UBSan; Linux x64 ASan/UBSan is added to the hosted
  standalone matrix. Hosted results must be reviewed on the actual PR head.
- `HYPERCINE_BUILD_X86_WAVELET_SIMD=OFF`: scalar exactness and explicit SIMD
  rejection PASS. ARM builds compile the baseline seam only; existing ARM CI
  must confirm scalar behavior. This does not implement NEON.

### Current-main qualification (2026-10-18)

PR #77 was merged forward onto protected main `e094682` and re-tested on a
second native Windows x64 host: AMD EPYC 7601, 8 physical cores / 8 logical
threads, MSVC 19.44.35229, Release. This host has AVX2 but no AVX-512, so it
exercises the AVX2-only path used by older x86 machines.

- `wavelet_simd_exact`: `PASS ISA=1 cases=2174`, `PASS ISA=2 cases=2174`,
  `HW_PENDING ISA=3`; 4,348 deterministic cases executed. AVX-512 was not
  claimed or emulated on this host.
- `wavelet_simd_packets`: 16 ordered full FS3 frames PASS with
  `AVX2=1 AVX512=0`; the frozen first packet is unchanged.
- Full standalone CTest with required FFmpeg decode: 34/34 PASS (the suite
  grew on current main). Native CPU SDK suite: 15/15 PASS.
- Native MSVC AddressSanitizer: both new CTests PASS on the AVX2-only host.
- `HYPERCINE_BUILD_X86_WAVELET_SIMD=OFF`: scalar exactness PASS; both SIMD
  requests are rejected as unavailable, as expected.

Hosted CI for the merged head is reviewed separately from these local runs.

4K here means raw wavelet plane exactness, not new 4K production FS3 support
or GoPro packet parity. The existing geometry and SDK-oracle restrictions
remain in force. Native CUDA/Metal/Vulkan qualification is outside this change.

## Reproduce

```powershell
cmake -S . -B build -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
build/Release/benchmark_wavelet_simd.exe 1
build/Release/benchmark_wavelet_simd.exe 8
```

FFmpeg and ffprobe must be on PATH, or supplied via
`HYPERCINE_FFMPEG_EXECUTABLE` and `HYPERCINE_FFPROBE_EXECUTABLE` CMake cache
entries. On single-configuration generators the executables are directly
under the build directory. For native MSVC ASan use `/fsanitize=address /Zi
/EHsc /D_DISABLE_STRING_ANNOTATION /D_DISABLE_VECTOR_ANNOTATION` in
`CMAKE_CXX_FLAGS`, `/INCREMENTAL:NO /DEBUG` in `CMAKE_EXE_LINKER_FLAGS`, and
make the installed MSVC ASan runtime available on PATH. Run the two
`wavelet_simd` tests in that separately instrumented build.

## Measurements and limits

`benchmarks/results/cpu_wavelet_simd_windows.json` records individual raw-plane
stage measurements at 1080p and 4K (31 iterations after three warmups), plus
complete stateful 1080p FS3 sessions (16 frames after three warmups) on the
original AVX2+AVX-512 host. `benchmarks/results/cpu_wavelet_simd_windows_avx2_only.json`
records the AVX2-only AMD EPYC 7601 host at the merged head, with the same
1-worker and 8-worker stages. Each entry includes median/p95,
average/min/max latency, FPS, megapixels/second and cumulative process peak
working set. Memory is the process high-water mark, not an isolated per-kernel
allocation. The ISA IDs are 1=scalar, 2=AVX2, 3=AVX-512.

Stage tests use identical seeded int16 input and separate 1-worker / 8-worker
runs. Horizontal work partitions rows; vertical work partitions columns to
preserve all filter boundaries. The 8-worker timing includes thread creation
and join for every call, without affinity pinning. Complete codec sessions
are always single-worker and use a test-only injected frontend, identical
input bytes and initial state for each ISA. They include conversion, wavelet,
quantization, coefficient hashing, entropy, framing and returned packet
destruction. They do not represent an enabled production backend. CPU clocks
and power are OS-managed, so these short local samples are indicative only.

AVX-512 is not consistently faster than AVX2. At 1080p, thread setup can cost
more than vectorized computation. No automatic ISA ranking, thread-pool
policy, general encode-speed promise or performance acceptance threshold is
derived from these measurements. The existing `benchmark_codec` is also run
against `reference_windows_msvc.json`; its GoPro-assisted codec baseline is
kept separate from these SDK-free sessions.

The final separate reference-codec run measured encode FPS 80.17 / 22.79 /
5.44 and decode FPS 428.12 / 125.06 / 27.77 for 1080p / 4K / 8K. The recorded
baseline is 77.70 / 21.84 / 4.28 encode and 394.15 / 125.31 / 27.96 decode.
Memory remains approximately 92 / 229 / 613 MiB. The 1080p encode maximum
latency had a 17.73 ms outlier versus the old 14.70 ms maximum, while average
latency fell from 12.56 to 12.17 ms. This unchanged reference executable has
no SIMD dispatch; OS-managed clocks/scheduling and independent short runs
limit causal interpretation. These results do not establish a production
codec improvement or a reproducible latency regression.

Production dispatch remains gated on review of actual Windows/Linux hosted
checks, capability/ISA isolation and the measurements. No merge is performed.
