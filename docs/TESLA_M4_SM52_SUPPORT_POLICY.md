# NVIDIA Tesla M4 (SM 5.2) legacy support policy

Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0; see LICENSE-MIT and LICENSE-APACHE.

This document is the research-only recommendation requested by
[#116](https://github.com/louis-0hrs/HyperCine/issues/116): it separates the
different meanings of "supported" for a legacy 4 GiB-class Maxwell device,
defines the configuration envelope that the measured evidence actually
covers, and proposes a minimal native plus hosted verification matrix with
explicit PASS / FAIL / SKIP semantics.

It is **not** a release qualification, a production-decoder certificate or a
pixel-accuracy claim. The original GoPro decoder reference remains
`BLOCKED_ORACLE_NOT_QUALIFIED` under
[#120](https://github.com/louis-0hrs/HyperCine/issues/120), so no decoded-pixel
result below is evidence of codec quality. Machine-identifying values
(hostnames, share paths, GPU UUIDs, PCI IDs) are deliberately omitted.

## 1. Evidence boundary

The native measurements referenced here were taken on one Windows x64 QEMU
guest with a single Tesla M4. They are **one device-class sample**, not a
hardware-fleet survey, and they do not transfer automatically to other
Maxwell parts, other drivers or other CUDA toolkits.

| Attribute | Recorded value |
| :--- | :--- |
| GPU device class | NVIDIA Tesla M4, 4 GiB class |
| CUDA compute capability | 5.2 |
| Build architecture | `sm_52`, selected with an explicit `-DCMAKE_CUDA_ARCHITECTURES=52` |
| CUDA toolkit / runtime | 12.8 (12.8.93) |
| Driver | 582.78 |
| Compiler | MSVC 19.44.35229.0, Release |
| Build system | CMake 3.31.12 with Ninja 1.13.2 |
| Host | Windows 11 x64 guest, AMD EPYC 7601, 8 vCPU, no SMT |
| CUDA-enabled clean build | 172/172 steps, no `INCLUDE` workaround ([#218](https://github.com/louis-0hrs/HyperCine/pull/218), integrated on `main` `b51e5e5`) |

`CMakeLists.txt` defaults an unset `CMAKE_CUDA_ARCHITECTURES` to `native`.
A `native` build on modern hardware therefore never demonstrates SM 5.2
support; an SM 5.2 claim requires the explicit `52` target and a device that
accepts the resulting binary.

Provenance anchors for the measurements in the following sections:

| Subject | Source heads |
| :--- | :--- |
| Clean CUDA build on SM 5.2 | #218 `a2bec2d`, `main` `b51e5e5` |
| Capacity diagnostics and injection | #194 `aadf1e9`, `main` `827f448` |
| Fail-closed reference gate and skip inventory | #206 `45e48e8`, `main` `b274587` |

Hosted workflows at those heads are green, but they run with CUDA disabled and
neither compile nor execute the Phase 2 CUDA tests. They cannot substitute for
native evidence in either direction.

## 2. Gates that must stay separate

A single green test count does not establish any of these gates. They are
ordered, and a later gate never repairs an earlier one.

| Gate | What it proves | SM 5.2 status |
| :--- | :--- | :--- |
| **G0 buildable** | The CUDA sources compile and link for `sm_52` with a pinned toolchain. | **PASS** — clean 172/172 Ninja build. |
| **G1 launches kernels** | The built binary is accepted by the driver and executes on the device. | **PASS** — CUDA Phase 2 tests and the entropy-capacity concurrency test execute natively. |
| **G2 strict encode equivalence** | Encoder input -> quantized coefficients -> entropy payload -> complete sample, plus geometry, metadata and resource checks, are byte-identical for the exercised synthetic inputs. | **PASS for the exercised configurations.** This gate is always fatal and is never derived from decoded pixels. |
| **G2P original-SDK pixel equality** | Decoded pixels and PSNR from the frozen original decoder match a trustworthy reference. | **NOT QUALIFIED.** The frozen decoder is nondeterministic for identical packets on this class (#120/#205/#206). The Phase 2 tests report exit `77` for the pixel gate unless an earlier strict gate already failed. |
| **G3 repeated stateful 1080p** | Repeated stateful runs produce stable strict results and a qualified pixel result. | **NOT QUALIFIED.** The pixel signal is exactly the unstable one quarantined by #120; full-pipeline repeatability under a qualified oracle has not been established on this class. |
| **G4 4K** | Same as G2/G3 at 3840×2160. | **Strict gates PASS at the measured configuration** (3 slots, 4 streams, host input, band-streamed D2H). Pixel gate **NOT QUALIFIED**. |
| **G5 8K** | Same as G2 at 7680×4320, stated per exact slot/stream configuration rather than as an envelope. | **Measured at three configurations only**, each with device input and two frames in one bounded run: 1 slot × 4 streams, 2 slots × 4 streams and 3 slots × 2 streams passed the strict encoder/bitstream gates, and in those runs the reference decode completed and its PSNR gate happened to pass. Those are not qualified pixel results (G2P), and these are not G3-style repeated runs. **3 slots × 4 streams fails hard** (see §3). Capacity failure is a FAIL, never a SKIP. No other combination is measured and none is inferred. |
| **G6 useful FPS** | Sustained throughput meets a declared target for a declared configuration. | **NOT MEASURED / NOT QUALIFIED** on this device class. No FPS figure is claimed here. |

Consequences:

- "18/20 CUDA tests green" or "23/26 native CTest" describes execution, not
  qualification. The missing pieces are a qualified pixel gate and a
  release-capable capacity envelope, and they stay visible.
- No configuration on this device is currently a **release** target, because
  G2P cannot pass until an independent pixel oracle is qualified and reviewed.

## 3. Measured resource ceiling

The 8K 3-slot × 4-stream configuration on the 4 GiB-class device requested
`33,178,624` bytes for the entropy workspace while only `24,838,144` bytes
were free, as recorded in the #194 capacity-diagnostics lane. The allocation
returned `cudaErrorMemoryAllocation`; the encoder reported `encoded=0` and a
nonzero exit with an `UNSUPPORTED_RESOURCE_BOUND` classification. This is a
genuine capacity boundary for that exact configuration and device class, not a
kernel-arithmetic mismatch and not an OOM explanation for unrelated 1080p
failures. The free-byte figure is one observation of that run; it is not a
fixed property of the device class.

Three other 8K pipeline configurations were observed to complete their strict
encoder/bitstream gates: 1 slot × 4 streams (`2,333,736,960` bytes measured),
2 slots × 4 streams (`3,381,002,240` bytes) and 3 slots × 2 streams
(`3,874,750,464` bytes, 98.0% of device memory). Each was a single bounded run
with device input and two frames, and in each the included original-SDK
reference PSNR gate happened to pass. That gate is nondeterministic on this
class (#120) and is not a qualified pixel oracle.

These are **three measured points, not a capacity envelope.** No other
slot/stream combination was run, no free-memory preflight is implemented, and
the 3-slot × 2-stream point leaves no demonstrated headroom. The observations
do not upgrade 8K to release quality, because G2P is still blocked.

The one-shot allocation-failure injection added in #194 is process-safe,
validated concurrently, and remains test-only. It does not change production
allocation policy.

## 4. Supported configuration envelope (recommendation)

These labels describe what evidence exists, not what customers may ship.

| Configuration | Label | Basis |
| :--- | :--- | :--- |
| 1080p (1920×1080) Phase 2 encoder configs exercised by the native suite | **Diagnostic (strict gates)** | G0–G2 evidence; pixel gate blocked. |
| 4K UHD (3840×2160), 3 slots / 4 streams, host input, band-streamed D2H | **Diagnostic (strict gates)** | G0–G2 evidence at that configuration. |
| 8K UHD (7680×4320), exactly 1 slot × 4 streams / 2 slots × 4 streams / 3 slots × 2 streams (device input, two frames) | **Diagnostic (strict gates), measured points only** | Three bounded runs; strict encoder/bitstream gates passed and the included PSNR gate happened to pass. Not an envelope, not repeated-stateful, no free-memory preflight. |
| 8K UHD, any other slot/stream combination | **Unmeasured** | No evidence; not inferred from the three measured points. |
| 8K UHD, 3 slots × 4 streams on a 4 GiB-class device | **Unsupported (hard failure)** | Measured request exceeds free memory; must fail, not skip. |
| Any decoded-pixel / PSNR acceptance | **Blocked** | #120 oracle not qualified. |
| Production decoder or NLE interoperability | **Out of scope** | No decoder promotion in #116. |
| Performance acceptance | **Not measured on this class** | No SM 5.2 FPS claim exists. |

Policy rules that follow:

1. **Explicit architecture.** An SM 5.2 claim requires an `sm_52`-targeted
   build on an SM 5.2 device. A generic `native` build proves nothing about
   legacy hardware.
2. **Pinned legacy toolchain.** Keep CUDA 12.x with a classical allocator path
   for the legacy lane. Do not install, downgrade or replace a system-wide
   driver or toolkit without maintainer approval. Optional modern features
   such as asynchronous allocation or CUDA Graphs must be feature-detected
   and must have a working classical-allocation fallback; they may not become
   a hard requirement of the legacy build.
3. **Fail-closed capacity.** Preflight unsupported high-concurrency
   configurations with a real free-memory query; propagate
   `cudaErrorMemoryAllocation` as a strict resource failure with a diagnostic
   record. Never convert it into a skip, retry loop or success.
4. **No silent pixel promotion.** A passing strict encode gate never implies
   pixel accuracy, and a `BLOCKED_ORACLE` result never counts as coverage.
5. **One core, no fork.** Architecture-specific kernels stay behind the
   existing capability seam; the legacy path must not fork the codec or
   lower modern-GPU targets.

## 5. Result semantics

| Observed result | Meaning | Allowed use |
| :--- | :--- | :--- |
| Exit `0` | The test's complete contract passed, including any pixel gate it owns. | Only tests without an unqualified pixel gate can reach this on the M4. Phase 2 tests do not, until #120 qualifies an oracle. |
| Exit `1` | A strict contract failed: encoder payload, geometry, metadata, decode error, capacity or resource bound. | Always fatal. Must be fixed or reported as a supported-configuration limit. Never relabeled as skip. |
| Exit `77` | The strict encoder gates passed, but the original-SDK pixel gate is `BLOCKED_ORACLE_NOT_QUALIFIED` or quarantined as nondeterministic. | Reported as `BLOCKED`, registered with `SKIP_RETURN_CODE=77`. Never counted as a pass, coverage or release evidence. CTest overall success with skips is not a release gate. |
| Build / configure failure | The lane did not produce a binary. | FAIL. No test result exists. |
| Not run / missing registration | Required evidence is absent. | BLOCKED. The release inventory runner fails closed. |

`tools/check_reference_release.py` encodes these rules; it refuses a release
`PASS` even when every executed process returns zero, because no reviewed
independent pixel-oracle adapter exists.

## 6. Minimal proposed verification matrix

Hosted jobs are necessary for every change but cannot qualify any CUDA path.
The native jobs require real SM 5.2 hardware and must run one at a time on a
shared device.

| Lane | Environment | Executes | Semantics |
| :--- | :--- | :--- | :--- |
| **H1 hosted, CUDA-off** | Linux x64, Windows MSVC x64, macOS arm64 (Release and ASan/UBSan); privacy jobs | Build, CPU reference, standalone decode, schema/privacy tests | Required per commit. Never evidence for CUDA or SM 5.2. |
| **N1 native build + launch** | SM 5.2 device, pinned CUDA 12.x, `-DCMAKE_CUDA_ARCHITECTURES=52` | Clean configure and full build; `test_entropy_capacity`; bounded 1080p Phase 2 run | PASS only if the build is clean and the strict gates report PASS or the expected oracle-blocked `77`. No pixel claim. |
| **N2 native 4K (optional)** | Same | The measured 4K configuration | Strict PASS or explicit FAIL. Pixel gate remains `BLOCKED`. |
| **N3 native 8K capacity** | Same, opt-in | Capacity preflight and one bounded 8K attempt | On a 4 GiB-class device the 3×4 case must **FAIL** with a resource classification. A skip or a green rerun is a defect. |
| **N4 pixel oracle** | Independent qualified decoder, same immutable packet and normalized output | Decoded pixels / PSNR | Unavailable; **BLOCKED** until #120 delivers a reviewed adapter. |
| **N5 performance** | Same as N1 | Declared configuration, warmed, pinned measurement | Only after N1–N3 strict correctness; no cross-class extrapolation. |

Ordering: H1 -> N1 -> (N2) -> (N3) -> N4 -> N5. A later lane never upgrades an
earlier failure. N4 stays blocked, so N5 must not be presented as release
performance even if it runs.

## 7. Recommendation

Retain **SM 5.2 as a legacy build and diagnostic target** on a pinned CUDA 12.x
toolchain with an explicit `sm_52` architecture and a classical allocator
fallback, under the fail-closed rules above. Do **not** promote any M4
configuration to production or release status until:

1. #120 qualifies and reviews an independent pixel oracle;
2. targeted 1080p and 4K runs compare source, coefficients, entropy payload
   and decoded pixels separately at the same immutable packet SHA;
3. repeated stateful 1080p runs are stable under that oracle;
4. each 8K slot/stream configuration proposed for support is measured
   individually or declared unsupported; no envelope is extrapolated from the
   three measured points;
5. a declared FPS target is measured on the declared configuration.

Until then the accurate status is
`SM_52_BUILD_AND_STRICT_DIAGNOSTICS_PASS / PIXEL_ORACLE_BLOCKED / LEGACY_NOT_CERTIFIED`.

## 8. Non-claims

- Not a production CUDA decoder or encoder certification.
- Not a decoded-pixel, PSNR or application-interoperability claim.
- Not a statement that every 1080p failure on this class is caused by the
  measured 8K capacity boundary; those remain separate findings.
- Not evidence for any GPU other than the measured SM 5.2 device class.
- Not an 8K envelope: only three exact 8K slot/stream configurations were
  measured, each once, and no adjacent configuration is implied.
- Not a performance comparison or regression verdict.
