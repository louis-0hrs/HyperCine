<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Current-main Windows SIMD integration, 2026-10-27

PR #77 source head `3f612a83a286815a7969d1759b33753bb2f821a3` is reconciled
with main `a267db4f1d9f41955c59675f2bd3690c30f7acb5` on isolated branch
`codex/windows-simd-integration-20261027`. Tested source revision:
`e39076e2be0aba740da03a13f01078dfdf9cefb4`. The native builds preceded sealing
this source snapshot as a commit; no
implementation changed afterwards. Build-time HEAD was #77 with integration
changes pending. A subsequent evidence commit changes documentation only. This
proposes an update to the owner branch; neither the owner branch nor
protected main is automatically modified.

## Reconciliation and runtime boundary

Resolved the two actual merge conflicts in root `CMakeLists.txt` and `.gitignore`.
Keep both current-main CUDA/Vulkan setup and the SIMD benchmark registration;
keep both shared SDK-free/NEON registrations and the SIMD packet test; preserve
all current fixture-verifier exceptions along with the SIMD CMake module.
No conflict markers remain. All SIMD kernel/frontend/test arithmetic is unchanged
from #77. Frozen `upstream/`, public APIs and GPU implementations are unchanged.

The generated MSVC project confirms `/arch:AVX2` only on `wavelet_avx2.cpp`
and `/arch:AVX512` only on `wavelet_avx512.cpp`. The wrapper, scalar filter,
runtime ISA probe and caller retain baseline compilation. Runtime admission
uses the existing CPU feature and XCR0 checks, including AVX512F/BW/VL and
OS YMM/ZMM state; unsupported/unknown ISA requests fail closed. Production
`ValidatedCpuIsaBackends()` stays empty and `ResolveCpuIsa(Auto)` stays Scalar.
The injected SIMD frontend owns its ISA per test encoder; it is not production
backend registration or a process-global ISA toggle. Existing current-main
session capability behavior remains in place.

The only additional test-harness correction explicitly decodes CMake diagnostic
text as UTF-8 rather than the Windows ANSI code page. It preserves process
return codes and every build/target assertion. Use a short local TEMP/TMP for
nested MSVC configure probes; the initial long-TEMP SDK failure is retained
as `simd-sdk-long-temp.json`, not hidden.

## Native evidence

Windows x64, Ryzen 9 7940HS (8 physical / 16 logical processors), MSVC
19.44.35229.0 / Visual Studio 2022, Windows SDK 10.0.26100.0, CMake 3.31.6.
AVX2 and AVX-512 actually execute on this host. All ordinary builds are Release.

| Run | Actual result |
|---|---|
| Full SDK-free standalone, required FFmpeg | 76 PASS, 1 platform BLOCKED, 0 FAIL; 77 registrations |
| Full frozen-SDK CPU suite | 43 PASS, 2 reference BLOCKED, 0 FAIL; 45 registrations |
| `wavelet_simd_exact` | Scalar 2,174 + AVX2 2,174 + AVX-512 2,174 = 6,522 deterministic coefficient/guard/padding cases |
| `wavelet_simd_packets` | 16 ordered complete FS3 frames, byte/coefficient/quantizer/state equality; frozen first packet unchanged; AVX2=1 / AVX512=1 |
| MSVC AddressSanitizer | Both SIMD tests PASS, including native AVX2/AVX-512 execution, 6,522 cases and 16 frames |
| `HYPERCINE_BUILD_X86_WAVELET_SIMD=OFF` | Both tests PASS; Scalar 2,174 cases, explicit SIMD requests unavailable; packet report AVX2=0 / AVX512=0 |

The platform-only output-directory negative remains BLOCKED on Windows. The two
SDK pixel-oracle gates remain BLOCKED. A successful CTest process with these
skips is not a qualified original-SDK pixel oracle. These are native x64 tests;
no new 32-bit Windows, AVX2-only physical-host or hosted CI results are inferred.
The portable ISA tests also exercise missing CPU/OS-state combinations.

Sanitized complete-suite and scoped results are in
[`windows-native-20261027/`](windows-native-20261027/). Raw local build/CTest
logs and JUnit are ignored. No private corpus was required for own SIMD/scalar
equality, and no new original GoPro golden parity is claimed.

## Performance and review boundary

The unchanged production `benchmark_codec` was run at 1080p / 4K / 8K and
compared with `benchmarks/results/reference_windows_msvc.json`. The sanitized
[performance report](windows-native-20261027/performance.json) records FPS,
MPix/s, avg/min/max latency and peak memory, plus 30 isolated SIMD timing rows
(median/p95, 1 and 8 workers, three warmups, 31 stage repeats and 16-frame
injected sessions). These timings are **INCONCLUSIVE** for regression or speedup
attribution: external GPU load was 98% at start / 97% at end, temperature 85 C,
and native CPU qualification overlapped. No external workload was stopped.
Production dispatch and the codec implementation measured by `benchmark_codec`
are unchanged from main; measurements do not establish controlled performance
acceptance or a production speedup. No acceleration performance milestone is
closed by this candidate.

No production Auto activation, throughput promise, merge, release or broader
encoder/decoder milestone is approved by this report. The scoped outcome is
a conflict-free integration candidate with native AVX2/AVX-512 exactness and
preserved kernel/runtime isolation. Owner review and exact published-head CI
remain separate gates.

To reproduce, configure Visual Studio 2022 x64 standalone with upstream OFF,
tests ON and required external FFmpeg/ffprobe; build Release and run the full
CTest suite. Configure SDK CPU separately with standalone OFF/upstream ON.
For the disabled path set `HYPERCINE_BUILD_X86_WAVELET_SIMD=OFF`, then run
`ctest --test-dir <build> -C Release -R '^wavelet_simd_' --output-on-failure`.
For MSVC ASan add `/fsanitize=address /Zi`, disable incremental linking and
make the installed ASan runtime available locally; execute the same two tests.
Keep host paths and raw logs out of publication.
