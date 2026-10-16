<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# CPU ISA capability and selection boundary (P0)

Issue: [#65](https://github.com/louis-0hrs/HyperCine/issues/65).
This phase adds internal introspection and policy only. No codec call site,
wavelet, color conversion or quantization uses SIMD dispatch yet; no performance
or bitstream qualification is claimed.

`src/cpu/cpu_isa.h` is private to the implementation. `DetectCpuIsa` reports x86
hardware flags separately from OS-enabled SSE/YMM and opmask/ZMM register state.
The x86 probes use bounded CPUID leaves and execute XGETBV only after checking
XSAVE, OSXSAVE and AVX. Both MSVC intrinsics and GCC/Clang CPUID plus guarded
XGETBV require no global AVX compilation flag. Microsoft documents XGETBV as
an [XSAVE intrinsic](https://learn.microsoft.com/en-us/cpp/intrinsics/x64-amd64-intrinsics-list).
Native Apple/Windows ARM64 reports baseline NEON; Linux AArch64 checks
`AT_HWCAP/HWCAP_ASIMD`. Other architectures conservatively report no SIMD.

Selection intersects runtime capabilities with the implemented-and-validated
backend registry. In P0 the registry contains scalar only, so native `Auto`
always returns `Scalar`. An explicit SIMD request throws even on capable
hardware until its backend is validated. Unknown enum values throw separately.
There are no environment overrides. The synthetic registry overload is an
internal test/future integration seam, not an application override.

| Backend | Required capabilities | Required implementation |
| --- | --- | --- |
| Scalar | Always available | Built in |
| AVX2 | CPUID AVX2; XSAVE/OSXSAVE/AVX; XCR0 SSE + YMM | Validated AVX2 |
| AVX-512 | CPUID F + BW + VL; AVX prerequisites; XCR0 SSE + YMM + opmask + both ZMM components | Validated AVX-512 |
| NEON | Native AArch64 ASIMD | Validated NEON |

`Auto` tries AVX-512, AVX2, NEON, then scalar. Explicit requests never silently
fall back. Capability detection alone cannot enable a backend. P1/P2 must add
their own bit-exact stage and full-packet gates before changing the registry.

## Verification

The `cpu_isa_selection` CTest checks all 4,096 combinations of x86 prerequisite,
hardware and required register-state bits, 1,024 capability/validated-backend
configurations, explicit rejection, unknown requests and the native probe.
Synthetic states intentionally include impossible combinations to verify that
missing prerequisites cannot be bypassed. Checks remain active in Release.
The target is registered in both standalone and SDK builds. Standalone CI's
required inventory includes it, covering Windows MSVC x64, Linux x64 and macOS
ARM64 through the existing workflow. No new dependency or ISA build flag is
needed. GPU and upstream sources are unchanged.

Native macOS ARM64 with AppleClang 21: the new test passes in Release and with
ASan/UBSan (`detect_leaks=0`, unsupported by Apple ARM64 ASan). Baseline x86-64
cross-compilation with warnings treated as errors passes; local execution is
unavailable because this host rejects x86-64 executables. Native x86 runtime
coverage must come from hosted CI, not the synthetic probe matrix.
The complete native standalone Release suite passes **28/28** in **75.02 s**,
including independent FFmpeg decode and the existing 16/120-frame SDK reference
manifest gates. The six CI-inventory regression tests and thirteen privacy-guard
tests also pass. These are CPU-only results.

Portable reproduction (requires FFmpeg and ffprobe on PATH):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_ENABLE_METAL=OFF \
  -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=OFF \
  -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

GPU runtime qualification is outside P0; no CUDA/Metal/Vulkan PASS is inferred
from CPU-only CI. P1 and P2 remain separate work after P0 review and their
respective dependencies.
