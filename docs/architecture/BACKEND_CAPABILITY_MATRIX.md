<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Backend capability and evidence matrix

Status: proposed P0 companion to [the ADR](MEDIA_ENGINE_ADR.md), issue #34.
Source baseline: `ae64f5f48a21844662215892ddc8cc957de4a20f`, reviewed 2026-10-16.
This is a routing/qualification plan. No hardware probe was performed for this
matrix; device IDs, driver versions and interop results are **PENDING** throughout.
Existing source and checked-in reports establish an implementation seam, not a
new validation result on a named machine. See [gates](MEDIA_PIPELINE_GATES.md).

## Status vocabulary

`SOURCE_PRESENT` means code exists at the baseline. `PLANNED` means no integrated
adapter is established by this audit. `PROBE_PENDING` requires actual device/API
inspection. `REFERENCE_PENDING` requires codec oracle evidence. `VERIFIED` is
reserved for the exact operation/environment/fixture combination passing all
applicable gates. Missing evidence remains unknown, never an inferred pass.

| Target | Compute route and source status | Media route (separate capability) | Next evidence required |
| --- | --- | --- | --- |
| CPU x86-64 | Scalar C++17 standalone SOURCE_PRESENT; SIMD dispatch is a future per-stage capability | Optional software adapter PLANNED | Native build/tests, golden stage/packet parity; detect ISA before enabling SIMD |
| CPU arm64 | Same scalar core SOURCE_PRESENT; NEON adapter PLANNED | Optional software adapter PLANNED | Native build/tests and identical integer stages; no x86 SDK dependency |
| Windows NVIDIA | CUDA SOURCE_PRESENT in legacy wrapper path; Vulkan and D3D12 compute PLANNED | NVENC/NVDEC PLANNED | Actual PCI vendor/device, driver/runtime, CUDA arch, operation/profile and memory-contract results; interop PROBE_PENDING |
| Windows AMD iGPU (requested Radeon 790M) | Vulkan first second-vendor target; D3D12 interop PoC PLANNED | AMF / OS media PLANNED | Resolve actual DXGI/Vulkan identity and driver; integer parity and external-memory/sync probes; no inferred LLVM target |
| Linux NVIDIA A2 target | CUDA SOURCE_PRESENT; Vulkan qualification tracked by #31 | NVENC/NVDEC or VA-API only after per-device API query | Actual PCI/driver/runtime, headless queue support, negotiated PCIe link and transfer cost; sanitizer gate #38 |
| Apple M2 Pro target | Metal FilmScan3 frontend SOURCE_PRESENT; scalar fallback SOURCE_PRESENT | VideoToolbox PLANNED | Registry/device identity locally, OS/Metal version, required-GPU tests, buffer limits, peak unified-memory use; interop PROBE_PENDING |
| Intel Arc future target | Vulkan PLANNED; SYCL experimental only after approval | Intel VPL PLANNED | Actual device/driver/API queries, integer parity and exact profile/surface support |

For all rows, publish only sanitized vendor/device model identifiers and driver/API
versions. Stable serials, device UUIDs, registry paths, hostnames and raw environment
dumps stay local. Use a run-local device alias in public evidence. PCI vendor/device
IDs identify hardware classes; they are distinct from personal machine identifiers.

The current FilmScan3 Metal path accelerates color and wavelets; host quantization,
entropy and writer remain CPU stages. The standalone FilmScan3 profile is fixed
1080p progressive YUV422 p10 GOP1 at the baseline. RGB/RGBA/Bayer, FIELDPLUS/GOP2,
other quality profiles and larger geometry require their own evidence. The CUDA
wrapper path is not evidence of SDK-free CUDA FilmScan3 support.

## API probes and selection record

| Route | Minimal local probe | Portable evidence retained |
| --- | --- | --- |
| CPU | Architecture, ISA feature detection, compiler/build configuration | Architecture, enabled ISA, source/binary hashes, test scope |
| CUDA | Enumerate device; query compute capability, driver/runtime, memory and copy properties | Model, PCI vendor/device IDs, versions, arch, limits, sanitizer status |
| Vulkan | Enumerate physical devices, queue families, integer features, buffer/image formats, external memory/semaphore capabilities | Vendor/device IDs, driver/API versions, explicitly enabled features and tested import pairs |
| D3D11/12 | DXGI adapter matching; resource/format and fence sharing probes | Vendor/device IDs, driver/feature levels, tested surface/queue pair; keep adapter LUID local |
| Metal | Enumerate devices; query supported features, maxBufferLength, allocation budget and command failure behavior | Model, OS/API facts, measured limits and workspace bytes; keep registry identity local |
| Media API | Query encode/decode operation, codec/profile/level, bit depth, chroma, dimensions and surface types | Supported tuples and rejected requests, version, actual output validation |

An import route additionally needs compatible allocation type, layout, access
rights, same-device identity and synchronization. A successful import call is not
proof that pixels or completion semantics are correct. Disallow zero-copy claims
until the round-trip content and lifetime tests pass. Driver updates invalidate
cached qualification keys; device loss invalidates the current device generation.

Selection reports requested operation, GPU scope, precision, surface formats,
transfer policy, compatible candidates, rejection reasons and selected stage plan.
Measured costs must include transfers, waits and host work under the same input
contract. If evidence or cost data is missing, report that fact; use an explicit
caller preference or CPU default rather than manufacture a fastest-device claim.

## Vendor constraints and current primary sources

AMD's [Windows HIP SDK requirements](https://rocm.docs.amd.com/projects/install-on-windows/en/latest/reference/system-requirements.html)
were checked on 2026-10-16 (page identifies HIP SDK 7.2.0). Radeon 790M is absent
from that supported GPU/APU table. Consequently this plan does not admit it as an
officially supported Windows HIP target. Recheck the exact SDK release before a
future probe; a successful experimental run cannot replace vendor support.

[NVIDIA's NVENC documentation](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-application-note/index.html)
distinguishes dedicated encoding hardware from CUDA compute.
The [Video Codec SDK overview](https://developer.nvidia.com/video-codec-sdk)
lists H.264, HEVC and AV1 encoding. Do not infer CineForm ASIC support from CUDA
support, or NVENC/NVDEC availability on a named GPU without querying that device.
A general media adapter must reject unsupported CFHD requests explicitly.

[Intel VPL](https://github.com/intel/libvpl) provides API/dispatcher components and
uses a separate GPU runtime. Use the name Intel VPL; SYCL compute and VPL media
capabilities are independently queried. Apple VideoToolbox, Windows media APIs,
VA-API/V4L2 and Vulkan Video are future per-operation adapters; none is granted a
CineForm hardware capability by this architecture.

## Promotion sequence

1. Keep CPU and existing CUDA/Metal correctness/safety work moving (#23, #29,
   #33, #36, #38). Preserve one shared integer and bitstream reference.
2. Qualify Vulkan compute under Linux (#31), then run the same deterministic
   vectors on a real Windows AMD device. No duplicated writer or quality state.
3. Admit one D3D12 or native-surface interop PoC only for a measured workflow.
4. Add one optional media codec adapter with its own conformance/license review.
   HIP, SYCL, Intel Arc and broader interop follow explicit device qualification.

GPU-free CI validates the portable core only. A row becomes VERIFIED only through
linked, sanitized evidence for its exact request tuple, never because another OS
or vendor passed the same source build.
