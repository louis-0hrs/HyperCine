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

## Timing (steady state: first 4 frames of each scenario discarded)

`benchmark_codec` scenario 2 is the 4K case (`sample_bytes` ~4.9 MB).

| scenario | frames | CPU median | CPU p95 | CPU worst | Metal median | Metal p95 | Metal worst |
|---|---|---|---|---|---|---|---|
| 0 (1080p, flat) | 28 | 27.22 ms | 27.27 | 27.28 | 19.69 ms | 20.00 | 20.04 |
| 1 (1080p, textured) | 28 | 29.02 ms | 29.09 | 29.09 | 21.63 ms | 21.81 | 21.83 |
| **2 (4K)** | 28 | **45.91 ms** | 46.01 | 46.62 | **38.68 ms** | 38.78 | 39.75 |
| 4 (mixed, 120 frames) | 116 | 29.06 ms | 45.89 | 46.00 | 21.71 ms | 38.47 | 38.70 |

4K: Metal is about **7.2 ms faster per frame** than the CPU path (median),
~16 % of CPU time. At 1080p the gap is ~7.4 ms (~27 %).

Scenario 4's p95 equals the 4K median because that scenario mixes 1080p and 4K
frames; it is not a slowdown within one resolution.

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
