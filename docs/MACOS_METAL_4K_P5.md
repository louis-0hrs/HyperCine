# macOS Metal 4K — P5 acceptance report (issue #45)

Measured on the real machine. Nothing below is extrapolated from 1080p.

## Test configuration

| item | value |
|---|---|
| Hardware | Apple M2 Pro, arm64 |
| OS | macOS 26.5.2 (build 25F84) |
| Source / provenance | `ab47a47` is the tested 4K encoder parent; the genuine 4K benchmark source is in PR #251 (initial correction head `4a684084`). The exact local measurement commit must be recorded separately before production performance qualification |
| Compiler | Apple clang 21.0.0 (clang-2100.1.1.101) |
| CMake | 3.31.5 |
| Build | Release, `-DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_ENABLE_METAL=ON -DHYPERCINE_BUILD_UPSTREAM=OFF` |
| FFmpeg | pinned local binary (not from PATH); version recorded by the gate at run time |
| Frame source | synthetic BGRA, deterministic generator inside `benchmark_codec` |

Reproduce:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_METAL=ON -DHYPERCINE_BUILD_TESTS=ON \
  -DHYPERCINE_BUILD_BENCHMARKS=ON -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON \
  -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build --parallel 6
ctest --test-dir build
./build/benchmark_codec > bench.json
```

## Test suite

```
ctest --test-dir build
100% tests passed, 0 tests failed out of 64
```

Zero skips. The two 4K gates individually:

| test | time | result |
|---|---|---|
| `filmscan3_metal_4k_encoder` | 8.44 s | PASS |
| `filmscan3_metal_4k_ffmpeg_gate` | 2.77 s | PASS |

`packet_equality` is reported by the benchmark itself: **PASS** — CPU and Metal
produce byte-identical packets for every frame in the run.

## Correction to the first version of this report

The first version of this document called scenario 2 the 4K case. It was not.
`benchmarks/benchmark_metal_p2.cpp` allocated `1920*1080*4` and passed `7680` as
the stride for every scenario, so **all** earlier timings were 1080p. The label
was wrong, not the measurement.

Fixed by adding real 4K scenarios and recording the geometry in every JSON
record, so a number can no longer be read as 4K without its width and height
next to it. The stale 1080p figures quoted earlier in this file are withdrawn.

Scenario 2 had also been labelled "4K" because its sample was large; it is a
1080p frame with a high-detail pattern, which is why it is the slowest 1080p
case.

## Timing (steady state: first 4 frames of each scenario discarded)

Real M2 Pro, arm64, Release. Geometry is read from the JSON, not assumed.

| scenario | geometry | frames | CPU median | CPU p95 | CPU worst | Metal median | Metal p95 | Metal worst |
|---|---|---|---|---|---|---|---|---|
| 0 | 1920x1080 | 28 | 27.21 ms | 27.28 | 28.73 | 19.76 ms | 20.00 | 21.17 |
| 1 | 1920x1080 | 28 | 29.04 ms | 29.07 | 29.12 | 21.60 ms | 21.80 | 21.87 |
| 2 | 1920x1080 | 28 | 45.91 ms | 45.95 | 45.97 | 38.68 ms | 38.70 | 38.73 |
| **5** | **3840x2160** | 28 | **108.70 ms** | 108.82 | 109.25 | **76.13 ms** | 76.28 | 76.40 |
| **6** | **3840x2160** | 28 | **113.16 ms** | 113.27 | 113.35 | **79.48 ms** | 80.02 | 80.04 |
| **7** | **3840x2160** | 28 | **184.43 ms** | 184.57 | 184.65 | **151.86 ms** | 152.01 | 152.39 |

`packet_equality: PASS` for every frame, at both geometries.

At real 4K, Metal is **32.6 ms faster per frame** than the CPU path (scenario 5,
median) — about 30 % of CPU time, against roughly 27 % at 1080p. The advantage
does not grow with resolution the way a GPU-bound pipeline would.

Scenario 8 (16 frames, mixed patterns) is not shown as a row: it mixes patterns
within one geometry, so its p95 reflects pattern changes, not a slowdown.

## Stage timings — real 4K scenario, **two different encoders**

The benchmark executes **two independent encodes** on the same input. `CpuMeter` instruments the standalone scalar CPU reference; `MetalTiming` records the Metal frontend plus shared host quantization/entropy work. **Do not add CPU reference conversion/wavelet to the Metal encode time.** GPU batch execution is included within the command-buffer wait rather than being an extra serial stage.

| Reported measurement | Time | Actually belongs to |
| --- | ---: | --- |
| Input upload | 0.67 ms | Metal transfer |
| Submit | 0.07 ms | Metal submit |
| GPU wait | 4.23 ms | Metal command-buffer wait (includes device GPU execution) |
| GPU batch execution | 3.51 ms | Subset of the wait, **not additive** |
| Band readback | 0.89 ms | Metal transfer |
| Quantization | 19.47 ms | Shared CPU writer processing Metal frontend output |
| Coefficient hashing | 42.68 ms | Shared CPU writer processing Metal frontend output |
| Entropy | 7.04 ms | Shared CPU writer processing Metal frontend output |
| Framing | 0.73 ms | Shared CPU writer, derived timing |
| CPU reference conversion | 19.66 ms | **Separate scalar CPU encoder, not Metal** |
| CPU reference wavelet | 19.74 ms | **Separate scalar CPU encoder, not Metal** |

These are owner-reported values for the actual 3840×2160 run; stage timings are not a lossless additive decomposition of the measured 76.13 ms full Metal median. `color_gpu_ms` and `wavelet_gpu_ms` remain zero in this reporting mode while `batch_gpu_ms` measures device time. Zero per-stage GPU counters do not mean the GPU was idle.

For this particular 4K pattern, hashing and quantization together are far more expensive than the GPU batch. Entropy runtime varies strongly by content and must not be generalized from one synthetic sample.

## Memory

| item | 1080p | real 4K |
|---|---|---|
| process peak RSS | 100.6 MiB | 345.2 MiB |
| Metal peak buffer | 30.2 MiB | 120.6 MiB |
| device allocated | 30.7 MiB | 121.3 MiB |
| buffer allocations, cold frame | 42 | 42 |
| buffer allocations, steady state | 0 | 0 |
| readbacks per frame | 30 | 30 |

Peak RSS is a **process-wide maximum across sequential benchmark scenarios**, not a directly measured per-session allocation and not evidence that both geometry workspaces were simultaneously resident. The per-geometry Metal buffer counters are more useful for comparing session footprint. The reported warm-path allocation counter is zero at both geometries.

## External decoder

The hosted `filmscan3_metal_4k_ffmpeg_gate` invokes the **CPU** `hypercine_encode_filmscan3` tool and verifies a 3840×2160 `yuv422p10le` FFmpeg decode and geometry rejection. It is *not*, by itself, a GPU-required Metal decode gate. Independent native M2 Pro CPU/Metal full-packet equality and FFmpeg decodes were reported separately for merged #248. Preserve that distinction.

## Known limits

- **Not bit-exact against GoPro.** No original-SDK 4K reference exists on this
  host, so no `BITEXACT_PASS` is claimed. What is proven is CPU/Metal equality
  and independent decodability, not conformance with GoPro's encoder.
- The 4K sample is synthetic. Real sensor footage with different noise
  characteristics could produce larger packets and different entropy timings.
- Per-stage GPU timing is unavailable in this build (see above).
- Only one Metal device and one encoder instance were used for these numbers.
  Two-session behaviour is M4 scope.
- 8K and above are out of scope and refused by geometry validation; see
  `docs/CAPABILITY_8K_17K_AUDIT.md`.

## Verdict

The 4K Metal path was reported CPU-packet-identical and independently decodable on a real M2 Pro. For the measured 4K benchmark patterns, the host-side **coefficient hashing and quantization** account for much of the runtime; these are separate from GPU batch timing and from the scalar reference encoder. These numbers do not establish a production performance guarantee or original GoPro 4K packet parity.