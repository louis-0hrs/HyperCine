<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# CPU SIMD: capability admission before codec dispatch

Status: P0 preflight for Issue #65, not a SIMD encoder or a speed claim.

The current wavelet/FilmScan3 CPU reference is scalar C++17. This P0 provides
an independent CPU ISA capability/selection module, with **no production codec
dispatch wiring** and therefore no change to packet/bitstream outputs.

- x86 GCC/Clang uses __builtin_cpu_supports for AVX2, AVX-512F/BW/VL with OS state.
- MSVC uses CPUID and XGETBV; ZMM and XMM/YMM OS state is required for AVX-512.
- AArch64 NEON is in the architecture baseline; no SVE feature is assumed.
- AVX-512 is admitted only with F + BW + VL and OS register state.
- Detection does not imply an implemented, validated vector kernel exists.
- Auto chooses the intersection of host support **and implemented/tested** ISA.
- An explicit unsupported or unimplemented ISA request returns an error.
- P0 intentionally registers no SIMD implementation: actual encoder paths stay scalar.

## Reproduce the independent probe

From repository root (no upstream/GoPro SDK, CUDA, Metal or Vulkan required):

```sh
cmake -S tests/cpu_isa_probe -B build-cpu-isa -DCMAKE_BUILD_TYPE=Release
cmake --build build-cpu-isa --config Release
ctest --test-dir build-cpu-isa -C Release --output-on-failure
```

On Windows use the Visual Studio CMake generator with -A x64.
GitHub CI validates native Windows MSVC, Linux x64 and macOS ARM64. These
tests verify the dispatch contract only, not actual AVX-512/NEON kernels.

## Next stages (separate PRs)

AVX2 and AVX-512BW/F/VL then ARM64 NEON 2-6 forward wavelet kernels.
Compare all LL/LH/HL/HH coefficients with the scalar reference, across
width/tail/stride/border/extreme signed inputs. Verify real original
GoPro packet corpus independently; compare GPU/CPU hardware facts honestly.
Do not globally enable -march=native or /arch:AVX512, or run an ISA that the
host has not advertised. Target-specific translation units or attributes only.
Never reproduce the upstream width-16 out-of-bounds hypothesis (#21) as a
SIMD compatibility workaround. No external dependencies or upstream edits.
