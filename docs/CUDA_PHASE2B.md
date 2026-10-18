# HyperCine Phase 2B: GPU Entropy

> [!NOTE]
> **Historical Phase 2B benchmark record.** This document captures a single early GPU-entropy development stage and its local tests, not the current release-candidate performance or decoder certification. See the [documentation index](README.md) and [Phase 3A.2c canonical benchmark](../benchmarks/phase3a2c_release_candidate.json).

Phase 2B moves highpass entropy onto the GPU one stage at a time. The wavelet and the quantizer are the Phase 2A kernels. The bitstream is still the stock CineForm coder: same run symbols, same codebook, same MSB-first 32-bit words, same band header and trailer. `upstream/` was not modified. `_THREADED_ENCODER` is 0. There is no functioning threaded upstream build, so every speedup below is against the default `CFHD_EncodeSample` path. See `docs/UPSTREAM.md`.

The algorithm and the golden band hashes are in `docs/ENTROPY.md`. Numbers below are the Release run that wrote `benchmarks/results/phase2b.json` on the RTX 4090 Laptop, CUDA 12.8, `sm_89`, canonical BGRA, FilmScan 3, GOP 1, progressive, seed `0x1337BEEF`. Upstream uses two warm-up frames and four timed frames at 1080p and 4K, two at 8K. Phase 2A and Phase 2B rows in that file use one warm-up frame and four, three, and two timed frames, plus one verified frame before the window. Phase 1C and the full Phase 2A matrix are the separate files written in the same sitting: `benchmarks/results/hybrid_phase1c.json` and `benchmarks/results/phase2a.json`.

## 1. Flaky 128×128 YUY2 test

`tests/test_codec_roundtrip` links only the stock codec. Two encoder opens in one process disagree in `TAG_CLIP_GUID`, `DATE`, `TIME`, and `TIMC` because the SDK stamps a new GUID and the local clock into the sample. After those payloads are masked, the two samples match, including across a one-second clock step. A second process can still differ by four coded bytes on a flat black frame. That residue is not the clock and not an unseeded `rand()` in the encoder. The regression does not freeze a cross-process hash.

The decoder side is separate. The stock library is built with `_THREADED_DECODER=1`, and `InvertHorizontalStrip16sToYUYV` calls `rand()` from the worker threads, so two decodes of one sample are not pixel-identical. PSNR is printed and is not a pass/fail gate. `tests/test_yuy2_stability` requires masked equality inside one process, exactly one GUID mismatch, and a successful decode at 128×128 YUY2 with pitch 256. Upstream was not edited to hide this.

## 2. Entropy stages

`EncodeQuantizedBand` for a FilmScan progressive highpass band does this, in order:

1. `SetCodingFlags` selects codebook 1 and peaks coding off.
2. Walk coefficients in row-major order. Padding past `width` is an implicit zero run and is not read.
3. Emit run-book indexes, then a clamped value-book index. The band-end code is one more symbol.
4. `PutBits` packs MSB-first into 32-bit words and flushes with `PutLong(SwapInt32)`.
5. `PutVideoBandHeader` opens the band on a 32-bit boundary. `PutVideoBandTrailer` zero-pads, flushes, and writes the trailer tags.

Lowpass is not this coder. It stays on the CPU: the int16 band is copied back and `EncodeLowPassBand` writes raw pairs. Peaks coding is also a CPU fallback. The measured FilmScan path does not take it.

The GPU side is four kernels, checked before the next one is trusted:

| Stage | Output | Check |
| :--- | :--- | :--- |
| 2B-A scan and tokens | run/value indexes, including runs that cross a 256-thread block | same sequence as the CPU walker |
| 2B-B codebook | `codeword[i]`, `bit_length[i]`, plus the band-end code | same entry the CPU table returns |
| 2B-C prefix | exclusive bit offsets, `offset[0] = 0` | CUB `DeviceScan::ExclusiveSum` on the bit lengths; the final sum equals the CPU bit count |
| 2B-D pack | big-endian coefficient words | one thread owns each output word, so a code that crosses a byte, a word, or a work item has one writer |

`tests/test_phase2b` requires byte equality with `EncodeOneExternalBand` for the synthetic vectors, every luma highpass band of the 128×128 canonical frame, and the three 4K luma level-1 bands (LH, HL, HH). Stopping after tokens, or after codewords, and finishing on the CPU produces the same coefficient bytes. The 1080p, 4K, and 8K full samples match `EncodeQuantizedGroup`, and the GPU coefficients match the refreshed CPU pyramid (the verify flag requires both). The stock decoder accepts the 1080p sample at 44.47 dB. FFmpeg, Premiere, and Resolve were not run. FFmpeg is not a build dependency.

## 3. Where the time goes

Isolated entropy, all highpass bands, one stream. `CPU_ENTROPY` is the staged reference on one thread. The two hybrid rows finish the remaining steps on the CPU. Times are milliseconds. Every row matched the CPU coefficient bytes.

| Resolution | Path | Wall | Scan | Tokens | Codebook | Prefix | Pack | Header |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1080p | CPU | 36.28 | 12.30 | 12.33 | 5.19 | 1.93 | 4.04 | 0.05 |
| 1080p | GPU tokens, CPU pack | 18.76 | 3.50 | 1.32 | 4.77 | 1.54 | 3.90 | 0.03 |
| 1080p | GPU codewords, CPU pack | 18.62 | 3.16 | 1.28 | 0.90 | 1.89 | 3.77 | 0.04 |
| 1080p | GPU full | 8.07 | 2.96 | 1.06 | 0.87 | 0.77 | 0.74 | 0.04 |
| 4K | CPU | 141.98 | 51.02 | 42.73 | 22.85 | 7.26 | 15.72 | 0.25 |
| 4K | GPU tokens, CPU pack | 62.74 | 7.37 | 1.19 | 21.86 | 6.93 | 15.86 | 0.11 |
| 4K | GPU codewords, CPU pack | 66.73 | 7.78 | 1.81 | 1.32 | 7.78 | 16.45 | 0.09 |
| 4K | GPU full | 15.24 | 7.92 | 1.30 | 1.16 | 0.61 | 0.92 | 0.38 |
| 8K | CPU | 574.33 | 194.18 | 180.89 | 88.88 | 30.10 | 66.88 | 2.67 |
| 8K | GPU tokens, CPU pack | 242.93 | 24.99 | 3.22 | 86.91 | 29.18 | 65.21 | 1.52 |
| 8K | GPU codewords, CPU pack | 253.19 | 26.80 | 3.31 | 1.77 | 27.62 | 67.37 | 1.49 |
| 8K | GPU full | 38.70 | 24.16 | 2.69 | 1.54 | 0.82 | 2.03 | 1.89 |

The hybrid rows show which stage matters. Moving only the scanner, or the scanner and the codebook, leaves the CPU pack in the lead and the wall stays above 240 ms at 8K. The full GPU pack is the row that drops 8K entropy from 574 ms on one thread to 39 ms. Inside that row the zero-run scan is the long kernel. Prefix sum is under 1 ms at 8K. Packing is about 2 ms.

`BlockScanKernel` can resident 6 blocks of 256 threads per SM. That is 1536 threads, which fills this GPU's per-SM thread limit, so the launch occupancy of the scan kernel is 100%. `nvidia-smi` samples during the runs peaked between about 10% and 80%. Those samples are sparse relative to a frame, so they are not a duty-cycle measurement.

## 4. Full frames

Phase 2B does not copy quantized highpass bands back to the host. The device path is:

device BGRA → GPU color → GPU wavelet → GPU quant → GPU entropy → compressed band payloads and the three lowpass bands to the host → CPU container assembly.

The host path inserts one BGRA upload at the start. Each band has its own coefficient range, token buffer, scan workspace, and output size. Bands are not funneled through one coder object. Assembly restores canonical order after the bands finish.

| Resolution | Path | Latency ms | FPS | Entropy ms | Assembly ms | H2D bytes | D2H bytes | Encoded bytes |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1080p | Upstream default | 23.66 | 42.27 |  |  |  |  |  |
| 1080p | Phase 2A int16 stream | 9.15 | 198.86 | 4.49 | 0.04 |  | 8,294,400 | 900,396 |
| 1080p | Phase 2A device resident | 11.14 | 174.47 | 7.86 | 0.04 |  | 4,212,000 | 900,396 |
| 1080p | Phase 2B host, 1 stream | 11.50 | 86.99 | 8.64 | 0.03 | 8,294,400 | 897,700 | 900,396 |
| 1080p | Phase 2B device, 1 stream | 10.55 | 94.83 | 8.97 | 0.03 | 0 | 897,700 | 900,396 |
| 4K | Upstream default | 94.39 | 10.59 |  |  |  |  |  |
| 4K | Phase 2A int16 stream | 30.80 | 52.77 | 18.65 | 0.23 |  | 33,177,600 | 3,563,684 |
| 4K | Phase 2A device resident | 21.78 | 75.93 | 17.41 | 0.27 |  | 16,848,000 | 3,563,684 |
| 4K | Phase 2B host, 1 stream | 24.62 | 40.62 | 15.38 | 0.18 | 33,177,600 | 3,560,988 | 3,563,684 |
| 4K | Phase 2B device, 1 stream | 15.97 | 62.60 | 12.85 | 0.14 | 0 | 3,560,988 | 3,563,684 |
| 8K | Upstream default | 369.79 | 2.70 |  |  |  |  |  |
| 8K | Phase 2A int16 stream | 112.69 | 14.95 | 57.91 | 0.89 |  | 132,710,400 | 14,009,028 |
| 8K | Phase 2A device resident | 80.94 | 23.23 | 64.31 | 1.43 |  | 67,392,000 | 14,009,028 |
| 8K | Phase 2B host, 1 stream | 68.12 | 14.68 | 36.57 | 1.53 | 132,710,400 | 14,006,332 | 14,009,028 |
| 8K | Phase 2B device, 1 stream | 44.53 | 22.46 | 33.87 | 1.03 | 0 | 14,006,332 | 14,009,028 |

Phase 2B D2H is compressed highpass payloads plus raw lowpass. The split is 768,100 + 129,600 at 1080p, 3,042,588 + 518,400 at 4K, and 11,932,732 + 2,073,600 at 8K. Against the Phase 2A int16 coefficient copy that is 9.2×, 9.3×, and 9.5× less traffic. Against the Phase 2A compact int8 stream it is about 4.7× less. The container outside those payloads is 2,696 bytes at all three sizes. Encode rate on the canonical frame is 1.74, 1.72, and 1.69 bits per wavelet coefficient.

Device-input speedup against upstream default, same process and frame counts:

| Resolution | Latency | FPS |
| :--- | ---: | ---: |
| 1080p | 2.24× | 2.24× |
| 4K | 5.91× | 5.91× |
| 8K | 8.31× | 8.31× |

Host-input one-stream FPS is 87.0, 40.6, and 14.7 (2.06×, 3.83×, and 5.43× upstream). The host window still uploads the BGRA frame: 8.3 MB, 33.2 MB, and 132.7 MB. In this run that upload measured 1.38 ms at 1080p, 5.36 ms at 4K, and 20.38 ms at 8K. The device window measures 0 H2D bytes.

Phase 2A's sustained FPS stays higher because it keeps two frames in flight. At 1080p, 16 CPU workers also finish entropy in 4.49 ms, ahead of the 8.97 ms GPU entropy. Phase 2B encodes one frame at a time. Its device latency is the shorter one at 4K (15.97 ms versus 21.78 ms) and at 8K (44.53 ms versus 80.94 ms). Sustained FPS at 8K is close: 22.46 for Phase 2B device, 23.23 for Phase 2A device resident.

## 5. Streams

Each configuration was byte-identical to `EncodeQuantizedGroup`. One stream finishes a band before launching the next. Several streams keep that many bands in flight, each with its own workspace, up to eight. Batched queues a wave of four bands on one stream and synchronizes once per wave.

| Resolution | Dispatch | FPS | Latency ms |
| :--- | :--- | ---: | ---: |
| 1080p host | 1 stream | 86.99 | 11.50 |
| 1080p host | 8 streams | 90.31 | 11.07 |
| 1080p device | 1 stream | 94.83 | 10.55 |
| 1080p device | 4 streams | 94.36 | 10.60 |
| 4K host | 1 stream | 40.62 | 24.62 |
| 4K host | 8 streams | 46.59 | 21.46 |
| 4K device | 1 stream | 62.60 | 15.97 |
| 4K device | 4 streams | 62.55 | 15.99 |
| 8K host | 1 stream | 14.68 | 68.12 |
| 8K host | 2 streams | 15.42 | 64.84 |
| 8K device | 1 stream | 22.46 | 44.53 |
| 8K device | 4 streams | 22.69 | 44.08 |

On the device-resident path one stream and four streams land on the same FPS. Eight streams were not faster there, and they cost eight full-size workspaces. The experimental default stays one stream. One 4K host row with four streams dropped to 19.4 fps because that window's upload measured 19.7 ms; the other 4K host rows are 40–47 fps, so that row is a stall, not a scheduling result. Batched dispatch tracks the multi-stream rows and does not win a resolution.

## 6. What is still on the CPU

8K device, one stream, inside the 44.53 ms frame:

| Piece | ms |
| :--- | ---: |
| Color | 0.55 |
| H2D in the timed window | 0.003 |
| Wavelet and quant | 1.19 |
| Entropy, of which the scan is 18.08 | 33.87 |
| Lowpass encode inside that entropy time | 1.40 |
| Band headers and trailers inside that entropy time | 2.30 |
| Container assembly | 1.03 |

CPU use for that run was about 4.9 cores, almost all of it the verify frame and the driver, not a 16-worker entropy pool. Headers, lowpass, and assembly together are about 5 ms. That is not worth a CUDA port. The long piece is the GPU scan, then the fact that the next frame's color and wavelet wait until this frame's entropy finishes.

## 7. Phase 2C, not started

Two follow-ups are justified by these numbers, and neither is in this tree:

1. Overlap the next frame's color and wavelet with this frame's entropy, the way Phase 2A overlapped CPU entropy. That is the throughput gap at 1080p and 4K.
2. Tighten the zero-run scan. It is about half of 8K GPU entropy. Codebook, prefix sum, and bit packing are already small.

Leave the container, the band tags, and the lowpass writer on the CPU.
