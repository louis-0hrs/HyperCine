# HyperCine Phase 2A: GPU Frontend and Band-Streamed Hybrid Encoder

Phase 2A does not implement GPU entropy or GPU VLC. It puts the verified BGRA color kernel on the real encode path, copies coefficients back per subband so CPU entropy can start early, and measures whether a lossless int8 transport is worth the cost.

The pristine `upstream/` tree was not modified. `_THREADED_ENCODER` stays 0 in the stock library. A second build of those same sources with the flag set to 1 does not compile. See `docs/UPSTREAM.md`.

Numbers below are from one Release process on this machine unless a row says otherwise. Raw records: `benchmarks/results/phase2a.json`.

| Class | Meaning |
| :--- | :--- |
| measured | Timed in this process |
| derived | Arithmetic on measured values |

Machine: NVIDIA GeForce RTX 4090 Laptop GPU, compute capability 8.9, CUDA 12.8, kernels for `sm_89`. CUDA reports one asynchronous copy engine and `gpu_overlap = 1`. `nvidia-smi` reports the negotiated link as **PCIe Gen4 x8** (maximum generation 4, maximum width x16). Canonical content, BGRA, FilmScan 3, GOP 1, progressive, seed `0x1337BEEF`. Warm-up is outside the window. Sustained FPS is frame count divided by wall time, including pipeline fill. Latency is submit-to-bitstream.

GB/s uses `10^9` bytes per second.

## 1. Upstream default and upstream threaded

`CFHD_EncodeSample` only. Two warm-up frames. Four timed frames at 1080p and 4K, two at 8K.

| Resolution | ms/frame | FPS |
| :--- | ---: | ---: |
| 1080p | 24.82 | 40.29 |
| 4K | 97.28 | 10.28 |
| 8K | 388.22 | 2.58 |

The threaded library (`CFHDCodecThreaded`, `-D_THREADED_ENCODER=1`, no edit under `upstream/`) fails in `encoder.c` and `wavelet.c`: missing identifiers (`display_height`, `ChromaFullRes`, `limit_yuv`) and calls that no longer match current prototypes (`SetEncoderFormat`, `PutVideoIntraFrameHeader`, `EncodeQuantizedFrameTransform`, `FilterSpatialYUVQuant16s`). A `/FI` forward declaration gets past the C++ typedef order error in `encoder.h` and does not make the threaded functions compile.

There is no functioning upstream threaded configuration. The fastest functioning upstream encoder is the default build. Channel threading inside `EncodeQuantizedGroupThreaded` was not observed because that function is not in the binary. Speedup versus the fastest functioning upstream configuration is the same number as speedup versus the default build.

## 2. GPU BGRA frontend

Host path, no CPU YUV frame:

BGRA host → H2D → GPU BGRA-to-10-bit-YUV 4:2:2 → GPU wavelet → GPU quantization → CPU entropy.

The color kernel writes Cb and Cr in natural order. Encoder channel 1 is Cr and channel 2 is Cb, so the chroma destinations are swapped before the wavelet, matching `ConvertRGB32to10bitYUVFrame`.

Device path: the frame is uploaded once before the timed window. Timed frames read that device pointer. H2D during the window is 0 bytes (measured 0.002–0.006 ms of event noise).

At 1080p, 4K, and 8K the sustained runs compared every GPU coefficient with the upstream pyramid: **0 mismatches**. The assembled sample matched `EncodeQuantizedGroup` (`byte_match = true`). An int8 band was checked against the int16 device buffer before that compare: **0 expansion mismatches**. `tests/test_phase2a` decoded the 1080p compact sample with the upstream decoder at **44.47 dB**. The host-compact and device-resident samples decode to the same pixels. One header byte differs because the frame number advanced. FFmpeg, Premiere, and Resolve were not run.

## 3. Raw PCIe

Independent of the codec. Sizes 4, 8, 16, 32, 64, and 128 MB. The rate is flat across that range, so the figures below are the 128 MB row from the run taken while controlled folder access was still interfering with this tree. The whitelisted rerun that wrote `benchmarks/results/phase2a.json` measures the same plateau at about 6.6 GB/s (128 MB pageable host-to-device 6.61 GB/s, device-to-host 6.46 GB/s). Pinning still does not raise it, and the bidirectional copy still lands on that same plateau, so the two directions still do not overlap. The tables below keep the earlier 3.3 GB/s rows as the record of that interfered run. Use the JSON for the current link.

> **Canonical Uncontended PCIe Update (Phase 2C)**: A standalone uncontended rerun across 4, 8, 16, 32, 64, and 128 MB (9 runs each, recorded in `benchmarks/results/phase2c.json` and `docs/CUDA_PHASE2C.md` Section 7) established the canonical hardware baseline on this machine: **13.30–13.40 GB/s H2D** and **12.94–13.15 GB/s D2H** for pinned memory (`cudaMallocHost`, `cudaHostAlloc`, `write_combined`), and **11.0–12.7 GB/s** for pageable memory. The 3.3 GB/s table below and the 6.6 GB/s `phase2a.json` records are preserved here as historical measurements of the Phase 2A runs.

| Host memory | Direction | GB/s |
| :--- | :--- | ---: |
| pageable | H2D | 3.30 |
| pageable | D2H | 3.24 |
| `cudaMallocHost` | H2D async | 3.36 |
| `cudaMallocHost` | D2H async | 3.29 |
| `cudaHostAlloc` | H2D async | 3.36 |
| `cudaHostAlloc` | D2H async | 3.29 |
| write-combined | H2D async | 3.36 |
| write-combined | D2H async | 3.29 |
| `cudaMallocHost` | both directions at once | 3.32 aggregate |

Pinned, write-combined, and asynchronous copies do not rise above the pageable plateau. The bidirectional row counts both directions and still lands on the same 3.3 GB/s, which means the two copies did not overlap. That matches one asynchronous engine.

Gen4 x8 payload bandwidth is about 15.8 GB/s. The copy path measured here is about one fifth of that. The codec's own 4K and 8K latency-mode copies land on the same plateau (4K host-to-device 33,177,600 bytes in 10.01 ms = 3.31 GB/s; 8K 132,710,400 bytes in 39.65 ms = 3.35 GB/s). The link really is this slow in this process. It is not an assumption carried over from the encoder.

1080p event spans are longer than `bytes / 3.3 GB/s`. Those spans include launch gaps. The 4K and 8K rows are the transfer times.

## 4. Coefficient ranges

Corpus: flat 0, flat 128, flat 255, gradient, checker, noise, impulse, high-frequency columns, and the canonical frame, at six qualities (low through FilmScan 3) at 1080p, plus canonical, noise, high-frequency, and flat 128 at 4K FilmScan 3. 58 frames. 30 bands.

Lowpass never fits in int8. Across the corpus the luma lowpass spans 1024..15024. Both chroma lowpass bands stay above 1900.

Highpass usually fits. Entire-band fit is 89–100% of frames depending on the band. The bands that miss are the large level-1 bands on noise and high-frequency frames (values past ±127, up to about ±473). Even on those frames, about 98–100% of the individual coefficients still lie in [-128, 127]. The transport does not use that partial fit. A band is int8 only when every value fits. Nothing is saturated.

On the canonical FilmScan 3 frame the byte counts show all 27 highpass bands in int8 and the 3 lowpass bands in int16:

| Resolution | int16 frame | compact | int8 bands |
| :--- | ---: | ---: | ---: |
| 1080p | 8,294,400 | 4,212,000 | 27 |
| 4K | 33,177,600 | 16,848,000 | 27 |
| 8K | 132,710,400 | 67,392,000 | 27 |

4,212,000 and 16,848,000 are the sizes Phase 1C derived for this packing. The entropy workers read int8 bands directly. The bitstream matches `EncodeQuantizedGroup`.

## 5. Range detection

A device reduction writes min and max. The CPU then either packs to int8 or copies int16. No value outside [-128, 127] is stored as int8.

The decision is not free. At 4K the latency-mode compact frame spent 2.05 ms in the range kernels and 6.23 ms packing, and device-to-host fell only from 10.35 ms (int16 streaming) to 9.22 ms. The detector plus the pack costs more than the copy it removes. At 8K the pack alone is about 23 ms. Sustained host FPS follows that: 4K int16 streaming is 43.3 fps and compact streaming is 40.7 fps.

The current range-detection path is rejected. The byte reduction is real and lossless. The synchronization used to choose the width is more expensive than the bytes.

## 6. Band streaming

Each published subband is copied on a second stream as soon as its level finishes. A worker pool (16 threads) starts `EncodeQuantizedBand`-equivalent work on that band. Later levels keep running. Workers may finish out of order. Assembly writes the canonical channel and level order.

At 4K int16 streaming, 8.80 ms of the 10.35 ms copy overlaps entropy (85% of the copy). At 8K, 35.0 ms of 40.6 ms overlaps (86%). Scheduling delay from “band is on the host” to “worker starts” is 0.12 ms at 4K and 0.05 ms at 8K. The queue holds about one ready band. Assembly itself is 0.17 ms at 4K and 0.95 ms at 8K. The concatenate starts about 2.5 ms (4K) to 7.8 ms (8K) after the last worker finishes.

Full-frame int16 copy waits until every band has arrived, so entropy overlap with the copy is under 0.1 ms.

GPU wavelet time on the int16 path is 0.49 ms at 4K and 1.59 ms at 8K. Overlap of that compute with the copy is a few tenths of a millisecond. The copy is much longer than the wavelet.

## 7. Transport comparison

Sustained FPS from the recorded run. Latency is the separate one-frame, depth-1 measurement.

| Path | 1080p FPS | 1080p latency | 4K FPS | 4K latency | 8K FPS | 8K latency | D2H bytes, 4K |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Full int16, GPU color | 44.1 | 28.4 ms | 39.0 | 41.8 ms | 8.54 | 157 ms | 33,177,600 |
| Int16 band stream | 44.8 | 22.6 ms | 43.3 | 30.6 ms | 10.2 | 113 ms | 33,177,600 |
| Compact band stream | 101 | 10.0 ms | 40.7 | 29.2 ms | 10.8 | 112 ms | 16,848,000 |
| Device resident + compact | 139 | — | 65.3 | 29.5 ms | 18.6 | 101 ms | 16,848,000 |

The 1080p device-resident cell is sustained FPS. Its one-frame latency was not measured separately. The one-frame device path that was measured is full int16, not compact: 9.2 ms at 1080p, 31.9 ms at 4K, 116 ms at 8K, with zero host-to-device bytes. The 4K and 8K latency numbers in the device-resident row are mean submit-to-bitstream while the sustained pipeline is running, so they are longer than the frame period.

1080p sustained FPS moved between about 45 and 144 across three consecutive runs of the same binary. 4K int16 streaming stayed at 42.5–43.3, and 8K int16 streaming stayed at 10.2–10.3. The 4K and 8K rows are the stable ones. 1080p is short enough that scheduling gaps dominate the average.

Host-to-device bytes are one BGRA frame (8,294,400 / 33,177,600 / 132,710,400) except the device-resident path, which is 0. In-encoder copy rate at 4K and 8K matches the raw 3.3 GB/s bench.

CPU use on the 4K int16 stream is about 5.9 cores. The compact 4K stream is about 5.6 cores. Device-resident 4K is about 7.9 cores.

## 8. Complete encoders

Sustained FPS, same content and settings.

| Path | 1080p | 4K | 8K |
| :--- | ---: | ---: | ---: |
| Upstream default | 40.3 | 10.3 | 2.58 |
| Upstream threaded | not built | not built | not built |
| Phase 1C pipeline | 33.0 | 18.8 | 4.20 |
| GPU color, full int16 copy | 44.1 | 39.0 | 8.54 |
| Int16 band stream | 44.8 | 43.3 | 10.2 |
| Compact band stream | 101 | 40.7 | 10.8 |
| Device resident + compact | 139 | 65.3 | 18.6 |

Single-frame latency is the latency column in the previous table. Upstream latency equals its frame time (the stock encoder does not overlap the next frame). At 4K that is 97.3 ms. Int16 band streaming returns one frame in 30.6 ms and then sustains 43.3 fps (a frame every 23.1 ms) because the next frame is already on the GPU.

## 9. Compatibility

Every sustained GPU path in the table has `byte_match = true`, `coeff_mismatches = 0`, and `expand_mismatches = 0`. The 128×128 hybrid test still shows a few dozen left-edge coefficient differences; that size is outside the canonical set, and a good decode of it is about 43 dB. The upstream decoder does not return the same pixels on two opens of one sample on this machine, so PSNR checks take the best of several decodes.

## 10. Where the time goes, and Phase 2B

4K int16 band stream, one frame, 30.6 ms:

| Piece | ms | Share of that latency |
| :--- | ---: | ---: |
| Host-to-device BGRA | 10.01 | 33% |
| Color kernel | 0.28 | 0.9% |
| GPU wavelet and quant | 0.49 | 1.6% |
| Device-to-host int16 | 10.35 | 34%, of which 8.8 ms overlaps entropy |
| CPU entropy | 15.50 | 51%, of which 8.8 ms overlaps the copy |
| Schedule | 0.12 | 0.4% |
| Assembly | 0.16 | 0.5% |

On the device-resident compact path the upload is gone. At 4K the frame period is 15.3 ms (65.3 fps) and entropy still takes 21.0 ms, so entropy spills into the next frame. At 8K the period is 53.8 ms and entropy takes 76.5 ms. Color is about 1 ms. Assembly is about 1 ms. The compact pack (about 7 ms at 4K, 21 ms at 8K) is larger than the copy it saves and is rejected.

Phase 1C already measured short zero runs (mean 2.81 samples, half of them length 1). A GPU zero-run scan or a GPU symbol pass would still leave the nonzero coefficients and the bitstream writer. Sixteen CPU workers are already in use; more host threads will not split the three large luma bands evenly.

Phase 2B should target **full GPU entropy**, and it should not start from the rejected range-detection path. The upload is no longer required for a GPU-resident caller, the wavelet is about 0.5 ms at 4K, and the remaining multi-tens-of-milliseconds stage is entropy. That work is not begun here.
