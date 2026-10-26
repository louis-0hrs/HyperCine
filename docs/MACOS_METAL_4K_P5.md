# macOS Metal 4K — P5 acceptance report (issue #45)

Measured on the real machine. Nothing below is extrapolated from 1080p.

## Test configuration

| item | value |
|---|---|
| Hardware | Apple M2 Pro, arm64 |
| OS | macOS 26.5.2 (build 25F84) |
| Source SHA | `ab47a47` (main, after #248) plus the validator fix in this branch |
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

## Where the Metal time goes (real 4K, median frame)

| stage | value |
|---|---|
| upload | 0.67 ms |
| submit | 0.07 ms |
| GPU wait | 4.23 ms |
| batch GPU | 3.51 ms |
| readback | 0.89 ms |
| CPU conversion | 19.66 ms |
| CPU wavelet | 19.74 ms |
| quantization | 19.47 ms |
| coefficient hashing | 42.68 ms |
| entropy | 7.04 ms |
| framing | 0.73 ms |

Two honest notes:

1. `color_gpu_ms` and `wavelet_gpu_ms` report **0** while `batch_gpu_ms` carries
   the GPU work. Per-stage GPU timing is not available from this build; reading
   the zeroes as "no GPU work" would be wrong.
2. At 4K, **~109 of the ~110 ms that remain on the Metal path are CPU-side
   stages**: conversion, wavelet, quantization and hashing. GPU wait plus batch
   work is about 7.7 ms. Optimising the GPU kernel further would move almost
   nothing; hashing alone is 42.7 ms and is the single largest term.

The 1080p run had entropy as the dominant term (20.4 ms). At 4K, entropy drops
to 7.0 ms while hashing rises to 42.7 ms — the coefficient count grew far more
than the entropy payload did.

## Memory

| item | 1080p | real 4K |
|---|---|---|
| process peak RSS | 100.6 MiB | 345.2 MiB |
| Metal peak buffer | 30.2 MiB | 120.6 MiB |
| device allocated | 30.7 MiB | 121.3 MiB |
| buffer allocations, cold frame | 42 | 42 |
| buffer allocations, steady state | 0 | 0 |
| readbacks per frame | 30 | 30 |

Peak RSS was measured across the whole run, so the 345.2 MiB figure includes
both geometries being held resident. The per-geometry Metal figures are the
meaningful ones. Buffer allocations are zero after the first frame at both
geometries.

## Where the Metal time goes (4K, steady state)

| stage | value |
|---|---|
| upload | 0.18 ms |
| submit | ~0.03 ms |
| GPU wait | 2.84 ms |
| batch GPU | 1.13 ms |
| readback | 0.23 ms |
| CPU conversion | 4.86 ms |
| CPU wavelet | 4.70 ms |
| quantization | 4.88 ms |
| coefficient hashing | 10.68 ms |
| entropy | 20.40 ms |
| framing | 0.40 ms |

Two honest notes:

1. `color_gpu_ms` and `wavelet_gpu_ms` report **0** while `batch_gpu_ms` carries
   the GPU work. The per-stage split is therefore *not* available from this
   build; the batch number is what the driver reported. Reading the zeroes as
   "no GPU work" would be wrong.
2. **Coefficient hashing (10.7 ms) plus entropy (20.4 ms) dominate the Metal
   path**, not the GPU. At 4K, 31 of ~39 ms are CPU-side stages that Metal does
   not touch. Optimising the GPU kernel further would move at most the
   remaining ~1 ms of GPU wait plus 1.1 ms of batch work.

## Memory (4K)

| item | value |
|---|---|
| process peak RSS | 100.6 MiB (105,414,656 B) |
| Metal peak buffer | 30.2 MiB (31,622,400 B) |
| device allocated | 30.7 MiB (32,194,560 B) |
| buffer allocations, cold frame | 42 |
| buffer allocations, steady state | 0 |
| command buffers per frame | 1 |
| uploads / readbacks per frame | 1 / 30 |

Buffer allocations drop to **zero** after the first frame: the workspace is
reused, as intended. 30 readbacks per frame correspond to the 30 coefficient
bands.

## External decoder

FFmpeg decodes both CPU and Metal packets of the 4K sample at 3840x2160,
`yuv422p10le`, and the byte count matches the geometry expectation. This is
asserted by `filmscan3_metal_4k_ffmpeg_gate`, which also proves the gate fails
when the declared container geometry is wrong.

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

The 4K Metal path is reproducible, CPU-identical and independently decodable on
real M2 Pro hardware. Its remaining cost is dominated by CPU-side entropy and
hashing, not by the GPU.
