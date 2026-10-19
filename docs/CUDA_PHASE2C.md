# HyperCine Phase 2C: Pipelined GPU Entropy Encoder

> [!NOTE]
> **Historical Phase 2C research snapshot.** Performance, test status and terminology below are retained as measured at the time; they are not the current release gate. Current benchmark source: [Phase 3A.2c](../benchmarks/phase3a2c_release_candidate.json). Decoder reproducibility remains under separate [investigation #120](https://github.com/louis-0hrs/HyperCine/issues/120).

Phase 2C combines **Phase 2A's multi-frame overlap throughput** with **Phase 2B's GPU entropy encoder and compressed coefficient D2H transfer** into a unified, production-oriented CUDA pipeline.

The pristine [`upstream/`](../upstream) tree remains 100% untouched (`git status --porcelain upstream/` is empty). All measurements below come from [`benchmarks/results/phase2c.json`](../benchmarks/results/phase2c.json), produced by [`benchmarks/benchmark_phase2c.cpp`](../benchmarks/benchmark_phase2c.cpp) and verified by [`tests/test_phase2c.cpp`](../tests/test_phase2c.cpp).

---

## Hardware & Canonical Environment

- **GPU**: NVIDIA GeForce RTX 4090 Laptop GPU (`sm_89`, 76 SMs, 16,375.5 MB VRAM, `AsyncEngines = 1`, PCI Bus 1 / Device 0)
- **CUDA Driver / Runtime**: Driver 13.0 (`13030`) / Runtime 12.8 (`12080`)
- **PCIe Link**: PCIe Gen4 (negotiated x8 / max x16; canonical uncontended pinned bandwidth **13.38 GB/s H2D**, **13.14 GB/s D2H**)
- **Canonical Input**: Deterministic synthetic test pattern (seed `0x1337BEEF`), BGRA 8-bit input → 10-bit YUV 4:2:2 internal (`CFHD_ENCODED_FORMAT_YUV_422`), FilmScan 3 (`CFHD_ENCODING_QUALITY_FILMSCAN3`), GOP 1, progressive, 3 channels (`Y`, `C1=Cr`, `C2=Cb`), 3 wavelet levels (30 subbands total).

---

## 1. Exact Phase 2B / 2C Subband Entropy Coverage Audit

In CineForm's 3-channel, 3-level wavelet pyramid (30 subbands total):
- **27 Highpass Subbands** (Levels 1, 2, 3 `LH`, `HL`, `HH` across channels `Y`, `C1`, `C2`, subbands 1..9): Entropy-encoded on GPU (`EncodeHighpassOnGpu` in Phase 2B; `QueueGpuEntropyFrame` in Phase 2C) using CineForm's signed zero-run + magnitude VLC (`table17`, `cs9`..`cs15`) and transferred D2H as **compressed 32-bit big-endian VLC words**.
- **3 Level-3 Lowpass Subbands** (`Y3LL`, `C13LL`, `C23LL`, subband 0): In the SMPTE VC-5 / CineForm specification, lowpass bands are **never VLC or RLE compressed**. Upstream's `EncodeQuant16s` writes each quantized `int16_t` coefficient directly as an uncompressed 16-bit big-endian integer between the subband header and trailer (`0x00394ADE`).

### Meaning of *"compressed download plus the lowpass bands"*
The phrase means that for each frame, the D2H PCIe transfer consists of:
1. The **27 highpass subbands** downloaded as **compressed VLC bitstreams** (`768,100 B` at 1080p, `3,042,588 B` at 4K, `11,932,732 B` at 8K), **plus**
2. The **3 level-3 lowpass (`LL`) subbands** downloaded as **raw `int16_t` coefficients** (`129,600 B` at 1080p, `518,400 B` at 4K, `2,073,600 B` at 8K — exactly $1/64$ of the frame area per channel), which are then byte-swapped into big-endian `EncodeQuant16s` band payloads on the CPU.

### Summary of Lowpass vs Highpass D2H & CPU Cost

| Metric | 1080p (`1920×1080`) | 4K UHD (`3840×2160`) | 8K UHD (`7680×4320`) |
| :--- | ---: | ---: | ---: |
| `Y3LL` Dimensions & Raw D2H Bytes | `240×135` (`64,800 B`) | `480×270` (`259,200 B`) | `960×540` (`1,036,800 B`) |
| `C13LL` Dimensions & Raw D2H Bytes | `120×135` (`32,400 B`) | `240×270` (`129,600 B`) | `480×540` (`518,400 B`) |
| `C23LL` Dimensions & Raw D2H Bytes | `120×135` (`32,400 B`) | `240×270` (`129,600 B`) | `480×540` (`518,400 B`) |
| **Total Lowpass Raw D2H Bytes (3 bands)** | **129,600 B** (0.130 MB) | **518,400 B** (0.518 MB) | **2,073,600 B** (2.074 MB) |
| **Total Highpass Compressed D2H Bytes (27 bands)** | **768,100 B** (0.768 MB) | **3,042,588 B** (3.043 MB) | **11,932,732 B** (11.933 MB) |
| **Total Frame D2H Bytes** | **897,700 B** (0.898 MB) | **3,560,988 B** (3.561 MB) | **14,006,332 B** (14.006 MB) |
| Raw `int16` Equivalent D2H Bytes (30 bands) | 8,294,400 B (8.294 MB) | 33,177,600 B (33.178 MB) | 132,710,400 B (132.710 MB) |
| **D2H Compression Ratio vs Raw `int16`** | **9.24×** | **9.32×** | **9.48×** |
| Lowpass CPU `BuildLowpassPayload` Time | **0.143 ms** | **0.300 ms** | **0.971 ms** |
| Lowpass % of Single-Frame Total Time | **0.53%** | **1.29%** | **1.71%** |

### All 30 Canonical Subbands (1080p, 4K UHD, 8K UHD)

| Idx | Band | Ch | Lvl | Sub | GPU Entropy | CPU Pack | 1080p Dim | 1080p Raw D2H | 1080p Comp D2H | 4K Dim | 4K Raw D2H | 4K Comp D2H | 8K Dim | 8K Raw D2H | 8K Comp D2H |
| --: | :--- | -: | --: | --: | :---: | :---: | :---: | ---: | ---: | :---: | ---: | ---: | :---: | ---: | ---: |
| 0 | `Y3LL` | 0 | 3 | 0 | No | Yes (`int16` BE) | `240×135` | 64,800 B | 0 B | `480×270` | 259,200 B | 0 B | `960×540` | 1,036,800 B | 0 B |
| 1 | `Y3LH` | 0 | 3 | 1 | Yes | Header/Trailer | `240×135` | 0 B | 3,692 B | `480×270` | 0 B | 14,372 B | `960×540` | 0 B | 55,444 B |
| 2 | `Y3HL` | 0 | 3 | 2 | Yes | Header/Trailer | `240×135` | 0 B | 4,356 B | `480×270` | 0 B | 24,828 B | `960×540` | 0 B | 93,752 B |
| 3 | `Y3HH` | 0 | 3 | 3 | Yes | Header/Trailer | `240×135` | 0 B | 7,092 B | `480×270` | 0 B | 28,264 B | `960×540` | 0 B | 112,284 B |
| 4 | `Y2LH` | 0 | 2 | 4 | Yes | Header/Trailer | `480×270` | 0 B | 26,372 B | `960×540` | 0 B | 104,640 B | `1920×1080` | 0 B | 416,544 B |
| 5 | `Y2HL` | 0 | 2 | 5 | Yes | Header/Trailer | `480×270` | 0 B | 30,252 B | `960×540` | 0 B | 117,396 B | `1920×1080` | 0 B | 438,536 B |
| 6 | `Y2HH` | 0 | 2 | 6 | Yes | Header/Trailer | `480×270` | 0 B | 38,228 B | `960×540` | 0 B | 152,684 B | `1920×1080` | 0 B | 609,652 B |
| 7 | `Y1LH` | 0 | 1 | 7 | Yes | Header/Trailer | `960×540` | 0 B | 113,348 B | `1920×1080` | 0 B | 451,948 B | `3840×2160` | 0 B | 1,808,628 B |
| 8 | `Y1HL` | 0 | 1 | 8 | Yes | Header/Trailer | `960×540` | 0 B | 119,396 B | `1920×1080` | 0 B | 464,072 B | `3840×2160` | 0 B | 1,829,800 B |
| 9 | `Y1HH` | 0 | 1 | 9 | Yes | Header/Trailer | `960×540` | 0 B | 81,720 B | `1920×1080` | 0 B | 326,604 B | `3840×2160` | 0 B | 1,305,668 B |
| 10 | `C13LL` | 1 | 3 | 0 | No | Yes (`int16` BE) | `120×135` | 32,400 B | 0 B | `240×270` | 129,600 B | 0 B | `480×540` | 518,400 B | 0 B |
| 11 | `C13LH` | 1 | 3 | 1 | Yes | Header/Trailer | `120×135` | 0 B | 3,116 B | `240×270` | 0 B | 12,360 B | `480×540` | 0 B | 48,488 B |
| 12 | `C13HL` | 1 | 3 | 2 | Yes | Header/Trailer | `120×135` | 0 B | 3,164 B | `240×270` | 0 B | 12,728 B | `480×540` | 0 B | 49,900 B |
| 13 | `C13HH` | 1 | 3 | 3 | Yes | Header/Trailer | `120×135` | 0 B | 4,532 B | `240×270` | 0 B | 18,116 B | `480×540` | 0 B | 72,284 B |
| 14 | `C12LH` | 1 | 2 | 4 | Yes | Header/Trailer | `240×270` | 0 B | 17,464 B | `480×540` | 0 B | 68,888 B | `960×1080` | 0 B | 275,156 B |
| 15 | `C12HL` | 1 | 2 | 5 | Yes | Header/Trailer | `240×270` | 0 B | 17,332 B | `480×540` | 0 B | 69,212 B | `960×1080` | 0 B | 275,272 B |
| 16 | `C12HH` | 1 | 2 | 6 | Yes | Header/Trailer | `240×270` | 0 B | 25,416 B | `480×540` | 0 B | 101,344 B | `960×1080` | 0 B | 405,708 B |
| 17 | `C11LH` | 1 | 1 | 7 | Yes | Header/Trailer | `480×540` | 0 B | 75,820 B | `960×1080` | 0 B | 302,892 B | `1920×2160` | 0 B | 1,210,844 B |
| 18 | `C11HL` | 1 | 1 | 8 | Yes | Header/Trailer | `480×540` | 0 B | 75,836 B | `960×1080` | 0 B | 303,280 B | `1920×2160` | 0 B | 1,210,276 B |
| 19 | `C11HH` | 1 | 1 | 9 | Yes | Header/Trailer | `480×540` | 0 B | 53,880 B | `960×1080` | 0 B | 215,404 B | `1920×2160` | 0 B | 861,568 B |
| 20 | `C23LL` | 2 | 3 | 0 | No | Yes (`int16` BE) | `120×135` | 32,400 B | 0 B | `240×270` | 129,600 B | 0 B | `480×540` | 518,400 B | 0 B |
| 21 | `C23LH` | 2 | 3 | 1 | Yes | Header/Trailer | `120×135` | 0 B | 132 B | `240×270` | 0 B | 4,420 B | `480×540` | 0 B | 23,800 B |
| 22 | `C23HL` | 2 | 3 | 2 | Yes | Header/Trailer | `120×135` | 0 B | 180 B | `240×270` | 0 B | 2,556 B | `480×540` | 0 B | 8,568 B |
| 23 | `C23HH` | 2 | 3 | 3 | Yes | Header/Trailer | `120×135` | 0 B | 1,188 B | `240×270` | 0 B | 4,432 B | `480×540` | 0 B | 17,380 B |
| 24 | `C22LH` | 2 | 2 | 4 | Yes | Header/Trailer | `240×270` | 0 B | 7,996 B | `480×540` | 0 B | 37,500 B | `960×1080` | 0 B | 102,000 B |
| 25 | `C22HL` | 2 | 2 | 5 | Yes | Header/Trailer | `240×270` | 0 B | 5,972 B | `480×540` | 0 B | 21,912 B | `960×1080` | 0 B | 61,904 B |
| 26 | `C22HH` | 2 | 2 | 6 | Yes | Header/Trailer | `240×270` | 0 B | 9,420 B | `480×540` | 0 B | 45,520 B | `960×1080` | 0 B | 163,552 B |
| 27 | `C21LH` | 2 | 1 | 7 | Yes | Header/Trailer | `480×540` | 0 B | 22,900 B | `960×1080` | 0 B | 70,904 B | `1920×2160` | 0 B | 237,260 B |
| 28 | `C21HL` | 2 | 1 | 8 | Yes | Header/Trailer | `480×540` | 0 B | 17,484 B | `960×1080` | 0 B | 58,812 B | `1920×2160` | 0 B | 209,524 B |
| 29 | `C21HH` | 2 | 1 | 9 | Yes | Header/Trailer | `480×540` | 0 B | 1,812 B | `960×1080` | 0 B | 7,500 B | `1920×2160` | 0 B | 28,940 B |

---

## 2. Explicit 5-Level Bitstream Identity & SHA-256 Verification

To make sample bitstreams 100% deterministic across independent encoder instances, `EncoderContext::Init` normalizes the four volatile metadata tags (`TAG_CLIP_GUID`, `TAG_ENCODE_DATE`, `TAG_ENCODE_TIME`, `TAG_TIMECODE`) to fixed canonical constants.

Across 1080p, 4K UHD, and 8K UHD, `VerifyPhase2CIdentity` verifies all five levels separately:
1. **Pre-entropy quantized coefficient equality** (`pre_entropy_coeff_equal = true`, 0 mismatches across all 30 bands)
2. **Per-band encoded payload byte equality** (`per_band_payload_byte_equal = true`, CPU vs GPU SHA-256 identical on all 30 bands)
3. **Per-band payload length equality** (`per_band_payload_len_equal = true`, exact byte length match on all 30 bands)
4. **Complete frame / sample byte equality** (`complete_sample_byte_equal = true` across `FRAME_COMPRESSED_COPY`, `BAND_COMPRESSED_STREAM`, 1/2/3/4/6/8 streams, and GPU lowpass packing)
5. **Upstream `CFHD_DecodeSample` pixel equality** (`upstream_decoder_pixel_equal = true`, `max_diff = 0`, identical decoded BGRA SHA-256)

### Sample-Level & Decoded Pixel SHA-256 Hashes

| Resolution | Sample Bytes | PSNR vs Source | CPU Sample SHA-256 | GPU Sample SHA-256 (Consolidated, Streamed, 1..8 Streams, GPU LL) | Upstream Decoded Pixels SHA-256 |
| :--- | ---: | ---: | :--- | :--- | :--- |
| **1080p** | `900,396 B` | `44.466 dB` | `2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52` | `2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52` | `c5e0e052be84a807a899d0501fa73f93bbe9d67e0f1c9e5cfdef97a3ffee54a0` |
| **4K UHD** | `3,563,684 B` | `44.554 dB` | `f27b313c6ad7ece0bb04996694920426bc1d673a570a9652cdaac21e3fdecf8a` | `f27b313c6ad7ece0bb04996694920426bc1d673a570a9652cdaac21e3fdecf8a` | `f6e4a4dde2c2e0695c8a575ef32c1e1b239912fe37254be8364ad6b130878ae9` |
| **8K UHD** | `14,009,028 B` | `44.582 dB` | `a14bf4ec63b82aabd3f5314bd084481ca06d6944f986e9f21295df28f5886ae2` | `a14bf4ec63b82aabd3f5314bd084481ca06d6944f986e9f21295df28f5886ae2` | `91373883fc5361188724dc8c72e37f58a7cf48bf1823405791bb6a4acd314bbd` |

*(All 90 per-band coefficient and payload SHA-256 digests across 1080p, 4K, and 8K are recorded in [`benchmarks/results/phase2c.json`](../benchmarks/results/phase2c.json#L1117-L1403).)*

---

## 3. Multi-Frame GPU-Entropy Pipeline Architecture

Phase 2C replaces Phase 2B's single-frame synchronous execution with an asynchronous **N-slot ring pipeline** (default 4 slots at 1080p/4K, configurable 1..8 slots) coordinated strictly via CUDA streams and non-blocking events (`cudaStreamWaitEvent`, `cudaEventQuery`) without any `cudaDeviceSynchronize()` in the encode loop:

- **Stage 1 (Frame $N+2$)**: Host-to-device BGRA DMA (`frontend_stream`, skipped for device-resident inputs) + GPU BGRA-to-YUV422 color conversion (`color_done_ev`).
- **Stage 2 (Frame $N+1$)**: Wavelet stream waits on `color_done_ev`, runs 3-level 2D wavelet transform + highpass quantization (`wavelet_done_ev`), and triggers async lowpass D2H copy on `copy_stream` (`lowpass_d2h_done_ev`).
- **Stage 3 (Frame $N$)**: `GpuEntropyFrameSlot`'s worker streams (`1..8` streams) wait on `wavelet_done_ev` and encode all 27 highpass subbands in **Longest-Processing-Time-First (LPT)** order (Level 1 `Y` → Level 1 Chroma → Level 2 → Level 3) with **zero host synchronization**, followed by device-side layout computation (`ComputeFrameLayoutKernel`), single-buffer consolidation (`ConsolidateFrameWordsKernel`), and async compressed D2H (`frame_d2h_done_ev`). To prevent a new frame's wavelet pass from starving `consolidate_stream` on the GPU scheduler, the next frame waiting on the same slot executes `cudaStreamWaitEvent(wavelet_stream, prev_entropy_done_ev, 0)` on the device.
- **Stage 4 (Frame $N-1$)**: A background CPU assembly worker thread polls `cudaEventQuery(frame_d2h_done_ev)` and `cudaEventQuery(lowpass_d2h_done_ev)`, reads the zero-copy mapped band lengths (`h_band_offsets`), wraps the 27 compressed highpass bands and 3 lowpass bands in CineForm headers/trailers directly from pinned memory (`BuildHighpassPayloadPtr`, `BuildLowpassPayloadPtr`), and concatenates the final `CFHD` sample bitstream.

---

## 4. Concurrency Inside GPU Entropy

### Why Phase 2B Entropy Was Slower Under Multi-Stream
In Phase 2B (`EncodeHighpassOnGpu`), every single band required:
1. Seven small kernel launches,
2. A synchronous `cudaMemcpyAsync` of the 4-byte scalar `d_scalars[1]` (total bits),
3. A **blocking `cudaStreamSynchronize()` on the host** to learn the compressed word count `words_used`, and
4. A second per-band `cudaMemcpyAsync` + `cudaStreamSynchronize()` to copy the compressed words.

This resulted in **54 host-blocking stream synchronizations and 54 tiny DMA transfers per frame**. When Phase 2B used 4 streams, host driver lock contention and WDDM launch queue serialization made 4 streams slower than 1 stream at 1080p (`7.07 ms` vs `6.89 ms`) and 4K (`9.20 ms` vs `8.03 ms`).

### How Phase 2C Eliminated Host Sync Inside GPU Entropy
Phase 2C (`QueueGpuEntropyFrame`) keeps all per-band state in pre-allocated device arenas (`d_coeff_arena`, `d_RL_arena`, `d_pack_arena`) and writes each band's `total_bits` into `d_scalars_all[band * 2 + 1]`. Once all 27 bands finish on their worker streams (`band_pack_done_ev[b]`), `ComputeFrameLayoutKernel` computes all 27 word counts and prefix offsets on the GPU and writes them to mapped pinned memory (`h_band_offsets`), while `ConsolidateFrameWordsKernel` packs all 27 compressed bitstreams into a single contiguous buffer (`d_consolidated_words`) for one async DMA transfer.

### Measured GPU Entropy Concurrency Sweep (`1, 2, 3, 4, 6, 8` Streams)

| Mode / Streams | 1080p Wall (ms) | 1080p GPU Ent (ms) | 1080p Speedup | 4K Wall (ms) | 4K GPU Ent (ms) | 4K Speedup | 8K Wall (ms) | 8K GPU Ent (ms) | 8K Speedup |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `Phase2B_OneStream_SyncPerBand` (1s) | 6.886 | 6.886 | 0.62× | 8.034 | 8.034 | 0.74× | 18.512 | 18.512 | 0.60× |
| `Phase2B_Batched_WaveSync` (4s) | 6.884 | 6.884 | 0.62× | 9.458 | 9.458 | 0.62× | 16.930 | 16.930 | 0.65× |
| `Phase2B_MultiStream4_SyncPerBand` (4s) | 7.073 | 7.073 | 0.60× | 9.201 | 9.201 | 0.64× | 15.856 | 15.856 | 0.70× |
| **`Phase2C_Consolidated_1s` (Baseline)** | **4.272** | **4.157** | **1.00×** | **5.902** | **5.608** | **1.00×** | **11.075** | **10.033** | **1.00×** |
| **`Phase2C_Consolidated_2s`** | **4.119** | **4.007** | **1.04×** | **5.146** | **4.855** | **1.15×** | **10.815** | **9.771** | **1.02×** |
| **`Phase2C_Consolidated_3s`** | **4.191** | **4.078** | **1.02×** | **4.795** | **4.515** | **1.23×** | **10.010** | **8.968** | **1.11×** |
| **`Phase2C_Consolidated_4s`** | **4.326** | **4.186** | **0.99×** | **5.332** | **5.029** | **1.11×** | **10.101** | **9.083** | **1.10×** |
| **`Phase2C_Consolidated_6s`** | **4.275** | **4.160** | **1.00×** | **4.759** | **4.414** | **1.24×** | **10.162** | **9.133** | **1.09×** |
| **`Phase2C_Consolidated_8s`** | **4.122** | **4.001** | **1.04×** | **4.758** | **4.463** | **1.24×** | **10.644** | **9.602** | **1.04×** |
| `Phase2C_BandStreamed_1s` | 5.062 | 4.133 | 0.84× | 5.764 | 4.827 | 1.02× | 9.883 | 9.817 | 1.12× |
| `Phase2C_BandStreamed_4s` | 4.939 | 4.082 | 0.87× | 5.593 | 4.741 | 1.06× | 9.600 | 9.115 | 1.15× |

- **Occupancy**: `cudaOccupancyMaxActiveBlocksPerMultiprocessor` reports **6 active blocks/SM** (256 threads/block = 1,536 threads/SM = 100% theoretical occupancy on `sm_89`) for the core scan/tokenize/pack kernels.
- **Why 3–4 streams is the sweet spot**: Level-2 and Level-3 bands (`120×135` to `480×270`, 64–507 thread blocks) do not fill all 76 SMs individually (capacity 456 blocks), so running 3–4 streams concurrently with LPT ordering packs small chroma/level-3 kernels alongside tail blocks of larger bands, yielding a **1.23–1.24× speedup over 1 stream at 4K** (`4.76 ms` vs `5.90 ms`) and **1.68–1.85× speedup over Phase 2B** (`4.76 ms` vs `8.03 ms` at 4K; `10.01 ms` vs `18.51 ms` at 8K). Beyond 4 streams at 8K, L2 cache and global memory bandwidth contention across large Level-1 bands (`3840×2160` = 32,400 blocks) slightly degrades throughput.

---

## 5. Pipelined Compressed D2H Transfers: `FRAME_COMPRESSED_COPY` vs `BAND_COMPRESSED_STREAM`

Both compressed D2H strategies are implemented and verified bit-exact:
- **Mode A (`FRAME_COMPRESSED_COPY`)**: Consolidates all 27 compressed bands on device (`ConsolidateFrameWordsKernel`, costing just `0.037–0.062 ms`) and performs **one contiguous async D2H DMA transfer** (`0.071 ms` at 1080p, `0.269 ms` at 4K, `1.198 ms` at 8K).
- **Mode B (`BAND_COMPRESSED_STREAM`)**: Copies each band's compressed words asynchronously on `d2h_stream` as soon as its bit-pack kernel finishes and wraps headers/trailers on the CPU as each `band_d2h_done_ev[b]` completes.

| Resolution | Mode | Sustained FPS | Mean Latency (ms) | Median Latency (ms) | p95 Latency (ms) | Comp D2H (ms) | CPU Assembly (ms) |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **1080p** | **`A. FRAME_COMPRESSED_COPY` (Default)** | **222.54** | 13.71 | 13.58 | 17.72 | **0.071** | 0.927 |
| **1080p** | `B. BAND_COMPRESSED_STREAM` | 173.79 | **6.14** | **5.92** | **7.06** | 0.462 | **0.351** |
| **4K UHD** | **`A. FRAME_COMPRESSED_COPY` (Default)** | **181.16** | 16.48 | 17.27 | 19.82 | **0.269** | **1.208** |
| **4K UHD** | `BAND_COMPRESSED_STREAM` | 140.15 | **7.85** | **7.65** | **13.75** | 0.561 | 1.214 |
| **8K UHD** | **`A. FRAME_COMPRESSED_COPY` (Default)** | **70.59** | **40.50** | **36.98** | **69.74** | **1.198** | **3.236** |
| **8K UHD** | `BAND_COMPRESSED_STREAM` | 58.65 | 46.85 | 40.47 | 88.53 | 2.001 | 3.725 |

**Selection Rationale**: Because the RTX 4090 Laptop GPU has `AsyncEngines = 1`, queuing 27 separate small D2H DMA transfers per frame incurs 27 DMA engine setup transactions and host event queries, increasing D2H time by **1.7× to 6.5×** and reducing multi-frame sustained throughput by **20–28%**. Therefore, **`FRAME_COMPRESSED_COPY` is selected as the default production path**.

---

## 6. Frame-Slot Count Comparison (`1, 2, 3, 4, 6` Frames in Flight)

Measured on the device-input pipeline (`FRAME_COMPRESSED_COPY`, 4 entropy streams):

| Resolution | Slots | Sustained FPS | Frame Period (ms) | Mean Latency (ms) | Median Latency (ms) | p95 Latency (ms) | GPU Util (%) | CPU Util (%) | Avg Queued | Slot VRAM (MB) | Total VRAM (MB) |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **1080p** | 1 | 212.53 | 4.71 | **4.69** | **4.63** | **5.61** | 91.5% | 96.9% | 1.00 | 44.1 | 126.0 |
| **1080p** | 2 | 206.95 | 4.83 | 7.37 | 7.20 | 10.40 | 94.7% | 94.3% | 2.00 | 88.1 | 174.0 |
| **1080p** | **3** | **218.16** | **4.58** | 9.02 | 8.94 | 14.11 | 94.9% | 100.0% | 3.00 | 132.2 | 224.0 |
| **1080p** | 4 | 215.79 | 4.63 | 14.33 | 14.33 | 19.62 | 95.1% | 100.0% | 4.00 | 176.2 | 272.0 |
| **1080p** | 6 | 216.05 | 4.63 | 13.65 | 14.33 | 15.03 | 97.4% | 100.0% | 3.00 | 264.3 | 370.0 |
| **4K UHD** | 1 | 167.44 | 5.97 | **5.92** | **5.72** | **7.75** | 82.6% | 87.2% | 1.00 | 162.7 | 486.0 |
| **4K UHD** | 2 | 170.53 | 5.86 | 9.40 | 9.17 | 12.67 | 86.3% | 100.0% | 2.00 | 325.4 | 662.0 |
| **4K UHD** | 3 | 173.32 | 5.77 | 11.75 | 12.32 | 17.64 | 87.8% | 100.0% | 3.00 | 488.1 | 838.0 |
| **4K UHD** | **4** | **183.81** | **5.44** | 16.13 | 16.82 | 18.81 | 93.0% | 100.0% | 3.00 | 650.8 | 1,014.0 |
| **4K UHD** | 6 | 181.53 | 5.51 | 16.76 | 17.58 | 20.46 | 95.4% | 100.0% | 3.00 | 976.2 | 1,366.0 |
| **8K UHD** | 1 | 54.90 | 18.22 | **18.05** | **17.90** | **22.81** | 72.7% | 100.0% | 1.00 | 637.3 | 1,920.0 |
| **8K UHD** | **2** | **81.91** | **12.21** | 22.25 | 22.77 | 24.52 | **100.0%** | 100.0% | 1.90 | 1,274.6 | 2,604.0 |
| **8K UHD** | 3 | 70.75 | 14.14 | 32.20 | 30.05 | 59.40 | 88.2% | 100.0% | 2.70 | 1,911.9 | 3,288.0 |
| **8K UHD** | 4 | 69.84 | 14.32 | 40.98 | 35.41 | 74.30 | 86.3% | 100.0% | 3.85 | 2,549.3 | 3,972.0 |
| **8K UHD** | 6 | 73.76 | 13.56 | 55.80 | 42.11 | 86.83 | 91.1% | 100.0% | 4.87 | 3,823.9 | 5,340.0 |

- **1 Slot (Low-Latency Mode)**: Delivers **4.69 ms** latency at 1080p (212.5 FPS), **5.92 ms** at 4K (167.4 FPS), and **18.05 ms** at 8K (54.9 FPS).
- **2–4 Slots (Sustained Throughput Mode)**: 4 slots is optimal for 4K (**183.81 FPS**), 3–4 slots is optimal for 1080p (**218.16–222.54 FPS**), and **2 slots is optimal for 8K (81.91 FPS, 12.21 ms frame period, 22.25 ms latency, 2.6 GB VRAM)** because 2 slots overlaps CPU assembly with GPU execution without overflowing L2 cache or WDDM command queues across 637 MB per-slot arenas.

---

## 7. Canonical Standalone PCIe Benchmark Cleanup

During Phase 2A, initial runs recorded **~3.3 GB/s** due to Windows Controlled Folder Access interference, and a whitelisted rerun recorded **~6.6 GB/s**. In the canonical uncontended Phase 2C environment (9 timed runs per point across 4, 8, 16, 32, 64, and 128 MB, recorded in [`benchmarks/results/phase2c.json`](../benchmarks/results/phase2c.json#L14-L1095)), the PCIe Gen4 link achieves **~13.0–13.4 GB/s** across all pinned allocation types.

### Canonical 32 MB & 128 MB Standalone PCIe Transfer Summary (`runs = 9`)

| Transfer Size | Buffer Type | Direction | Mean (ms) | Median (ms) | p95 (ms) | Mean (GB/s) | Median (GB/s) | p95 (GB/s) |
| ---: | :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **32 MB** (4K BGRA) | `pageable` | `h2d_sync` | 2.694 | 2.615 | 3.176 | 12.50 | 12.83 | 12.85 |
| **32 MB** (4K BGRA) | `pageable` | `d2h_sync` | 3.157 | 2.726 | 4.998 | 11.06 | 12.31 | 12.62 |
| **32 MB** (4K BGRA) | `cudaMallocHost` | `h2d_async` | 2.522 | 2.510 | 2.568 | **13.30** | **13.37** | **13.41** |
| **32 MB** (4K BGRA) | `cudaMallocHost` | `d2h_async` | 2.584 | 2.553 | 2.836 | **13.00** | **13.14** | **13.16** |
| **32 MB** (4K BGRA) | `cudaHostAlloc` | `h2d_async` | 2.508 | 2.510 | 2.522 | **13.38** | **13.37** | **13.42** |
| **32 MB** (4K BGRA) | `cudaHostAlloc` | `d2h_async` | 2.774 | 2.554 | 4.510 | 12.49 | **13.14** | **13.15** |
| **32 MB** (4K BGRA) | `write_combined` | `h2d_async` | 2.512 | 2.506 | 2.541 | **13.36** | **13.39** | **13.42** |
| **32 MB** (4K BGRA) | `write_combined` | `d2h_async` | 2.556 | 2.552 | 2.566 | **13.13** | **13.15** | **13.16** |
| **32 MB** (4K BGRA) | `cudaMallocHost` | `bidirectional_async` | 5.259 | 5.087 | 6.403 | **12.83** | **13.19** | **13.29** |
| **128 MB** (8K BGRA) | `pageable` | `h2d_sync` | 10.571 | 10.362 | 11.644 | 12.72 | 12.95 | 13.01 |
| **128 MB** (8K BGRA) | `pageable` | `d2h_sync` | 10.871 | 10.558 | 12.063 | 12.38 | 12.71 | 12.78 |
| **128 MB** (8K BGRA) | `cudaMallocHost` | `h2d_async` | 10.568 | 10.058 | 14.159 | 12.85 | **13.34** | **13.38** |
| **128 MB** (8K BGRA) | `cudaMallocHost` | `d2h_async` | 10.478 | 10.221 | 12.053 | 12.84 | **13.13** | **13.16** |
| **128 MB** (8K BGRA) | `cudaHostAlloc` | `h2d_async` | 10.093 | 10.075 | 10.213 | **13.30** | **13.32** | **13.39** |
| **128 MB** (8K BGRA) | `cudaHostAlloc` | `d2h_async` | 10.556 | 10.245 | 12.952 | 12.78 | **13.10** | **13.14** |
| **128 MB** (8K BGRA) | `write_combined` | `h2d_async` | 10.029 | 10.016 | 10.159 | **13.38** | **13.40** | **13.42** |
| **128 MB** (8K BGRA) | `write_combined` | `d2h_async` | 10.289 | 10.220 | 10.865 | **13.05** | **13.13** | **13.16** |
| **128 MB** (8K BGRA) | `cudaMallocHost` | `bidirectional_async` | 20.965 | 20.386 | 24.174 | 12.84 | **13.17** | **13.24** |

Note: Because `AsyncEngines = 1`, `bidirectional_async` serializes H2D and D2H on the single copy engine, achieving the same ~13.2 GB/s aggregate rate as unidirectional DMA.

---

## 8 & 9. Complete Encoder Benchmark Comparison Across Modes

Comparison of all 5 required encoder architectures in the same process on the canonical FilmScan 3 corpus:

| Resolution | Mode | Sustained FPS | Speedup vs Upstream | Mean Latency (ms) | Median Latency (ms) | p95 Latency (ms) | H2D MB/f | D2H MB/f | GPU Time (ms) | CPU Time (ms) | Bitstream Bytes | PSNR (dB) | Byte Match |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| **1080p** | `UPSTREAM_DEFAULT` | 37.51 | 1.00× | 26.65 | 27.09 | 28.13 | 0.00 | 0.00 | 0.00 | 26.65 | 1,181,024 | 44.47 | Ref |
| **1080p** | `PHASE2A_DEVICE_PIPELINED` | 179.87 | 4.80× | 10.55 | — | — | 0.00 | 4.212 | 12.16 | 8.34 | 900,396 | 44.47 | Yes |
| **1080p** | `PHASE2B_DEVICE_SINGLE_STREAM` | 104.84 | 2.79× | **9.54** | — | — | 0.00 | **0.898** | 7.61 | 0.44 | 900,396 | 44.47 | Yes |
| **1080p** | **`PHASE2C_HOST_PIPELINED`** | **190.95** | **5.09×** | 15.37 | 15.94 | 17.31 | 8.294 | **0.898** | 5.15 | **0.33** | 900,396 | 44.47 | **Yes** |
| **1080p** | **`PHASE2C_DEVICE_PIPELINED`** | **222.54** | **5.93×** | 13.71 *(4.69 @ 1s)* | 13.58 | 17.72 | 0.00 | **0.898** | **4.37** | 0.93 | 900,396 | 44.47 | **Yes** |
| **4K UHD** | `UPSTREAM_DEFAULT` | 9.28 | 1.00× | 107.75 | 108.08 | 117.53 | 0.00 | 0.00 | 0.00 | 107.75 | 4,668,864 | 44.55 | Ref |
| **4K UHD** | `PHASE2A_DEVICE_PIPELINED` | 54.83 | 5.91× | 35.18 | — | — | 0.00 | 16.848 | 14.88 | 29.27 | 3,563,684 | 44.55 | Yes |
| **4K UHD** | `PHASE2B_DEVICE_SINGLE_STREAM` | 68.03 | 7.33× | 14.70 | — | — | 0.00 | **3.561** | 9.09 | 1.86 | 3,563,684 | 44.55 | Yes |
| **4K UHD** | **`PHASE2C_HOST_PIPELINED`** | **126.95** | **13.68×** | 23.98 | 24.13 | 35.95 | 33.178 | **3.561** | 9.82 | 1.61 | 3,563,684 | 44.55 | **Yes** |
| **4K UHD** | **`PHASE2C_DEVICE_PIPELINED`** | **181.16** *(183.81)* | **19.52×** | 16.48 *(5.92 @ 1s)* | 17.27 | 19.82 | 0.00 | **3.561** | **5.44** | **1.21** | 3,563,684 | 44.55 | **Yes** |
| **8K UHD** | `UPSTREAM_DEFAULT` | 2.38 | 1.00× | 419.98 | 424.82 | 432.66 | 0.00 | 0.00 | 0.00 | 419.98 | 18,393,596 | 44.58 | Ref |
| **8K UHD** | `PHASE2A_DEVICE_PIPELINED` | 11.26 | 4.73× | 172.52 | — | — | 0.00 | 67.392 | 80.39 | 142.57 | 14,009,028 | 44.58 | Yes |
| **8K UHD** | `PHASE2B_DEVICE_SINGLE_STREAM` | 22.74 | 9.55× | 43.97 | — | — | 0.00 | **14.006** | 28.94 | 6.15 | 14,009,028 | 44.58 | Yes |
| **8K UHD** | **`PHASE2C_HOST_PIPELINED`** | **56.41** | **23.69×** | 54.93 | 52.06 | 82.09 | 132.710 | **14.006** | 26.71 | 5.41 | 14,009,028 | 44.58 | **Yes** |
| **8K UHD** | **`PHASE2C_DEVICE_PIPELINED`** | **70.59** (**81.91 @ 2s**) | **29.65×** (**34.40×**) | **40.50** *(18.05 @ 1s)* | **36.98** | **69.74** | 0.00 | **14.006** | **13.54** | **3.24** | 14,009,028 | 44.58 | **Yes** |

---

## 10. Bottleneck Breakdown in the Device-Resident Pipeline

Per-frame stage breakdown for `PHASE2C_DEVICE_PIPELINED` (4 slots, 4 entropy streams):

| Stage | 1080p (ms) | 1080p (% of Period) | 4K UHD (ms) | 4K (% of Period) | 8K UHD (ms) | 8K (% of Period) |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **Effective Frame Period (`1000 / FPS`)** | **4.494 ms** | **100.0%** | **5.520 ms** | **100.0%** | **14.167 ms** *(12.21 @ 2s)* | **100.0%** |
| GPU BGRA → YUV422 Color | 0.050 ms | 1.1% | 0.146 ms | 2.6% | 0.599 ms | 4.2% |
| GPU 3-Level Wavelet + Quant | 0.400 ms | 8.9% | 0.440 ms | 8.0% | 1.232 ms | 8.7% |
| **GPU Highpass Entropy (27 bands wall)** | **3.839 ms** | **85.4%** | **4.574 ms** | **82.9%** | **10.432 ms** | **73.6%** |
| ↳ *Entropy Substage Sum: Block Scan* | *0.602 ms* | — | *0.974 ms* | — | *7.200 ms* | — |
| ↳ *Entropy Substage Sum: Merge Carry* | *0.702 ms* | — | *0.749 ms* | — | *6.682 ms* | — |
| ↳ *Entropy Substage Sum: Gather Pairs* | *0.614 ms* | — | *0.533 ms* | — | *4.775 ms* | — |
| ↳ *Entropy Substage Sum: Tokenize* | *1.458 ms* | — | *1.445 ms* | — | *10.557 ms* | — |
| ↳ *Entropy Substage Sum: Codebook* | *0.422 ms* | — | *0.523 ms* | — | *2.536 ms* | — |
| ↳ *Entropy Substage Sum: Prefix Sum* | *0.980 ms* | — | *0.963 ms* | — | *6.146 ms* | — |
| ↳ *Entropy Substage Sum: Bit Pack* | *0.563 ms* | — | *0.741 ms* | — | *4.080 ms* | — |
| GPU Layout + Consolidate | 0.037 ms | 0.8% | 0.053 ms | 1.0% | 0.062 ms | 0.4% |
| Compressed Highpass D2H DMA | 0.071 ms | 1.6% | 0.269 ms | 4.9% | 1.198 ms | 8.5% |
| Raw Lowpass D2H DMA (overlapped) | 0.277 ms | 6.2% | 0.200 ms | 3.6% | 0.557 ms | 3.9% |
| CPU Lowpass Pack (overlapped) | 0.214 ms | 4.8% | 0.182 ms | 3.3% | 0.506 ms | 3.6% |
| CPU Highpass Header Wrap (overlapped) | 0.635 ms | 14.1% | 0.619 ms | 11.2% | 1.485 ms | 10.5% |
| CPU Sample Concatenation (overlapped) | 0.079 ms | 1.8% | 0.407 ms | 7.4% | 1.245 ms | 8.8% |

**New Dominant Bottleneck**: With CPU assembly (`0.93 ms` at 1080p, `1.21 ms` at 4K, `3.24 ms` at 8K) and D2H DMA (`0.07 ms` / `0.27 ms` / `1.20 ms`) completely hidden behind multi-frame overlap, **GPU highpass entropy kernel execution (`3.84 ms` at 1080p, `4.57 ms` at 4K, `10.43 ms` at 8K — 74% to 85% of the frame period)** is now the undisputed bottleneck. Specifically, launching 7 separate passes × 27 bands = **189 kernel launches per frame** and writing/reading intermediate global memory arrays (`d_RL_counts`, `d_RL_vals`, `d_bit_OFF`, `d_bit_CNT`) dominates GPU time.

---

## 11. Lowpass GPU Packing vs CPU Packing Study

We implemented `Bswap16LowpassKernel` to byte-swap the 3 level-3 lowpass subbands into big-endian `uint16_t` words on the GPU prior to D2H transfer and compared it against CPU byte-swapping in `BuildLowpassPayloadPtr`:

| Resolution | Lowpass Mode | Sustained FPS | CPU Lowpass Time (ms) | GPU Wavelet+LL Time (ms) | Lowpass D2H Bytes | Bitstream SHA-256 Match |
| :--- | :--- | ---: | ---: | ---: | ---: | :---: |
| **1080p** | **CPU Pack (Default)** | **222.54** | 0.214 ms | 0.400 ms | 129,600 B | Yes |
| **1080p** | GPU Pack (`Bswap16LowpassKernel`) | 221.06 | 0.035 ms | 0.397 ms | 129,600 B | Yes |
| **4K UHD** | **CPU Pack (Default)** | **181.16** | 0.182 ms | 0.440 ms | 518,400 B | Yes |
| **4K UHD** | GPU Pack (`Bswap16LowpassKernel`) | 172.60 | 0.159 ms | 0.500 ms | 518,400 B | Yes |
| **8K UHD** | **CPU Pack (Default)** | **70.59** | 0.506 ms | 1.232 ms | 2,073,600 B | Yes |
| **8K UHD** | GPU Pack (`Bswap16LowpassKernel`) | 63.39 | 0.304 ms | 1.428 ms | 2,073,600 B | Yes |

**Finding**: Because CineForm lowpass subbands are uncompressed 16-bit words, byte-swapping on the GPU **does not reduce D2H byte volume** (`129.6 KB` / `518.4 KB` / `2.07 MB`). Furthermore, CPU lowpass packing already executes in `0.18–0.51 ms` on the background assembly thread in parallel with GPU entropy. Adding 3 extra GPU kernels slightly increases GPU stream overhead and reduces sustained throughput by 0.7%–10%. Thus, raw lowpass D2H + CPU big-endian packing remains optimal unless full sample bitstream assembly is moved to the GPU.

---

## 12. Recommendation for Phase 2D

Do **not** optimize CPU assembly or PCIe D2H further—both are now `< 10%` of the frame period and overlapped. **Phase 2D should target Fused GPU Entropy Kernels + CUDA Graph Capture**:
1. **Kernel Fusion inside GPU Entropy (189 launches → ~54 launches)**:
   - Fuse `GatherRunLengthPairsKernel` + `TokenizeAndCountVlcKernel` + `AppendCodebookEndKernel` into a single kernel that reads `d_RL_counts` / `d_RL_vals`, looks up the VLC LUT in shared/constant memory, and outputs both packed VLC code+len (`uint32_t`) and per-thread/per-warp bit counts.
   - Replace the 3-launch recursive `BlellochScanBlockKernel` hierarchy with a single-pass decoupled lookback / cub-style chained scan over bit counts, or fuse warp-level bit-offset scan directly into the bit-packing pass.
2. **CUDA Graph Launch (`cudaGraphLaunch`)**:
   - Because all 27 subband dimensions and arena pointers inside `GpuEntropyFrameSlot` are fixed for a given resolution, the entire 27-band multi-stream DAG (plus layout and consolidation) can be captured into a per-slot `cudaGraphExec_t`, eliminating ~3.5 ms of WDDM kernel launch overhead at 1080p and 4K.
