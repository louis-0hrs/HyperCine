# Phase 3B performance opportunities

> [!NOTE]
> **Historical optimization study.** These observations were collected during Phase 3A.1; suggestions are not implementation or release commitments. Use the [Phase 3A.2c dataset](../benchmarks/phase3a2c_release_candidate.json) for current README headline throughput and the [benchmark guide](../benchmarks/results/README.md) for canonical-versus-archive distinctions.

These are observations from the Phase 3A.1 measurement pass. None of them were implemented. The Phase 3A.1 numbers in `docs/PHASE3A1_BENCHMARK_HARDENING.md` are the baseline.

## 1. The default slot count is not the measured throughput peak

`SelectPhase2DFrameSlots` picks 2 slots at 1080p, 3 at 4K, and 2 at 8K. On the RTX 4090 Laptop, HostPageableEndToEnd with CUDA graphs and the same content did this:

| Resolution | 1 slot | 2 slots | 3 slots | 4 slots |
| :--- | ---: | ---: | ---: | ---: |
| 1080p median FPS | 388 | 448 | 591 | 658 |
| 4K median FPS | 127 | 156 | 223 | 239 |
| 8K median FPS | 29.9 | 37.7 | 58.2 | 64.0 |

Latency rises with slot count. A throughput policy and a latency policy are different products. Changing the default would move the baseline, so it stays as it is until a Phase 3B change says which product it is optimizing.

## 2. CUDA graph launch is a 1080p and 4K host-launch win

At matched 1995 MHz clocks, graph launch versus per-kernel launch on HostPageableEndToEnd changed median sequence FPS by about +95% at 1080p, +38% at 4K, and +3% at 8K. Host enqueue fell from roughly 3.1 ms to 0.16 ms at 1080p. At 8K the frame period is about 26 ms and the copy plus entropy dominate, so removing launch calls does not move the sequence rate. The graph already excludes quantizer tables. Further graph work should not recapture on `FSratelimiter` changes.

## 3. 8K host throughput is the copy, not the kernel

On the median 8K HostPageable trial the frame period was 26.2 ms. Pageable staging was 6.5 ms and H2D was 10.0 ms. DeviceInput, which skips both, had a 10.6 ms period. The H2D copy itself ran at about 13.3 GB/s. An 8K BGRA frame is exactly 132,710,400 bytes. At the measured 38.2 FPS that is 5.06 GB/s of raw input, well below the copy rate, because staging, entropy, and assembly do not fully hide behind that copy.

## 4. Entropy is still the long device-side stage at 8K

DeviceInput entropy was about 7.7 ms inside a 10.6 ms period. Color plus wavelet was about 1.6 ms. Another launch-overhead pass will not move 8K device throughput the way it moves 1080p.

## 5. Laptop boost makes single runs lie

Several headline trials spent part of the window below 1600 MHz and some reported `SwPowerCap` or `SwThermalSlowdown`. 1080p HostPageable in a cold-to-warm five-trial set had median 441 FPS. The same mode in a fresh process after 8K and 4K, already at 1995 MHz, measured 482 FPS. Phase 3B should keep the five-trial median and the clock trace. It should not tune against whichever single run was hottest.

## 6. Host assembly is visible at 8K

CPU sample assembly averaged about 4.0 ms on the 8K host path and about 3.9 ms on DeviceInput. It overlaps the next frame, which is why the frame period can be shorter than the sum of the stage averages. It is still a large part of submit-to-complete latency (about 38 ms host, about 17 ms device).
