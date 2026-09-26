# HyperCine Phase 1C: Measurement Reconciliation and Hybrid Encoder

Phase 1C does not implement GPU entropy or GPU VLC. It measures one canonical encoder configuration, sizes the coefficient traffic `EncodeQuantizedBand` actually reads, and runs a hybrid encoder: GPU wavelet plus quantization, then parallel CPU subband entropy, then the canonical CineForm bitstream.

Every number below is from one Release process on this machine unless a row says otherwise.

| Class | Meaning |
| :--- | :--- |
| measured | Timed in this process |
| derived | Arithmetic on measured values |
| estimated | A model, not a timer |
| hypothetical | Not implemented and not timed |

Machine: AMD Ryzen 9 7940HS (16 hardware threads, process affinity mask `0xFFFF`), NVIDIA GeForce RTX 4090 Laptop GPU (compute capability 8.9), MSVC 19.44, CUDA 12.8, kernels compiled for `sm_89`. Upstream `upstream/` was not modified. `_THREADED_ENCODER` is 0 in this build, so the stock encoder does not run `EncodeQuantizedGroupThreaded`.

Raw records: `benchmarks/results/hybrid_phase1c.json`.

## Canonical configuration

All full-encoder and hybrid numbers use this setup.

| Item | Value |
| :--- | :--- |
| Input | `CFHD_PIXEL_FORMAT_BGRA` |
| Encoded format | `CFHD_ENCODED_FORMAT_YUV_422` |
| Quality | `CFHD_ENCODING_QUALITY_FILMSCAN3` |
| Flags | `CFHD_ENCODING_FLAGS_NONE` |
| GOP | 1, progressive, spatial pyramid, 3 levels, 3 channels |
| Pattern | Deterministic BGRA frame, LCG seed `0x1337BEEF`, low-amplitude noise on a spatial gradient |
| Warm-up | 2 frames, not included in the average |
| Timed frames | 4 at 1080p and 4K, 2 at 8K for the hybrid runs |
| Timer | `CFHD_EncodeSample` only. `CFHD_GetSampleData` is outside the timer |
| Threads | No affinity change. Entropy worker count is explicit and reported per row |

`CFHD_GetSampleData` on the flat 4K frame added 1.41 ms (measured: 31.21 ms without it, 32.62 ms with it).

## 1. Why 32 ms and 55 ms were different

They are not two readings of the same encode.

The Phase 1B matrix filled the frame with BGRA 128. Re-running that frame through `CFHD_EncodeSample` at 4K measures **31.21 ms**. That is the ~32.08 ms matrix result.

The Phase 1B profiler used a gradient-plus-checkerboard frame. The same pattern on this machine measures **41.05 ms**, not 55.61 ms. This run does not reproduce 55.61 ms. The profiler's published total was a wall-clock around `CFHD_EncodeSample`, not a sum of nested timers. Adding level 1 a second time on top of today's gradient replay would produce 47.85 ms (derived), which still does not reach 55.61. The unreproduced 13 ms is left as a difference between that earlier process and this one, not as double-counted time inside the 32 ms run.

What the content sweep does show, on one 4K configuration, is that entropy moves and the transform barely does:

| Pattern | Full encode | Color | Level-1 wavelet | Levels 2–3 | Entropy and framing |
| :--- | ---: | ---: | ---: | ---: | ---: |
| Flat BGRA 128 | 31.21 ms | 16.60 ms | 8.11 ms | 2.83 ms | 7.49 ms |
| Gradient checker | 41.05 ms | 14.68 ms | 7.14 ms | 2.45 ms | 16.45 ms |
| Canonical textured | 92.47 ms | 16.22 ms | 8.08 ms | 2.56 ms | 66.59 ms |

Those stage times are measured by calling the production routines once each, in order, with no overlapping clocks. `ComputeGroupTransformQuant` starts at wavelet index 1. It does not recompute level 1. The level-1 row and the levels 2–3 row are disjoint.

On the canonical 4K replay the outer clock is 96.54 ms and the six stage clocks sum to 93.45 ms. The 3.09 ms gap is timer and call overhead, 3.2% of that replay. Replay versus `CFHD_EncodeSample` is 96.54 / 92.47 = **1.044** (derived). On the resolution sweep the same ratio is 1.001 at 1080p, 1.021 at 4K, and 1.027 at 8K.

Stage shares below are the canonical 4K replay, normalized to its own wall clock. They are exclusive of each other. They are not shares of a different flat-frame encode.

| Stage | ms | Share of instrumented wall |
| :--- | ---: | ---: |
| Color conversion | 16.22 | 16.8% |
| Level-1 wavelet, Y+U+V | 8.08 | 8.4% |
| Levels 2 and 3 | 2.56 | 2.6% |
| Entropy and framing | 66.59 | 69.0% |
| Timer overhead | 3.09 | 3.2% |

The old profiler also split entropy into scan, VLC, and an 85/15 packing guess taken from the remainder. That split is not exclusive measurement. This phase does not repeat it.

## 2. Canonical upstream encoder

`CFHD_EncodeSample`, canonical pattern, FilmScan 3, BGRA to YUV 4:2:2.

| Resolution | ms/frame | FPS (derived) |
| :--- | ---: | ---: |
| 1080p | 23.71 | 42.2 |
| 4K | 93.25 | 10.7 |
| 8K | 362.24 | 2.76 |

FPS is `1000 / ms`. Instrumented replay of the same call is 23.73, 95.24, and 371.83 ms.

## 3. What quantization does to the transfer size

`EncodeQuantizedBand` asserts `PIXEL_TYPE_16S` and calls `EncodeQuantLongRuns` on `PIXEL` (`int16_t`). `_HIGHPASS_8S` is 0, so highpass bands are not stored as 8-bit samples. Quantization changes coefficient values. It does not change the container.

`EncodeQuantLongRuns` converts pitch from bytes to samples and skips the gap `pitch/sizeof(PIXEL) - width`. Padding is not entropy input.

On 1080p, 4K, and 8K the encoder pitch of every measured band is `width * 2`. Padding is zero. Allocated bytes, entropy-input bytes, and the minimum device-to-host copy for the current entropy routine are the same number.

The wavelet is critically sampled, so the 30 encoded subbands contain the same number of samples as the 16-bit YUV 4:2:2 planes. That is why a quantized copy is the same size as the upload:

| Resolution | H2D and minimum D2H | Bytes |
| :--- | :--- | ---: |
| 1080p | Y + both chroma planes, int16, tight | 8,294,400 |
| 4K | same | 33,177,600 |
| 8K | same | 132,710,400 |

The Phase 1B 16.59 MB figure was one 4K int16 plane, copied back as four int16 subbands of half resolution. Four half-resolution int16 bands are the same byte count as the plane. Quantization was already applied. The container stayed 16-bit.

Highpass values on the canonical frame fit in int8. The three lowpass bands do not (4K luma lowpass spans 1058..12459). Packing only the bands that fit, and leaving lowpass in int16, would be 16,848,000 bytes at 4K (derived from the measured ranges). `EncodeQuantizedBand` cannot read that packed form. The hybrid copies the 33,177,600 bytes the entropy routine reads.

### 4K subbands

Channel 0 is Y. Channels 1 and 2 are the two chroma planes after `ConvertRGB32to10bitYUVFrame` swaps the U and V destinations. Level 1 is the full-resolution highpass. Level 3 is the top of the pyramid. Storage is int16. Bytes are `width * height * 2`.

| Channel | Level | Band | Width | Height | Storage | Bytes |
| :--- | ---: | :--- | ---: | ---: | :--- | ---: |
| Y | 3 | LL | 480 | 270 | int16 | 259,200 |
| Y | 3 | LH | 480 | 270 | int16 | 259,200 |
| Y | 3 | HL | 480 | 270 | int16 | 259,200 |
| Y | 3 | HH | 480 | 270 | int16 | 259,200 |
| Y | 2 | LH | 960 | 540 | int16 | 1,036,800 |
| Y | 2 | HL | 960 | 540 | int16 | 1,036,800 |
| Y | 2 | HH | 960 | 540 | int16 | 1,036,800 |
| Y | 1 | LH | 1920 | 1080 | int16 | 4,147,200 |
| Y | 1 | HL | 1920 | 1080 | int16 | 4,147,200 |
| Y | 1 | HH | 1920 | 1080 | int16 | 4,147,200 |
| C1 | 3 | LL | 240 | 270 | int16 | 129,600 |
| C1 | 3 | LH | 240 | 270 | int16 | 129,600 |
| C1 | 3 | HL | 240 | 270 | int16 | 129,600 |
| C1 | 3 | HH | 240 | 270 | int16 | 129,600 |
| C1 | 2 | LH | 480 | 540 | int16 | 518,400 |
| C1 | 2 | HL | 480 | 540 | int16 | 518,400 |
| C1 | 2 | HH | 480 | 540 | int16 | 518,400 |
| C1 | 1 | LH | 960 | 1080 | int16 | 2,073,600 |
| C1 | 1 | HL | 960 | 1080 | int16 | 2,073,600 |
| C1 | 1 | HH | 960 | 1080 | int16 | 2,073,600 |
| C2 | 3 | LL | 240 | 270 | int16 | 129,600 |
| C2 | 3 | LH | 240 | 270 | int16 | 129,600 |
| C2 | 3 | HL | 240 | 270 | int16 | 129,600 |
| C2 | 3 | HH | 240 | 270 | int16 | 129,600 |
| C2 | 2 | LH | 480 | 540 | int16 | 518,400 |
| C2 | 2 | HL | 480 | 540 | int16 | 518,400 |
| C2 | 2 | HH | 480 | 540 | int16 | 518,400 |
| C2 | 1 | LH | 960 | 1080 | int16 | 2,073,600 |
| C2 | 1 | HL | 960 | 1080 | int16 | 2,073,600 |
| C2 | 1 | HH | 960 | 1080 | int16 | 2,073,600 |

Thirty bands. Sum of the byte column is 33,177,600.

## 4. Subband entropy

`EncodeQuantizedFrameTransform` walks each channel from the top of the pyramid and encodes LH, then HL, then HH. Lowpass is subband 0 and is encoded by `EncodeLowPassBand` before that loop. Highpass subbands restart at 1. Each band carries its own header and a byte-aligned trailer. Nothing in `EncodeQuantLongRuns` reads another band's coefficients. The 30 jobs are independent.

The scheduler calls those same routines. Each worker has its own `BITSTREAM` and output buffer. Payloads are concatenated after every worker finishes, in canonical channel and level order, with the same group, channel, highpass, and sample-size tags as `EncodeQuantizedGroup`. There is no shared bit writer and no lock inside the coefficient scan.

Byte identity with `EncodeQuantizedGroup` was measured for subband worker counts 1 and 8 at 1080p and 4K, for 3-way channel workers at 1080p, and for the hybrid bitstream at 1080p, 4K, and 8K (0 coefficient mismatches, then a matching assembly). Other worker rows were timed without a second serial encode; their encoded sizes match the compared rows.

Entropy stage time, canonical frames. Assembly is under 1 ms and is listed in the JSON.

| Workers | 1080p | 4K | 8K |
| ---: | ---: | ---: | ---: |
| 1 | 22.55 ms | 85.46 ms | 338.37 ms |
| 2 | 13.28 ms | 44.41 ms | 168.38 ms |
| 4 | 6.34 ms | 25.16 ms | 87.36 ms |
| 8 | 4.65 ms | 18.05 ms | 70.50 ms |
| 16 | 4.56 ms | 15.93 ms | 68.60 ms |
| 3 channels | 12.55 ms | 48.46 ms | 181.57 ms |

Fastest configuration: **16 subband workers**. 4K efficiency from 1 to 16 workers is `(85.46 / 15.93) / 16 = 0.33` (derived). Most of the gain is in place by 8 workers (efficiency 0.59). At 16 workers the slowest worker still takes 15.6 ms and the fastest busy worker takes 3.7 ms, so the extra threads do not have equal work.

Three channel workers, which is what the dormant upstream threaded encoder does, land between 2 and 4 subband workers. Subband scheduling is faster because the large bands are all in Y. Channel threading cannot split Y.

Serial per-band times at 4K, slowest first:

| Band | ms | Coefficients | Encoded bytes |
| :--- | ---: | ---: | ---: |
| Y level-1 HL | 10.64 | 2,073,600 | 658,768 |
| Y level-1 LH | 10.57 | 2,073,600 | 643,616 |
| Y level-1 HH | 8.60 | 2,073,600 | 456,940 |
| C1 level-1 LH | 5.50 | 1,036,800 | 424,600 |
| C1 level-1 HH | 5.36 | 1,036,800 | 290,592 |
| C1 level-1 HL | 5.20 | 1,036,800 | 425,016 |

Those six bands are the runtime. Scheduling overhead outside the slowest worker is about 0.4–0.6 ms.

4K coefficient scan, runs counted across rows and not across row padding:

| Quantity | Value |
| :--- | ---: |
| Coefficients | 16,588,800 |
| Zeros | 8,976,794 (54.1%) |
| Nonzeros | 7,612,006 |
| Zero runs | 3,194,331 |
| Mean zero run | 2.81 samples |
| Runs of length 1 | 1,603,803 |
| Encoded bits per coefficient, headers included | 2.25 |

Runs are short. A zero-run detector would still leave a nonzero symbol for almost half of the samples.

## 5. Hybrid pipeline

Host BGRA is converted with the production `ConvertRGB32to10bitYUVFrame`. The three int16 planes are packed tightly into pinned memory, uploaded, transformed, and quantized on the GPU. Level index 1 uses the 10-bit prescale `(sample + 3) >> 2` horizontal filter; the other levels use the ordinary 2-6 filter. Only the 30 entropy bands are copied back. They are scattered into the encoder's band buffers and encoded by the 16-worker subband scheduler.

At 1080p, 4K, and 8K the GPU coefficients matched the upstream pyramid on every compared sample (`coeff_mismatches = 0`). The assembled sample matched `EncodeQuantizedGroup`. A 128×128 frame still shows a few dozen left-edge differences on the smallest chroma band; that size is below the canonical set and is not used for the speed table.

The upstream decoder accepted the hybrid samples. PSNR against the source BGRA frame was 44.58 dB at 1080p, 44.68 dB at 4K, and 44.70 dB at 8K. FFmpeg is not installed on this machine, so there is no FFmpeg decode result. FFmpeg is not linked into the codec.

Pipelined mode keeps three frame slots. While frame N is entropy-coded, frame N+1 is converted and transferred and frame N's GPU work has already finished into that slot. Latency is submit-to-bitstream. FPS divides the frame count by the wall clock of the run, including the first frame.

| | 1080p sync | 1080p pipe | 4K sync | 4K pipe | 8K sync | 8K pipe |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| Sustained FPS | 59.22 | 87.01 | 15.97 | 22.71 | 4.27 | 5.22 |
| Mean latency | 16.83 ms | 18.35 ms | 62.50 ms | 70.12 ms | 233.94 ms | 254.67 ms |
| Startup | 16.43 ms | 16.66 ms | 63.87 ms | 63.44 ms | 237.23 ms | 244.08 ms |
| Color | 3.46 ms | 4.07 ms | 13.20 ms | 16.44 ms | 52.14 ms | 63.14 ms |
| Pack | 0.47 ms | 0.51 ms | 2.65 ms | 2.60 ms | 8.46 ms | 11.27 ms |
| H2D | 1.26 ms | 1.28 ms | 4.97 ms | 5.02 ms | 19.83 ms | 19.89 ms |
| GPU wavelet + quant | 0.27 ms | 0.22 ms | 0.34 ms | 0.35 ms | 1.37 ms | 1.42 ms |
| D2H | 1.35 ms | 1.35 ms | 5.11 ms | 5.12 ms | 20.25 ms | 20.25 ms |
| Entropy | 4.95 ms | 4.56 ms | 17.43 ms | 18.21 ms | 67.72 ms | 66.82 ms |
| Assembly | 0.06 ms | 0.05 ms | 0.28 ms | 0.26 ms | 0.93 ms | 1.59 ms |
| H2D = D2H bytes | 8,294,400 | | 33,177,600 | | 132,710,400 | |
| Encoded bytes | 1,181,024 | | 4,668,864 | | 18,393,596 | |

The pipeline's single-frame latency is higher than its average frame period. At 4K the pipe delivers a frame every 44.0 ms (derived, `1000 / 22.71`) while a frame spends 70.1 ms from submit to bitstream.

4K PCIe rates from the synchronous event times are 6.67 GB/s host-to-device and 6.49 GB/s device-to-host (derived). GPU wavelet time divided by the synchronous wall clock is 0.55% (derived from CUDA events, not an SM-utilization counter). Process CPU time over that wall clock is about 4.3 cores (derived).

## 6. GPU color conversion

The CUDA kernel implements the integer path used by `ConvertRGB32to10bitYUVFrame` for BGRA: byte to 16-bit planar RGB with an 8-bit left shift, the `BITSUSED = 13` coefficient conversion selected by `COLOR_SPACE_CG_709` (color space value 2 on this encoder), saturating 16-bit `mulhi` accumulation, the 0..16383 clamp, the final left shift, a logical right shift of 6, even-sample chroma, and the vertical flip the production function applies before the row loop. Mismatches against the CPU function: **0** at 1080p and **0** at 4K.

| | 1080p | 4K |
| :--- | ---: | ---: |
| CPU `ConvertRGB32to10bitYUVFrame` | 3.38 ms | 12.97 ms |
| CUDA kernel, BGRA already on device | 0.114 ms | 0.158 ms |
| Second kernel, still resident | 0.033 ms | 0.114 ms |
| H2D of BGRA plus kernel | 1.43 ms | 5.30 ms |
| D2H of the three planes, separate | 1.62 ms | 5.28 ms |

The kernel is not wired into the timed hybrid path. The hybrid still spends the CPU conversion time above. BGRA at 8 bits and 4:2:2 YUV at 16 bits are the same byte count, so the color kernel does not shrink the upload. It removes the 13 ms CPU conversion when the upload is going to happen anyway.

## 7. Whole-encoder speedup

Derived from the measured FPS above. Same pattern, format, quality, compiler, and machine.

| Resolution | Upstream FPS | Hybrid sync FPS | Hybrid pipe FPS | Sync speedup | Pipe speedup |
| :--- | ---: | ---: | ---: | ---: | ---: |
| 1080p | 42.2 | 59.2 | 87.0 | 1.40× | 2.06× |
| 4K | 10.7 | 16.0 | 22.7 | 1.49× | 2.12× |
| 8K | 2.76 | 4.27 | 5.22 | 1.55× | 1.89× |

## 8. GPU entropy is not justified yet

Measured remainder, fastest CPU entropy configuration:

| Resolution | Entropy ms | Share of hybrid sync latency | Share of pipe frame period |
| :--- | ---: | ---: | ---: |
| 1080p | 4.60 | 27% | 40% of 11.5 ms |
| 4K | 16.15 | 26% | 37% of 44.0 ms |
| 8K | 69.56 | 30% | 36% of 191.5 ms |

The pipe-period shares are derived. Entropy overlaps the next frame's color conversion and transfer, so those shares are not added on top of the other stages.

Scaling from 1 to 16 workers at 4K is 5.37× on 16 threads, efficiency 0.33. The three level-1 luma bands dominate serial time. Half of the coefficients are zero, but the average run is 2.81 samples and half of the runs have length 1. Encoded size is 2.25 bits per coefficient including headers.

| Proposal | Status |
| :--- | :--- |
| A. GPU zero-run detection only | hypothetical |
| B. GPU symbol generation | hypothetical |
| C. GPU prefix-sum bit offsets | hypothetical |
| D. Full GPU VLC bit packing | hypothetical |

No GPU entropy kernel was built, and none of those four have a measured runtime.

If entropy on the critical path disappeared and nothing else changed, 4K synchronous latency would fall from 62.5 ms by about 17.4 ms, to roughly 45 ms (derived upper bound). That is another ~1.4× on the synchronous path, in the same region the pipeline already reaches, before any new device-to-host traffic for a packed bitstream. It is not an order-of-magnitude change. The next measured wins are the color kernel, which is already exact, and the transfer, which is still half of a 4K GPU round trip. GPU VLC should wait until those are in the timed pipeline and entropy is still the limiter.

## Where the time goes now

At 4K the GPU wavelet is 0.34 ms. The synchronous frame is 62.5 ms. The large pieces are CPU color conversion (~13 ms), the int16 upload and download (~10 ms together), and subband entropy (~17 ms). Entropy used to be 85 ms on one thread. Sixteen workers brought it in line with color conversion. That pair, plus the transfer, is the bottleneck.
