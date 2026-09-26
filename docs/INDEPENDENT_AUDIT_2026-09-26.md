<!-- Copyright (c) 2026 HyperCine Contributors. Dual-licensed under the MIT License or Apache License, Version 2.0. See LICENSE-MIT and LICENSE-APACHE for details. -->

# Independent adversarial audit — 26 September 2026

## Scope and provenance

This audit examined the **working tree as found**, not a clean commit. HEAD was `467ac53df7e7dd531df553ec858da2822de3abbe` on `phase1b-cuda-pyramid-quant`; the Phase 2D encoder, benchmark, and tests were untracked, and several build files were modified. No production change was retained. The one audit experiment that inserted `cudaDeviceSynchronize()` before the Phase 2D timer stopped restored the source byte for byte and rebuilt the normal binary. `git status --porcelain upstream/` was empty before and after.

Hardware: AMD Ryzen 9 7940HS, 16 logical processors; NVIDIA GeForce RTX 4090 Laptop GPU, 16,376 MiB, compute capability 8.9; NVIDIA driver 610.62; CUDA Toolkit 12.8.61; MSVC 19.44.35228.0; CMake 3.31.6. The isolated Release build used Visual Studio 2022 x64 and CUDA enabled.

The raw reproduction artifacts are in `build-audit/reference-repro.json`, `build-audit/benchmarks/results/phase2d.json`, `build-audit/phase2d-audit-console.txt` (first normal run), `build-audit/phase2d-audit-repro2-console.txt` (second normal run), and `build-audit/phase2d-sync-console.txt` (temporary final-device-sync run). The pre-existing `benchmarks/results/phase2d.json` is preserved. The first normal run's JSON write failed inside the sandbox; the second normal run saved JSON outside it. The temporary source edit was restored, and no saved benchmark result was overwritten.

## Verdicts

| # | Question | Verdict | Basis and limit |
|---|---|---|---|
| 1 | Build reproducibility | **PASS** | Fresh local Release CUDA build succeeded. Source state is dirty, so this is reproducibility of this workspace snapshot only. |
| 2 | Upstream immutability | **PASS** | `git status --porcelain upstream/` empty before and after. |
| 3 | Coefficient bit exactness | **INCONCLUSIVE** | Canonical sample payloads match, but the test does not independently compare CPU and GPU coefficient arrays, and variable or adversarial inputs were not tested. |
| 4 | GPU entropy byte exactness | **INCONCLUSIVE** | All 27 canonical highpass payloads match in the existing comparison; synthetic boundary, escape, and long-run arrays were not exercised. |
| 5 | Complete single-frame sample equality | **PASS** | For the canonical frame at 1080p, 4K, and 8K in two preset quantizer states, the rebuilt comparison found complete equality after identified volatile metadata masking. Scope is those six cases. |
| 6 | Stateful multi-frame sample equality | **NOT YET IMPLEMENTED** | The Phase 2D pipeline snapshots upstream quantization once and reuses it; no per-frame rate-control update occurs. |
| 7 | Metadata correctness | **INCONCLUSIVE** | Fresh clips had distinct GUIDs and identical samples after volatile-field masking; per-frame timecode, dates, and long sequences were not verified. |
| 8 | Benchmark synchronization correctness | **PASS** | For its existing repeated-frame path, CUDA completion events and D2H checks precede CPU assembly; the worker joins before the timer stops. The final-device-sync experiment found no consistent FPS reduction, though run variance prevents a precise overhead estimate. |
| 9 | Reported frame count correctness | **INCONCLUSIVE** | `encoded_frames` is assigned from `cfg.frames`; only the last sample is retained and hashed. No 300-frame unique-ID completion audit exists. |
| 10 | CUDA Graph measurement validity | **INCONCLUSIVE** | Same-kernel graph and non-graph paths were rerun, but only eight iterations and no independent GPU timeline/Nsight data are available. |
| 11 | Host-input benchmark fairness | **FAIL** | HyperCine copies BGRA into pinned memory before timing; upstream uses its ordinary host vector. Source representation is BGRA in both, but memory preparation and allocation differ. |
| 12 | Device-input benchmark validity | **PASS** | Valid as a device-resident, repeated-frame integration measurement: the BGRA upload is excluded and GPU encode/D2H/assembly are timed. It is not a host-input comparison. |
| 13 | 1080p performance claim | **INCONCLUSIVE** | Only 24 identical measured frames per Phase 2D run; no five-run median or variable sequence. |
| 14 | 4K performance claim | **INCONCLUSIVE** | Only 18 identical measured frames per run; no five-run median or variable sequence. |
| 15 | 8K performance claim | **INCONCLUSIVE** | Only 12 identical measured frames per run; saved and fresh FPS differ materially. |
| 16 | Memory safety | **INCONCLUSIVE** | CUDA memcheck reported zero errors for `test_phase2d`; three CPU ASan tests passed. Racecheck did not complete after more than six minutes and was stopped. No sustained path was sanitized. |
| 17 | Long-run stability | **INCONCLUSIVE** | No 10,000/2,000/500-frame continuous run or resource trend was available from the current benchmark interface. |
| 18 | Production readiness | **FAIL** | Stateful quantization and distinct-frame ingestion are absent from the Phase 2D pipeline; the central sequence requirement is unmet. |

## Reproduced checks

From the repository root:

```powershell
cmake -S . -B build-audit -G 'Visual Studio 17 2022' -A x64 -DHYPERCINE_ENABLE_CUDA=ON -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=ON
cmake --build build-audit --config Release --parallel 8
ctest --test-dir build-audit -C Release --output-on-failure
```

All 12 tests passed in each of **10 complete CTest runs**, with 0 flaky failures. The first run took 31.29 seconds; the remaining nine took 25.77–26.25 seconds. The rebuilt `test_phase2d` verified 30 sample band payloads and decoded-pixel equality for one canonical frame at all three resolutions and two preset quantizer states. It does not establish coefficient-array equality or sequence behavior (`tests/test_phase2d.cpp:55-105`, `src/core/phase2d_encoder.cpp:490-805`). The comparison's `seed` argument is ignored (`src/core/phase2d_encoder.cpp:490-500`).

| Canonical case | FSratelimiter | Upstream = GPU sample bytes | 30 payloads equal | Metadata-masked samples equal | PSNR (dB) |
|---|---:|---:|---|---|---:|
| 1080p, frame 1 | 4 | 900,396 | yes | yes | 44.466 |
| 1080p, preset steady state | 0 | 1,181,024 | yes | yes | 44.582 |
| 4K, frame 1 | 4 | 3,563,684 | yes | yes | 44.554 |
| 4K, preset steady state | 0 | 4,668,864 | yes | yes | 44.676 |
| 8K, frame 1 | 4 | 14,009,028 | yes | yes | 44.582 |
| 8K, preset steady state | 0 | 18,393,596 | yes | yes | 44.703 |

The standalone upstream baseline was reproduced with:

```powershell
.\build-audit\benchmarks\Release\benchmark_codec.exe --json .\build-audit\reference-repro.json
```

It measured 85.86 FPS at 1080p (50 frames), 24.28 FPS at 4K (25 frames), and 6.10 FPS at 8K (10 frames). These are short, repeated-frame baseline measurements, not the requested sustained variable-complexity comparison (`benchmarks/benchmark_codec.cpp:105-165,330-375`).

## Sequence and frame-count failure of coverage

`RunPhase2D` constructs one `CanonicalPattern::Canonical` frame, initializes upstream with one or three encodes, reads `QuantTables2D` once, then supplies the same host pointer or one pre-uploaded device pointer to every submitted frame (`src/core/phase2d_encoder.cpp:1000-1155,1340-1398`). No code in that loop updates `lastgopbitcount`, `FSratelimiter`, or the quantizer table. The upstream baseline's normal `CFHD_EncodeSample` call can update that state, while the Phase 2D pipeline cannot. Therefore the requested 100-frame changing-complexity equivalence is **not yet implemented**; the first divergence for such a sequence cannot be measured through this API. The six passing preset-state single-frame checks do not resolve it.

The benchmark starts the clock on submission of the first measured frame. It waits for GPU entropy completion and D2H, assembles the sample, and joins the assembly worker before taking the end time (`src/core/phase2d_encoder.cpp:1225-1555`, `src/cuda/entropy_gpu.cu:2023-2117`). Thus it does more than submission-only timing. But it sets `res.encoded_frames = cfg.frames` without counting returned sample objects and retains only `res.last_bitstream` for a SHA-256 digest (`src/core/phase2d_encoder.cpp:1260-1275,1551-1555,1640-1660`). It never proves that distinct frame IDs survived slot reuse. The Phase 2D benchmark configures only 24, 18, and 12 measured frames (`benchmarks/benchmark_phase2d.cpp:160-164,369-372`). The test uses six pipeline frames in its 4K checks (`tests/test_phase2d.cpp:175-210`).

## Throughput reproduction and 8K anomaly

Run the freshly built benchmark from `build-audit` so it writes separate JSON:

```powershell
cd build-audit
.\benchmarks\Release\benchmark_phase2d.exe
```

The table gives the pre-existing saved result, the first fresh console run, and the second fresh raw-JSON run. Each FPS number is from one short repeated-frame run, not a five-run median.

| Mode | Resolution | Frames | Saved FPS | Fresh 1 FPS | Fresh 2 FPS |
|---|---:|---:|---:|---:|---:|
| Final device, preset steady state | 1080p | 24 | 804.72 | 809.00 | 778.52 |
| Final host, preset steady state | 1080p | 24 | 352.67 | 513.43 | 482.73 |
| Final device, preset steady state | 4K | 18 | 439.12 | 450.48 | 437.92 |
| Final host, preset steady state | 4K | 18 | 273.17 | 266.10 | 259.06 |
| Final device, preset steady state | 8K | 12 | 103.57 | 71.18 | 66.58 |
| Final host, preset steady state | 8K | 12 | 27.68 | 45.48 | 38.49 |

At 8K, `D2H_THROUGHPUT_AUTO_SLOTS` and `D2H_LOW_LATENCY_AUTO_SLOTS` both select **2 slots and `FrameConsolidated`**. `SelectPhase2DFrameSlots` resolves both policies to that same transfer mode above 4096×2160 (`src/core/phase2d_encoder.cpp:894-972`); the only configuration distinction is the policy enum before resolution. The saved JSON says 64.56 versus 91.21 FPS. Fresh run 1 says 89.93 versus 61.78; fresh run 2 says 70.26 versus 59.72. The ranking reversed and cannot establish a policy speedup. Thermal, clock, order, and scheduler effects were not independently isolated.

The host-input path copies the canonical BGRA vector into `cudaHostAlloc` pinned memory **before** timing, then includes H2D in the timed loop (`src/core/phase2d_encoder.cpp:1077-1090,1141-1152,1355-1366`). The upstream loop receives the ordinary BGRA vector and includes its CPU conversion and encode (`benchmarks/benchmark_phase2d.cpp:105-155`). Both omit initialization. Their sample bytes are equal only for the fixed preset state; the pinned-memory preparation cost and differing measured frame counts make the headline host comparison unfair as an end-to-end host-source comparison. The device path uploads the one BGRA frame before timing (`src/core/phase2d_encoder.cpp:1105-1112`) and must remain separately labeled.

The fresh second run's 4K device final mode reports 437.92 FPS, or 2.284 ms/frame, with color 0.189 ms, wavelet 0.551 ms, GPU entropy 2.045 ms, compressed D2H 0.359 ms, and CPU assembly 0.691 ms. Pipeline overlap can make a period shorter than the sum of these stage averages. The provided `gpu_utilization_pct` is computed from summed stage durations divided by wall time, not sampled hardware utilization (`src/core/phase2d_encoder.cpp:1639-1640`). GPU clocks, power, and an independent timeline were not captured, so physical timeline consistency remains unverified.

## Synchronization and CUDA Graph experiment

For the final sync experiment, `cudaDeviceSynchronize()` was inserted immediately before `timed_end_ms`, with a CUDA error check, then the exact original source bytes were restored and the unsynchronized binary rebuilt. The synchronized final device FPS was 827.35 / 380.40 / 89.70 at 1080p / 4K / 8K; the first normal run was 809.00 / 450.48 / 71.18 and the second normal run 778.52 / 437.92 / 66.58. The change has no consistent direction. These short runs cannot estimate the synchronization cost, but do not show a clear missing-work collapse.

The same-kernel graph comparison keeps 434 kernels per frame in both variants, with host calls falling from 434 to 1 (`src/core/phase2d_encoder.cpp:1774-1858`). In the fresh raw JSON, 4K host enqueue fell from 2.854 to 0.078 ms and the event-bracketed GPU entropy span fell from 17.709 to 2.724 ms; 1080p spans were 3.842 to 0.763 ms, and 8K spans 20.607 to 10.879 ms. Host dispatch gaps and scheduling are plausible explanations, but event spans are not an independent kernel timeline. Nsight Systems was not installed; the benchmark uses eight timed iterations and a fixed input. No claim of a quantified physical GPU-kernel speedup is justified yet.

## Sanitizers, limits, and production gate

CUDA memcheck command:

```powershell
& 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin\compute-sanitizer.bat' --tool memcheck .\build-audit\tests\Release\test_phase2d.exe
```

It completed with `ERROR SUMMARY: 0 errors`. The same `racecheck` command remained active for more than six minutes and was interrupted without a result. A CPU-only MSVC `/fsanitize=address` Release build could not build the full default target because `test_cuda_pyramid.cpp` unconditionally includes `cuda_runtime.h` even with CUDA disabled (`tests/CMakeLists.txt:44-46`, `tests/test_cuda_pyramid.cpp:8`). Three already-built CPU tests (`test_transform_wavelet`, `test_codec_roundtrip`, `test_yuy2_stability`) passed under ASan when run outside the sandbox with the Visual Studio ASan runtime on `PATH`:

```powershell
ctest --test-dir build-audit-asan -C Release --output-on-failure -R 'test_(transform_wavelet|codec_roundtrip|yuy2_stability)'
```

No 300-distinct-frame run at any resolution, 100-frame varying-complexity state comparison, five-run sustained performance series, 10,000/2,000/500-frame stability run, adversarial coefficient boundary corpus, or Nsight timeline was completed. The current `RunPhase2D` interface does not accept a sequence of input frames or expose every completed sample, so those checks require an audit-only harness or API instrumentation. No production code was altered to make a failed check pass.
