<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Tesla M4 / SM 5.2 capability matrix and 1080p repeatability (2026-10-25)

Status: read-only native evidence. Issue: #116.
Base: protected `main` `55b0e9c32567a8494c06725f24e2b7d7aa75deda`.

This note records the two evidence items that `docs/TESLA_M4_SM52_SUPPORT_POLICY.md`
still listed as unrecorded — an embedded-code-verified capability matrix and a
bounded repeated 1080p measurement — at the current `main` rather than at the
older heads the policy document cites. It adds no production-code change and
does not qualify any configuration for release.

## 1. Source, toolchain and device

| field | value |
| --- | --- |
| Tested commit | `55b0e9c32567a8494c06725f24e2b7d7aa75deda` (protected `main`) |
| Build | CMake 3.31.12, Ninja 1.13.2, `Release` |
| CUDA | 12.8.93 `nvcc`; `CMAKE_CUDA_ARCHITECTURES=52` (explicit, not `native`) |
| `cuobjdump` | reports `V12.8.90` |
| Host compiler | MSVC 19.44.35229.0, Release |
| Device | Tesla M4, compute capability 5.2, 8 SMs, `3,953,131,520` B |
| Idle device state | `3,840 MiB` reported by `nvidia-smi`, `10 MiB` in use, `P8`, ECC enabled |
| Driver | 582.78, CUDA driver API `13.0` (newer than the 12.8 runtime) |
| CUDA runtime | 12.8 (`12080`) |
| NVML | available; `nvidia-smi` works |
| Host class | Windows 11 x64 guest, AMD EPYC 7601, 8 logical processors, no SMT |

`nvcc` warns that offline compilation for targets below `sm_75` is deprecated.
`sm_52` still compiles in CUDA 12.8, which is why the policy document pins a
CUDA 12.x lane; the warning is the build-time signal that a later toolkit will
drop the target.

## 2. Embedded code, not a successful link

The build produced 156 Ninja steps with exit `0` and no compiler errors.
`cuobjdump` shows real `sm_52` device code and `sm_52` PTX in the artifacts
under test:

| Artifact | `sm_52` cubins (SASS) | `sm_52` PTX |
| --- | ---: | ---: |
| `hypercine_m4_resolution_probe.exe` | 3 | 3 |
| `hypercine_wavelet.lib` | 6 | — |
| `test_cuda_wavelet.exe` | 3 | 3 |
| `test_phase2c.exe` | 3 | 3 |
| `test_entropy_capacity.exe` | 1 | 1 |

`cuobjdump -sass` prints `code for sm_52` with both `Fatbin elf code` and
`Fatbin ptx code` sections. Driver acceptance is demonstrated by execution: the
probe loads this binary and produces bit-exact encoder output on the device
(§4), so this is not a PTX-only or JIT-only observation.

## 3. Device feature detection

Attributes queried on the device with a standalone CUDA program built for
`-arch=sm_52`:

| Capability | Value | Consequence |
| --- | --- | --- |
| `cudaDevAttrMemoryPoolsSupported` | `0` | no memory pools |
| `cudaMallocAsync(4096)` | `cudaErrorNotSupported` | the asynchronous allocator is unavailable; the repository has no `cudaMallocAsync` call site and uses classical allocation only |
| cooperative launch | `0` | no `cudaLaunchCooperativeKernel` call site exists in `src/` or `include/` |
| compute preemption | `0` | preemption-dependent designs are not viable |
| stream priorities / async engines | `1` / `2` | the existing multi-stream design is supported |
| L2 cache | `1,048,576` B | small working set; consistent with the measured capacity ceiling |
| max shared memory per block (opt-in) | `49,152` B | no 64 KiB opt-in on this part |
| managed memory / unified addressing / `canMapHostMemory` | `1` / `1` / `1` | pinned and mapped host buffers available |
| `canUseHostPointerForRegisteredMem` | `0` | registration of arbitrary host pointers is not allowed |

The async-allocation and cooperative-launch assumptions named by the issue are
therefore answered from the device rather than from documentation, and neither
is required by the current code.

## 4. Staged 1080p comparison

Probe: `hypercine_m4_resolution_probe identity 1920 1080`, fixed seed
`0x1337BEEF`, identical inputs, one fresh process per run. The probe compares
the same stages the issue asks about:

| Stage | Report field | Result in every run |
| --- | --- | --- |
| wavelet + quantized coefficients | `coeff` | `1` — 30/30 bands equal |
| entropy payload | `len`, `payload` | `1` — per-band length and bytes equal |
| normalized CFHD sample, all D2H paths | `sample`, `streamed`, `multistream`, `gpulowpass` | `1` — sample SHA-256 `2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52` on every path |
| frozen GoPro decoder pixels | `decoder_pixels` | **unstable** — see §5 |

No CUDA API call failed and no allocation failed in any run. The identity
sequence used about `53.5 MB` of device memory before and `57.7 MB` after, so
this is not a memory-pressure observation.

## 5. Repeat matrix and decoder instability

Each row is one fresh process, same inputs, same binary.

| Arm | Runs | Pixel gate PASS | Failure classes |
| --- | ---: | ---: | --- |
| normal | 10 | 8 | 2 × first decode differs, `max_diff=236`, `psnr_vs_source=44.465986` |
| `CUDA_LAUNCH_BLOCKING=1` (diagnostic only) | 10 | 4 | 3 × first decode differs with `psnr_vs_source` unchanged (44.465986), `max_diff=211…237`; 3 × second decode differs with `psnr_vs_source` collapsing to 16.374, 18.606 and 22.467 dB |

- Eight distinct corrupt pixel hashes were observed; the correct hash
  `c5e0e052be84a807a899d0501fa73f93bbe9d67e0f1c9e5cfdef97a3ffee54a0` is the only
  stable one.
- **Within-process divergence:** in every failing run both decode calls received
  byte-identical samples (`sample=1` proves the packets are equal) and returned
  different pixels. The nondeterminism is not only cross-process; one process
  can produce two different images for the same bytes.
- **Pipeline path:** `pipeline 1920 1080 1 2 2 host|device`, 4 runs — all
  `ok=1`, `bitstream_match=1`, `decode_ok=1`, PSNR `44.465986`. The pipeline
  PSNR failure quoted in the issue did not reproduce in this bounded set.
- The launch-blocking arm changes timing and heap state. The different failure
  rates (2/10 vs 6/10) narrow hypotheses but do not prove a race, and both arms
  are single-device samples of `n=10`.

## 6. Registered suite at this head

`ctest --test-dir build-cap -j1` (CUDA-ON Release, one test at a time):
**37 registered — 32 passed / 4 skipped (return code 77) / 1 failed**, 388.95 s.

- Skips: `test_phase2a`, `test_phase2b`, `test_phase2d` and
  `standalone_sdk_decode_3_packets` — the documented
  `BLOCKED_ORACLE_NOT_QUALIFIED` lane.
- The only failure is `test_phase2c`, and its sole cause is the measured
  capacity boundary:
  `FAIL: 8K DeviceInput Consolidated strict encoder/bitstream gate failed: UNSUPPORTED_RESOURCE_BOUND: GPU entropy stream 2 entropy workspace allocation failed: requested 33178624 B; free 23789568 B of 3953131520 B; CUDA cudaErrorMemoryAllocation`.
  The row stays `FAIL`.
- In the same run the identity pixel stages happened to pass: 1080p
  (`900396` B, decoded `c5e0e052…`, `max_diff=0`, `44.466 dB`), 4K
  (`3563684` B, `44.5539 dB`) and 8K (`14009028` B, `44.5823 dB`), each
  `reference_decode=EQUAL`. Pixel gates green inside the suite while 2 of 10
  standalone probe runs produced corrupt decodes is the nondeterminism itself,
  not a change in the codec path.

## 7. Interpretation

The GPU wavelet, quantization, entropy and packet-assembly stages are bit-exact
on SM 5.2 across every exercised path and every repeat. The 1080p identity
failure quoted in the issue reproduces at this head, but only in the frozen
GoPro decoder's pixel output. That rules out an M4 kernel or SM 5.2 arithmetic
cause for that symptom and confirms the existing `G2P NOT QUALIFIED` status.

Two consequences for any future gate:

1. **PSNR cannot carry this gate.** A locally corrupt decode still reported
   `44.47 dB`, because compression error dominates the metric. Only exact byte
   comparison (or a structural check) detects that class. The identity test's
   exact-hash comparison is the correct gate, and a PSNR threshold is not a
   substitute for it.
2. **`mismatch_at=-1` is not an offset.** Every failure reports `-1` because the
   field refers to a packet mismatch, and the packets are byte-identical by
   construction. It must never be read as a pixel or byte offset.

## 8. Reproduction

```
cmake -S . -B build-cap -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_ENABLE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=52 \
  -DHYPERCINE_BUILD_UPSTREAM=ON -DHYPERCINE_BUILD_TESTS=ON \
  -DHYPERCINE_BUILD_BENCHMARKS=OFF
cmake --build build-cap
ctest --test-dir build-cap -j1
build-cap/hypercine_m4_resolution_probe identity 1920 1080
CUDA_LAUNCH_BLOCKING=1 build-cap/hypercine_m4_resolution_probe identity 1920 1080
build-cap/hypercine_m4_resolution_probe pipeline 1920 1080 1 2 2 host
```

Embedded-code check:

```
cuobjdump --list-elf  build-cap/hypercine_m4_resolution_probe
cuobjdump --list-ptx  build-cap/hypercine_m4_resolution_probe
cuobjdump -sass       build-cap/hypercine_wavelet.lib
```

On Windows the executables carry an `.exe` suffix. A run at the same commit on
hardware without an SM 5.2 device does not reproduce §4–§6 and is not a
substitute for it.

## 9. Non-claims

- Not an M4 production, release or customer-support qualification; no
  configuration moves out of `LEGACY_NOT_CERTIFIED`.
- Not an 8K envelope: only the registered `test_phase2c` configurations were
  executed, and the 3-slot × 4-stream row remains a hard `FAIL`.
- Not a pixel-oracle result. The original GoPro decoder reference remains
  `BLOCKED_ORACLE_NOT_QUALIFIED` under #120; the decoder instability recorded
  here belongs there and is not diagnosed in this note.
- Not a proof of or against a data race: both repeat arms are `n=10` and the
  launch-blocking arm deliberately changes timing.
- Not a performance result; no FPS target is claimed for this device class.
- No codec, kernel, allocator, threshold, gate, CTest property or `upstream/`
  change accompanies this note.
