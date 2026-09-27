# Phase 3A.1 — Benchmark hardening

This pass did not try to make the encoder faster. It fixed measurement bugs that could report a wrong rate, then remeasured the Phase 3A encoder under one definition. The machine-readable record is [`benchmarks/phase3a1_canonical.json`](../benchmarks/phase3a1_canonical.json). Fresh-process companions are `benchmarks/phase3a1_fresh_forward.json`, `benchmarks/phase3a1_fresh_reverse.json`, `benchmarks/phase3a1_fresh_1080.json`, `benchmarks/phase3a1_fresh_4k.json`, and `benchmarks/phase3a1_fresh_8k.json`.

The tree was dirty when these files were written. Every JSON file is marked `"publication": "DIRTY"`. `upstream/` was not modified.

## Executive summary

The three published HostPageableEndToEnd tables were the same encoder path measured under different GPU clocks and different run lengths. They were not three different timing boundaries.

On the checked-in `benchmarks/results/phase3a_sequence.json`, the 60-frame 1080p HostPageable run that printed about 154 FPS held a flat 450 MHz graphics clock and an 810 MHz memory clock for the whole window (about 0.39 s, 13.8 W, `GpuIdle`). The 1,000-frame run that printed 481.6 FPS in that same file, and 462 FPS in the hand-written doc, held 1995 MHz and about an 8 GHz memory clock. H2D on those two records was 2.55 ms versus 0.63 ms. Mean bitstream size was 1.65 MB versus 1.66 MB, so the frames were not a different test. The doc's 169 FPS table was a third short run, copied by hand, and it does not match the JSON.

A fresh process on this machine, after 8K and 4K had already brought the GPU to 1995 MHz, measured 481.7 FPS for 1,000 frames of 1080p HostPageableEndToEnd. That reproduces the old 481 JSON figure. It is a hot single run. The five-trial median in a cold-to-warm session is 440.7 FPS, with trials from 400.1 to 457.4. The headline is the median, not 462 and not 481.

4K is not faster than 1080p when both are at boost. Median HostPageable FPS is 440.7 at 1080p and 227.2 at 4K. The old "4K beats 1080p" table compared an idle-clock 1080p run with a boosted 4K run.

8K HostPageable median is 38.2 FPS, which matches the old ~38–39 FPS. 8K DeviceInput median is 94.0 FPS. The old ~120–127 FPS device figure was not reproduced.

CUDA graph on versus off, both at 1995 MHz, is about +95% at 1080p, +38% at 4K, and +3% at 8K. The old +88% / +56% / +2% story had the right shape. The 4K gain at matched clocks is +38%, not +56%. Hashes match with the graph off and on.

Speedup versus pristine single-thread `CFHD_EncodeSample` is about 7.0× / 11.3× / 10.6× for HostPageable at 1080p / 4K / 8K. DeviceInput is higher and is not a file-transcode rate. A blanket "10–20× versus CineForm" does not describe the host-pageable path.

## What was wrong in the old benchmark

1. **One name, several clocks.** `HostPageableEndToEnd` in the sequence summary, the hand-written matrix, and the long run all called `RunPhase3A` with graph on, dynamic quant, and auto slots. The 60-frame window never left idle clocks. The long run started after the 8K variance study had already heated the GPU.
2. **Hand-copied tables.** `docs/PHASE3A_SEQUENCE_CORRECTNESS.md` did not match `phase3a_sequence.json`. Those tables are removed. This file is the only performance table.
3. **Verification lied when it was off.** `verify_sequence == false` still set `decode_ok`, `stamp_verify_ok`, and PSNR to 42 dB. Skipped verification is now `verification_performed: false` and does not count as a pass.
4. **The timer did not say what it was.** `sustained_fps` is completed frames over the post-warmup wall clock. It includes pageable staging, H2D, encode, D2H, and CPU assembly. It is not a submission rate. With 2 slots, `1000 / mean_latency` is not the FPS. The old 1080p long run had mean latency 3.12 ms (about 321 FPS if it were one frame at a time) and throughput 482 FPS. That ratio is what two overlapping slots look like, not a missing sync.
5. **No explicit drain.** The loop did wait for D2H and assembly before stopping, but it never called `cudaDeviceSynchronize`. The canonical timer now does. On the median 1080p trial that sync took 0.009 ms, so there was not a hidden queue of GPU work. The call is still inside the window.
6. **Overflow looked like "not ready."** `PollGpuEntropyFrameD2H` returned false both when the GPU was still running and when the packed word count exceeded the buffer. The second case spins forever in a poll loop. It now returns `Failed`, and the packer refuses to write past the cap.
7. **8K device and 1080p graph percentages were single-condition numbers.** They are replaced by the paired, clock-matched runs below.

`first_quarter_fps` in the old code is not the same statistic as `last_quarter_fps`. The first quarter is measured from the first measured submit to the quarter's last completion, so it includes one frame of latency. The last quarter is completion-to-completion. They are not used as headlines.

## Canonical timing: ProductionEndToEnd

`ProductionEndToEnd` is the contract, not a fourth input mode. Every headline FPS number uses it.

The wall clock starts when the first measured frame is accepted, after warmup. Warmup is at least the slot count, so the one-time CUDA graph capture (one capture per slot, on the first use of that slot) finishes before the clock starts. The clock stops after the last measured sample has been assembled on the host and `cudaDeviceSynchronize` has returned. A frame counts only when its compressed sample is available to the caller.

Included, when that input mode has them:

- pageable-to-pinned staging
- H2D
- GPU color conversion and wavelet, including quantization inside the wavelet kernels
- GPU entropy
- D2H of the compressed result and the lowpass
- packet assembly and metadata insertion
- the rate-control update that runs before the next wavelet launch
- the final device drain

Excluded, and reported separately when they were measured:

- encoder creation and CUDA context creation
- input-frame generation
- one-time graph capture (`graph_capture_max`, `warmup_excluded_ms`)
- filesystem output

`sequence_fps` is measured frames divided by that wall clock. `steady_state_fps` drops the first and last `slot_count` completions and is labeled separately. It is not the headline.

### Input modes

| Mode | What the caller already did | What the timer still includes |
| :--- | :--- | :--- |
| **HostPageableEndToEnd** | Nothing. Ordinary pageable BGRA. | Staging, H2D, encode, result on the host. This is the desktop number. |
| **HostPinnedApi** | Pinned the buffer. That allocation is outside the timer. | H2D, encode, result on the host. |
| **DeviceInput** | Placed BGRA in device memory. | Encode and the compressed result. This is an integration rate for a caller that already has the frame on the GPU. It is not the file-transcode rate. It is never the only number in the table. |

Settings on every row: BGRA8 source, `CFHD_ENCODED_FORMAT_YUV_422`, FilmScan 3, GOP 1, progressive, `Phase3AQuantMode::DynamicUpstreamState`, CUDA graph on for the headline, auto slots unless the slot study says otherwise. Baseline 1 is pristine `CFHD_EncodeSample` with `_THREADED_ENCODER=0`. That build is single-threaded. `CFHDCodecThreaded` is `EXCLUDE_FROM_ALL` and does not compile, which `docs/UPSTREAM.md` already records. Baseline 2, the CPU reference inside `VerifyPhase3ASequence120`, is the equivalence oracle (upstream spatial wavelet plus CPU entropy). It is not a second throughput product, so no speedup uses it.

Frame counts: 1,000 at 1080p, 300 at 4K, 120 at 8K, five trials. The speed runs cycle a distinct pool (96, 36, and 12 frames) that is longer than the slot count. They are not one static frame, and they are not 1,000 unique 1080p images. Equivalence runs use one buffer per index, stamped with that index.

Standard deviation below is the root-mean-square deviation of the five trial FPS values from their mean (divide by n). A row is flagged when that deviation exceeds 5% of the mean.

## Machine

| Item | Value |
| :--- | :--- |
| CPU | AMD Ryzen 9 7940HS, 16 logical processors. The name string also reports Radeon 780M. |
| RAM | 33,524,736,000 bytes |
| OS | Windows 10.0 build 26200, AC power online |
| GPU | NVIDIA GeForce RTX 4090 Laptop GPU, 16,376 MiB, driver 610.62 |
| CUDA | toolkit 12.8.61, nvcc, `sm_89` |
| Compiler | MSVC 19.44.35228, Release, `/O2`. CMake does not pass `/arch:AVX2`. |
| Git | `3e168fca3e69d3475e84d572202132db47241657`, branch `phase3a-sequence-correctness`, dirty |
| Link snapshot | Taken at process start, while idle: PCIe gen 1 x8. The query's maximum is gen 4 x16. This snapshot is not the link during encode. Power limit came back `[N/A]`. |
| Discrete GPU count | 1 (`nvidia-smi -L`) |

`upstream/` status was empty. The dirty list is the Phase 3A.1 sources, this document, and the JSON.

## Headline matrix

FPS is `sequence_fps`. Latency is submit-to-assembled-sample. Speedup is median FPS divided by the median upstream FPS on that row's resolution. Mean clock is the mean of the five per-trial mean clocks. Several of those trials were not at full boost the whole time, which is why the mean clock can be below the 1995 MHz median trial.

| Resolution | Mode | Median FPS | Mean FPS | Stdev | Min–max | Mean latency ms | P95 latency ms | Raw input GB/s | Output MB/s | Speedup | Graph | Slots | Mean clock MHz | Mean power W | Max °C | Flag |
| :--- | :--- | ---: | ---: | ---: | :--- | ---: | ---: | ---: | ---: | ---: | :--- | ---: | ---: | ---: | ---: | :--- |
| 1080p | Upstream CPU | 63.18 | 63.06 | 0.42 | 62.42–63.61 | 15.86 | 34.08 | 0.524 | 111.0 | 1 | — | 1 thread | — | — | — | |
| 1080p | HostPageable | 440.73 | 436.52 | 19.76 | 400.08–457.44 | 3.62 | 4.99 | 3.656 | 774.4 | 6.98× | on | 2 | 1840 | 44.9 | 57 | |
| 1080p | HostPinned | 543.85 | 536.15 | 30.33 | 477.41–563.10 | 2.69 | 3.82 | 4.511 | 955.6 | 8.61× | on | 2 | 1917 | 46.3 | 57 | stdev 5.7% |
| 1080p | DeviceInput | 783.85 | 753.06 | 69.27 | 641.32–821.15 | 2.30 | 3.84 | 6.502 | 1377 | 12.41× | on | 2 | 1720 | 50.4 | 58 | stdev 9.2% |
| 4K | Upstream CPU | 20.17 | 19.13 | 2.16 | 16.51–21.28 | 52.96 | 119.2 | 0.669 | 134.0 | 1 | — | 1 thread | — | — | — | stdev 11% |
| 4K | HostPageable | 227.21 | 218.60 | 16.50 | 196.86–238.73 | 12.58 | 17.57 | 7.538 | 1526 | 11.26× | on | 3 | 1701 | 53.1 | 61 | stdev 7.5% |
| 4K | HostPinned | 288.27 | 288.42 | 7.01 | 276.62–296.66 | 9.28 | 14.73 | 9.564 | 1936 | 14.29× | on | 3 | 1847 | 54.7 | 61 | |
| 4K | DeviceInput | 395.56 | 367.99 | 49.09 | 280.54–409.30 | 7.13 | 12.50 | 13.12 | 2657 | 19.61× | on | 3 | 1695 | 59.2 | 62 | stdev 13% |
| 8K | Upstream CPU | 3.597 | 3.560 | 0.080 | 3.401–3.608 | 281.1 | 553.0 | 0.477 | 122.3 | 1 | — | 1 thread | — | — | — | |
| 8K | HostPageable | 38.16 | 37.83 | 0.63 | 36.83–38.46 | 38.11 | 52.19 | 5.064 | 1297 | 10.61× | on | 2 | 1920 | 55.7 | 60 | |
| 8K | HostPinned | 45.74 | 45.70 | 0.11 | 45.51–45.82 | 29.88 | 45.53 | 6.070 | 1555 | 12.71× | on | 2 | 1952 | 64.7 | 61 | |
| 8K | DeviceInput | 93.96 | 93.99 | 1.63 | 91.55–96.45 | 17.32 | 33.74 | 12.47 | 3194 | 26.12× | on | 2 | 1789 | 70.9 | 64 | |

Mean latency and P95 latency in this table are the means of the five trials' own mean and P95. They are not one pooled histogram. Throttle strings seen on GPU trials include `None`, `GpuIdle`, `SwPowerCap`, and `SwThermalSlowdown`, often together. They were not used to drop trials.

Raw frame sizes, exactly: 1080p `1920*1080*4 = 8,294,400` bytes, 4K `33,177,600` bytes, 8K `132,710,400` bytes. At 120 FPS an 8K frame would be `132,710,400 * 120 / 1e9 = 15.925` GB/s of raw BGRA before any extra GPU traffic. The measured 8K HostPageable rate is 5.064 GB/s. The measured 8K DeviceInput rate is 12.47 GB/s of pixels that were already on the device.

Mean compressed size was identical across the three GPU modes in a resolution (1,757,177 / 6,717,335 / 33,992,290 bytes) because they saw the same cycle. Upstream means were 1,757,174 / 6,640,048 / 33,992,443. The small gap is live metadata on the speed path. With deterministic metadata, the equivalence runs match per frame. See below.

On the median-FPS 1080p HostPageable trial, mean latency was 3.69 ms and the frame period was 2.27 ms. `1000/3.69 = 271` FPS, and the sequence rate was 441 FPS. Two slots are in flight. The same pattern holds at 4K (12.0 ms latency, 4.40 ms period, 3 slots) and 8K (37.7 ms latency, 26.2 ms period, 2 slots).

### Stage times on the median-FPS trial

These are mean durations of each stage. They overlap. Their sum is not the frame latency and not the frame period. The period column is the wall-clock frame period.

| Stage | 1080p HostPageable | 4K HostPageable | 8K HostPageable | 8K DeviceInput |
| :--- | ---: | ---: | ---: | ---: |
| Sequence FPS | 440.7 | 227.2 | 38.16 | 94.0 |
| Frame period ms | 2.27 | 4.40 | 26.21 | 10.64 |
| Pageable staging ms | 0.50 | 1.73 | 6.54 | 0 |
| H2D ms | 0.65 | 2.58 | 9.95 | 0 |
| Color ms | 0.072 | 0.25 | 0.58 | 0.51 |
| Wavelet, including quant ms | 0.30 | 0.46 | 1.12 | 1.13 |
| Entropy ms | 0.53 | 1.58 | 7.59 | 7.66 |
| Compressed D2H ms | 0.14 | 0.49 | 2.50 | 2.51 |
| Host assembly ms | 0.32 | 1.26 | 4.03 | 3.90 |
| Rate-control update ms | 0.011 | 0.018 | 0.015 | 0.015 |
| Sum of GPU stage averages ms | 1.70 | 5.36 | 21.74 | 11.82 |
| H2D GB/s | 12.7 | 12.8 | 13.3 | 0 |
| Final `cudaDeviceSynchronize` ms | 0.009 | 0.014 | 0.010 | (same drain) |

The two timing methods agree. Method A is `std::chrono` around the sequence, which is `sequence_fps`. Method B is the mean completion-to-completion period from per-frame host timestamps, cross-checked against CUDA-event stage durations. On these trials `mean_completion_period_ms` matches `frame_period_ms` (8K DeviceInput 10.6422 versus 10.6426). The stage sum is larger than the period at 4K because the stages overlap. That is expected. The stage sum is not used as FPS.

Wall windows for those median trials were 2.27 s (1080p, 1,000 frames), 1.32 s (4K, 300 frames), and 3.14 s (8K, 120 frames). 4K's 300-frame count is what the brief asked for; at 227 FPS it is only 1.3 s, so startup still has some weight. `steady_state_fps` on that trial was 229, close to 227. 1080p steady-state on the median trial was 441 versus 441 sequence. The short 4K window is a limitation, not a hidden exclusion of work.

## Why 4K looked faster than 1080p

It was a cold 1080p measurement next to a warm 4K measurement.

Controlled HostPageable, graph on, FilmScan 3, same pattern family, clocks recorded:

| Slots | 1080p median FPS | 1080p latency ms | 4K median FPS | 4K latency ms | 8K median FPS | 8K latency ms |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 387.5 | 2.57 | 127.2 | 7.83 | 29.89 | 33.40 |
| 2 | 448.2 | 3.57 | 156.4 | 9.65 | 37.71 | 38.66 |
| 3 | 590.8 | 4.61 | 223.3 | 12.23 | 58.19 | 46.57 |
| 4 | 657.6 | 5.50 | 239.2 | 16.18 | 63.98 | 59.66 |

At every slot count, 1080p is faster than 4K, and 4K is faster than 8K. The inversion does not survive once the clocks match. More slots raise throughput and latency together. The auto policy's 2 slots at 1080p are not the peak. That is recorded as a Phase 3B option, not changed here.

The 1080p slot trials above sat at 1995 MHz. Their 2-slot median (448) matches the hot end of the headline set, not the 154 FPS idle run.

## CUDA graph

Graph off and graph on used the same frames, the same slot count, and the same streams. Trials alternated off then on so the on-run was not always the warmer one. Every one of these trials reported a flat 1995 MHz graphics clock.

| Resolution | Frames | Off median FPS | On median FPS | Median delta | Off enqueue ms | On enqueue ms | Off entropy ms | On entropy ms | Captures |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1080p | 400 | 237.3 | 463.6 | +95.4% | 3.08 | 0.161 | 2.33 | 0.525 | 2 |
| 4K | 120 | 159.1 | 220.0 | +38.3% | 2.72 | 0.212 | 3.33 | 1.59 | 3 |
| 8K | 48 | 36.72 | 37.91 | +3.2% | 1.56 | 0.141 | 8.35 | 7.38 | 2 |

Capture count equals the slot count. Capture time is not inside `sequence_fps`. A separate identity run with deterministic metadata hashed every frame:

| Resolution | Frames hashed | Graph off FNV | Graph on FNV | First-frame SHA-256 match |
| :--- | ---: | :--- | :--- | :--- |
| 1080p | 16 | `439b8a18a4933e05` | same | yes |
| 4K | 16 | `8cf55264b3b5b7dd` | same | yes |
| 8K | 8 | `45986d1b06805dd6` | same | yes |

The old +88% at 1080p was the right direction and a bit lower than the matched-clock +95%. The old +56% at 4K is not what matched clocks produce. The old +2% at 8K matches the +3% here. Byte identity holds.

## Quantization and the CUDA graph

One description, from the code as it runs:

1. `FSratelimiter` is computed on the CPU. `PrepareFrameState` calls upstream `SetEncoderQuantization` and `SetTransformQuantization`. The previous frame's sample size is committed by `CompleteFrameState` as `lastgopbitcount = sample_bytes * 8`, which is what `QuantizationSetQuality` reads on the next frame.
2. The 36 quant values and the 36 scale values are generated on the CPU before the wavelet launch.
3. `HyperCine_GpuSlotKickWaveletWithQuant` copies those 36 ints into `slot->run_quant` and launches the wavelet kernels with them. That launch is not inside a CUDA graph.
4. The wavelet kernels are what consume `quant[]`. They write quantized `int16` subband buffers.
5. The entropy graph reads those buffers. Its cache key is the session, band count, stream count, D2H mode, kernel mode, LPT flag, and each band's device pointer, width, height, and pitch. Quant tables are not in the key.
6. Codebooks are uploaded before capture. They are fixed for the subband. A new codebook upload while graphs are enabled synchronizes that stream first.
7. Dynamic parameters across frames are the quant table (host-side, applied at the wavelet launch) and the coefficient values in the fixed device buffers. Pointers inside the graph stay fixed.
8. An `FSratelimiter` change does not recapture the graph. Recapture happens when the key changes: resolution, pointers, stream count, or kernel mode.

`docs/CUDA_PHASE2D.md` used to say a rate-control change would require `cudaGraphExecKernelNodeSetParams` on a captured wavelet kernel. That sentence described a design that was not built. The wavelet launch is outside the graph. The sentence now points here.

## Entropy output bound

The GPU coefficient buffer stores packed VLC words only. Band headers and trailers are built later on the host, in a different allocation (`BuildHighpassPayloadPtr` adds 256 bytes on top of the coefficient bytes and fails the call if they do not fit). They are not in this bound.

For one highpass band of `N = height * pitch_px` virtual coefficients, including row-gap zeros:

- A zero run emits one token per step, and every selectable run code in the loaded tables covers at least one zero (`ValidateEntropyTablesForWordBound` rejects a non-positive step). Zero tokens are therefore at most the number of zeros.
- Each nonzero emits one value token. Value magnitudes outside the codebook are clamped into it, which is what `upstream/Codec/encoder.c` does with `VALUE_TABLE_LENGTH`. There is no extra escape word beyond that table entry.
- Value codeword length lives in the top 5 bits of the `VLE` entry (`VLE_CODESIZE_SHIFT` is 27), so it is at most 31. Run codeword lengths and the band-end length are checked to be in 1..31. The validator accepted all 17 subband codebooks loaded from a FilmScan 3 encoder.
- Tokens are therefore at most `N`, plus one band-end codeword.
- Bits are at most `31 * (N + 1)`. Packed words are `ceil(bits / 32)`.

`ceil(31*(N+1)/32) <= N+1` for every `N >= 1`. The allocator's per-band cap is `N + 4096`, which is above that bound. The frame arena is sized for every band at that cap (`2*W*H` plus a per-band margin). The test `test_entropy_capacity` encodes all-zero, all-max, and alternating bands, including a row gap, and checks the word count against the bound.

If a kernel would need more words than the cap, `PackKernel` and the consolidate kernel set an overflow flag and do not write past the buffer. `PollGpuEntropyFrameD2HStatus` then returns `Failed` with the text `entropy output exceeded the proven word capacity`. The capacity test forces a 1-word cap and checks that the poll stays `Failed` instead of looking pending. A bad run code that does not consume zeros also breaks out of the emit loop instead of spinning inside the kernel.

## Asynchronous errors

`GpuStageStatus` is `Pending`, `Complete`, or `Failed`.

- `PollGpuEntropyFrameD2HStatus` returns `Pending` only for `cudaErrorNotReady`. A hard CUDA error or an overflow is `Failed` and sticky.
- `IsGpuEntropyFrameComplete` and `IsGpuEntropyComputeDone` do not treat a hard error as "still running."
- `PollNextStreamedBandReady` returns `-2` on failure. The Phase 3A loop stops the encode.
- `HyperCine_GpuSlotGpuIdle` already returned `-1` for a dead run. The lowpass D2H query that used to turn every non-success into "not idle" now returns `-1` for errors other than not-ready.
- The encode loop has a deadline (`180 s + 250 ms per frame`). Past it, the run fails with `encode watchdog` instead of spinning. `test_phase3a` still passes, including the 120-frame equivalence and the input-mode hash check.

`Complete` on the consolidated poll means the D2H has been queued. `IsGpuEntropyFrameComplete` is what waits for the copy to finish. Callers that only looked at the bool now also have `GpuEntropyFrameError`.

## Fresh processes and order

HostPageableEndToEnd, one trial, new process. FPS:

| Order | 1080p | 4K | 8K |
| :--- | ---: | ---: | ---: |
| 1080, then 4K, then 8K | 456.6 | 236.6 | 38.39 |
| 8K, then 4K, then 1080 | 481.7 | 234.8 | 38.08 |
| Fresh process, that resolution only | 464.5 | 239.9 | 38.03 |

8K host-pageable does not care about order (38.0–38.4). 4K moves by about 2%. 1080p moves by about 25 FPS depending on whether the GPU was already at 1995 MHz. The 481.7 FPS reverse-order run is the old long-run number, reproduced, and it is the hot end rather than the median. Later tests in one process get a warmer GPU. They do not get a different algorithm. DeviceInput in those same processes stayed in the 91–96 FPS band at 8K and did not reach 120.

## Output equivalence

Deterministic metadata, one buffer per frame, graph on:

| Resolution | Frames | Pageable FNV | Pinned FNV | Device FNV |
| :--- | ---: | :--- | :--- | :--- |
| 1080p | 24 | `98783ee86d55ea66` | same | same |
| 4K | 24 | `20034c3fd788e67c` | same | same |
| 8K | 12 | `42d81ab4454b93cc` | same | same |

First, middle, and last SHA-256 matched across the three modes in each resolution. Stamps decoded. Those equivalence runs are correctness runs. Their FPS is low because hashing and decode run on the completion path. Do not quote them as throughput. The headline runs turn that off.

## Claims

**Still true**

- 8K HostPageable is about 38–39 FPS on this class of laptop link. The new median is 38.16.
- CUDA graphs do not change the bitstream. Hashes match.
- Graph gain at 8K is about 2–3%.
- Graph gain at 1080p is large. Matched clocks measured +95%, against an older +88% from a less controlled pair.
- Host pageable, host pinned, and device input are different contracts. Pinned is faster than pageable. Device is faster than pinned. At 8K the pageable-to-device gap is 38 versus 94 FPS.
- Multi-slot latency is longer than the frame period. Old latency columns were real submit-to-complete times. They were not `1000/FPS`.
- The upstream encoder used for comparison is the single-thread build.

**Corrected**

- 462 FPS and 481 FPS at 1080p HostPageable are hot single runs of the same mode. 481.7 FPS was reproduced after 8K and 4K had warmed the GPU. The five-trial median is 440.7 FPS. Publish the median.
- 154 FPS and 169 FPS were short runs at idle or low clocks (the JSON's 154 FPS run never left 450 MHz / 810 MHz memory). They are not a second algorithm.
- 4K faster than 1080p was that cold 1080p run beside a warm 4K run. At matched clocks, 1080p is faster at 1, 2, 3, and 4 slots.
- 4K graph gain of +56% becomes +38% when off and on both sit at 1995 MHz.
- 8K DeviceInput of ~120–127 FPS was not reproduced. The five-trial median is 94.0 FPS, including trials whose mean clock was near 1900 MHz. Pageable 8K did reproduce, so the gap is not "the machine cannot reach the old host number."
- "10–20× versus CineForm" is not one number. HostPageable is 7.0×, 11.3×, and 10.6×. DeviceInput at 8K is 26× against the same single-thread CPU, and it skips the upload the CPU still does.
- DeviceInput is not the NLE file-transcode rate. It is the rate once a BGRA frame is already in device memory.
- `decode_ok: true` with PSNR 42 on runs that never decoded was a stub. It is gone.

## Limitations

- The result is marked DIRTY. Commit the tree before treating a future run as publishable without `--allow-dirty`.
- Speed sequences cycle 96 / 36 / 12 distinct frames. That is enough to keep a slot from re-reading the pointer it still owns. It is not a thousand unique 1080p images.
- The 4K headline window is 1.3 s. The brief's 300-frame minimum is met. Steady-state and sequence FPS differ by about 1% on the median trial.
- Laptop clocks move. Rows with stdev above 5% are flagged and were kept.
- The PCIe generation was sampled at process start, while the link was idle. It is not a measurement of the link during the copy.
- `nvidia-smi` reported no power limit (`[N/A]`).
- Baseline 2 was not given its own FPS. It is the equivalence path, not a shipping threaded CPU encoder.
- Equivalence and graph-identity FPS include hash and decode work. They are identified by suite name so they are not mixed into the headline.

## What this phase changed in code

- `RunPhase3A` names the timing contract, records steady-state FPS, drain, warmup exclusion, p99, first-frame latency, and rate-control time, drains with `cudaDeviceSynchronize`, and fails a watchdog instead of spinning.
- Skipped verification no longer reports a fake PSNR.
- Entropy capacity is checked against the bound above. Overflow is `Failed`.
- `test_entropy_capacity` covers the bound and the overflow poll.
- `benchmark_phase3a1` writes the JSON this document uses.

No wavelet or entropy algorithm was rewritten to go faster. Slot policy was not changed. Opportunities that showed up in the numbers are in `docs/PHASE3B_PERFORMANCE_OPPORTUNITIES.md`.
