<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# SIMD inventory and Windows qualification refresh, 2026-10-27

PR #278 is forward-integrated with main
`45700478cbf088b6768c45d93a91ba87a082a014` from its previous child head
`68029065501d1944276c347f0c3af3f548cabdea`. Parent #77 is unchanged.
The tested precommit source tree is
`0359f0ffbc281b1856bbd82ad35397bf08e58764`. The resulting publication commit
adds only this report and sanitized evidence to that tested tree; configure-time
HEAD remains the previous child, not an uncreated future commit.

## Inventory correction and negative mutations

`ci/run_standalone.py::REQUIRED` now includes `wavelet_simd_exact` and
`wavelet_simd_packets`. The permanent inventory regression independently drops
each name and requires the error to identify the missing registration.
All 12 inventory unit tests pass.

The actual CMake registrations were also removed one at a time locally,
followed by reconfiguration and collection of the real CTest JSON inventory.
Each removal made `registered_tests()` fail for the missing test. Both source
files were restored byte-for-byte in a finally block and CMake was reconfigured;
the restored real inventory passes. The mutation is not present in the commit.

## Native correctness and memory checks

Windows 11 Home 10.0.26200, AMD Ryzen 9 7940HS, 8 physical cores / 16 logical
processors; MSVC 19.44.35229.0, VS2022 x64, Windows SDK 10.0.26100.0,
CMake 3.31.6. Standalone uses upstream OFF, CUDA OFF, tests ON, required
FFmpeg/ffprobe ON. The SDK build uses standalone OFF/upstream ON/CUDA OFF.

| Run | PASS | FAIL | SKIP | Time |
|---|---:|---:|---:|---:|
| Full SDK Release | 44 | 0 | 2 | 90.55 s |
| Full standalone Release | 79 | 0 | 1 | 426.04 s |
| Scoped Debug | 3 | 0 | 0 | 91.86 s |
| Scoped MSVC AddressSanitizer | 3 | 0 | 0 | 95.66 s |
| Scoped SIMD disabled | 3 | 0 | 0 | 6.97 s |

The three scoped tests are CPU ISA selection, exact coefficients and complete
FilmScan3 packets. Native Debug and ASan execute Scalar, AVX2 and AVX-512
separately: 2,174 deterministic coefficient/guard/padding cases each, 6,522
total. Sixteen ordered full frames match Scalar in packets, hashes, quantizers
and state; the frozen first packet matches. AVX2=1 and AVX512=1 are observed,
not inferred from CTest exit zero. SIMD-disabled execution reports only Scalar
cases, HW_PENDING for unavailable experimental ISAs and AVX2=0/AVX512=0;
its unsupported requests return NOT_SUPPORTED and are not native SIMD passes.

ISA admission has 4,096 synthetic CPUID/XCR0 probes and 1,024 selection
configurations plus a native probe. XGETBV is guarded by XSAVE, OSXSAVE and
AVX; YMM state requires XCR0 0x6 and AVX-512 requires 0xe6 plus F/BW/VL CPU
features. Target-specific compiler switches remain confined to their respective
translation units. Production validated backends remain empty and Auto stays
Scalar. This inventory correction authors no kernel arithmetic, public API or
GPU change; API updates are inherited from main. Frozen upstream is unchanged.

SDK skips remain the two unqualified pixel-oracle gates. The standalone skip
is the live Windows symlink-privilege prerequisite. Neither is a correctness
pass. MSVC ASan is the available memory sanitizer; no UBSan/TSan execution is
claimed on this Windows toolchain.

Sanitized results are in
[`windows-simd-refresh-20261027/`](windows-simd-refresh-20261027/).
Performance comparison is recorded separately after correctness completes;
kernel timings and injected full-encoder timings must not be conflated.
No production speedup or acceleration milestone is claimed. The child is offered
for parent review, with no automatic parent/main merge, release or history rewrite.

## Performance observation after correctness

The existing `benchmark_codec` ran at 1080p, 4K and 8K and was compared with
`benchmarks/results/reference_windows_msvc.json`. The linked performance JSON
retains FPS, MPix/s, average/minimum/maximum latency and peak working set,
baseline ratios, and 30 separate SIMD timing rows (one/eight workers, three
warmups, 31 stage measurements and 16-frame injected sessions). Kernels and
full encoder times are distinct. Clocks/power are OS-managed and affinity is
unpinned; external GPU utilization varied from 3% to 26%. These are observational
measurements, not a controlled production speedup or regression acceptance.
Production dispatch and all measured algorithms are unchanged.

The first 8K SDK decode was 0.916 times baseline FPS; a separately retained
8K-only repeat was 1.139 times baseline. This variation on the same unchanged
executable prevents attributing either result to the inventory correction. Both
runs are retained in the performance JSON; no best-result selection is used.
