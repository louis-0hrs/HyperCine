# HyperCine Phase 2D — Upstream Bitstream Equivalence, Production Readiness & Controlled GPU Entropy Optimization

> [!WARNING]
> **Historical Phase 2D report, not a current production-safety approval.** The “complete”, “production readiness” and “12/12 CTest” statements below describe that *specific earlier snapshot*. Later multi-frame methodology changed the canonical benchmark; the frozen reference decoder is now known to produce nondeterministic pixels in some scenarios. Use the [Phase 3A.2c canonical JSON](../benchmarks/phase3a2c_release_candidate.json), [compatibility scope](COMPATIBILITY.md), and [decoder issue #120](https://github.com/louis-0hrs/HyperCine/issues/120) for the current boundaries.

**Status**: Complete (12/12 CTest suites passing; `upstream/` 100% untouched)  
**Target GPU**: NVIDIA GeForce RTX 4090 Laptop GPU (`sm_89`, 76 SMs, 16,375 MB VRAM, PCIe Gen4 x8, CUDA 12.8 / Driver 572.61)  
**Host CPU**: 16 Logical Processors (Windows x64, MSVC 19.44 Release)

---

## Executive Summary

Phase 2D resolves all correctness, benchmarking, and production-readiness questions discovered in the Phase 2C report, and executes three controlled, bit-exact GPU entropy optimization stages in strict order:

1. **Part 1 — Exact Resolution of the `3,563,684 B` vs `4,668,864 B` 4K Sample Size Discrepancy**:
   - The `1,105,180`-byte difference at 4K (`3,563,684 B` in the Phase 2C pipeline vs `4,668,864 B` in `UPSTREAM_DEFAULT`) is **not** a difference between HyperCine's entropy coder and upstream `CFHD_EncodeSample`.
   - It is caused entirely by upstream `QuantizationSetQuality` (`upstream/Codec/quantize.c:223-390`) adapting `q->FSratelimiter` across frames based on `encoder->lastgopbitcount >> 3`:
     - **Frame 1 (`Frame1Initial`, `lastgopbytes = 0`)**: `FSratelimiter = 4`, `scale = 12`. Finer spatial highpass subbands `8..16` (Level 1 LH/HL/HH across Y, C1, C2) receive quantizers `{6, 6, 9}` (Y) and `{6, 6, 12}` (C1/C2), producing **`3,563,684` bytes** at 4K (`900,396 B` at 1080p, `14,009,028 B` at 8K), a compression ratio of `24,883,200 / 3,563,684 = 6.982:1`.
     - **Frame 2+ (`SteadyState`, `lastgopbytes = 3,563,684`)**: Because `compression = 6.982 > 6.5`, `QuantizationSetQuality` decrements `FSratelimiter` by `1 (>4.5) + 1 (>5.5) + 2 (>6.5) = 4` in a single step (`4 -> 0`, `scale = 8`). Subbands `8..16` tighten to quantizers `{4, 4, 6}` (Y) and `{4, 4, 8}` (C1/C2), producing **`4,668,864` bytes** at 4K (`1,181,024 B` at 1080p, `18,393,596 B` at 8K), a compression ratio of `5.330:1` (clamped at `FSratelimiter = 0` for all subsequent frames).
   - In Phase 2C, `UPSTREAM_DEFAULT` called `CFHD_EncodeSample` in a loop (updating `encoder->lastgopbitcount` after Frame 1 and running timed frames at `FSratelimiter = 0` -> `4,668,864 B`), whereas the Phase 2C GPU pipeline seeded its quantizer tables from a single warm-up call (`FSratelimiter = 4` -> `3,563,684 B`).
   - When evaluated at the **same quantizer state** (`Frame1Initial` OR `SteadyState`), pristine upstream `CFHD_EncodeSample` (**A**), HyperCine CPU/reference (**B**), and HyperCine GPU (**C**) produce **100% identical compressed subband payloads across all 30 subbands**, identical total sample byte counts, **0 byte differences (`A == B == C`)** when assembled on the same encoder handle, **0 byte differences (`MaskCfhdVolatileMetadata(A) == MaskCfhdVolatileMetadata(B) == MaskCfhdVolatileMetadata(C)`)** across independent encoder handles, and **0 decoded pixel differences (`PSNR = 44.55 dB` at Frame 1, `44.68 dB` at SteadyState for 4K)**.
2. **Part 2 — Production Metadata Safety**:
   - `deterministic_test_metadata` now defaults to **`false`** in both `Phase2CConfig` and `Phase2DConfig`.
   - Production encodes preserve genuine upstream `TAG_CLIP_GUID`, `TAG_ENCODE_DATE`, `TAG_ENCODE_TIME`, and `TAG_TIMECODE` per clip.
   - `MaskCfhdVolatileMetadata()` zeros only those volatile metadata value fields for cross-handle regression comparisons while deterministic test mode remains opt-in (`deterministic_test_metadata = true`).
3. **Part 3 — CPU Utilization & Blocking Event Synchronization**:
   - Replaced `cudaEventQuery` busy-spin loops with `cudaEventCreateWithFlags(&ev, cudaEventBlockingSync)` + `cudaEventSynchronize` and condition-variable coordination (`Phase2DSyncMode::Blocking`).
   - Added full 16-core normalized CPU utilization (`normalized_process_cpu_pct`), single-core equivalent (`single_core_equiv_pct`), per-thread CPU utilization, running/blocked/spin time breakdown, and `CPU core-seconds / 60fps encoded video second`.
   - At 4K (3 slots, SteadyState), blocking synchronization increases throughput from `392.77 FPS` to **`425.82 FPS`** while cutting normalized CPU utilization in half (`8.5% -> 4.6%`, `spin_ms = 19.02 ms -> 0.00 ms`, `core_s/60s = 0.208 -> 0.104`).
4. **Parts 4 & 5 — Resolution-Aware & VRAM-Bounded Slot Policy + Dual D2H Modes**:
   - Implemented `SelectPhase2DFrameSlots()` supporting `DEFAULT_AUTO` (`1080p -> 2`, `4K -> 3`, `8K -> 2`), `LOW_LATENCY` (`1` slot), `MAX_THROUGHPUT` (`1080p -> 2`, `4K -> 3`, `8K -> 3`), and explicit user overrides, bounded by live `cudaMemGetInfo()` available VRAM.
   - Preserved both compressed D2H strategies: `FrameConsolidated` (single DMA per frame, default for `THROUGHPUT`) and `BandStreamed` (per-band event + DMA for `LOW_LATENCY` at 1080p/4K).
5. **Parts 6–9 — Controlled GPU Entropy Optimization (`2D-A -> 2D-B -> 2D-C`)**:
   - **Phase 2D-A (`CUDA_GRAPH`)**: Captures the 4-stream, 27-band LPT entropy + consolidation DAG into per-slot cached `cudaGraphExec_t` instances, collapsing **434 host kernel launch calls/frame into 1 `cudaGraphLaunch` call/frame** and accelerating 4K GPU entropy from `17.358 ms` to `3.034 ms` (**`5.72x`**).
   - **Phase 2D-B (`FUSED_GRAPH`)**: Fuses `GatherRuns + EmitTokens` (`GatherAndEmitTokensGpu`) and `VlcMap +pack_pass1` (`MapTokensToCodewordsGpu`), eliminating `run_idx`, `run_zeros`, `run_vals`, and `codewords` global-memory round-trips and reducing launches from **16 to 13 per band (434 -> 353 nodes/frame)**, accelerating 4K GPU entropy to `2.192 ms` (**`7.92x`**).
   - **Phase 2D-C (`FUSED_SCAN_OPTIMIZED`)**: Replaces the 2-pass `ScanBlockRunsGpu + ScanBlockCarriesGpu` with a single-launch warp-ballot + shared-memory lookback kernel (`ScanRunsSinglePassGpu`) and replaces the 5-kernel Blelloch bit-offset tree with a 2-kernel radix-1024 warp-prefix scan (`BitScanBlock1024Gpu` + `BitScanTopAndFinalizeGpu`), reducing launches from **13 to 9 per band (353 -> 245 nodes/frame)** and accelerating 4K GPU entropy to **`1.963 ms` (`8.84x`–`10.03x` vs `NO_GRAPH`)** while remaining 100% bit-identical (`B == C`).

---

## Part 1 — Exact Resolution of the `3,563,684 B` vs `4,668,864 B` Sample Size Discrepancy

### 1.1 Root Cause in Upstream `QuantizationSetQuality` (`upstream/Codec/quantize.c:223-390`)

In the pristine upstream GoPro CineForm SDK (`upstream/Codec/quantize.c`), `QuantizationSetQuality(..., newQuality = 6 /* FilmScan 3 */, progressive = 1, precision = 12, goplength = 1)` computes an adaptive highpass quantizer multiplier `scale` from `q->FSratelimiter`:

1. **Before Frame 1 is encoded (`encoder->lastgopbitcount == 0`)**:
   - `q->FSratelimiter` is initialized to **`4`** (`quantize.c:236`: `if (q->FSratelimiter == 0) q->FSratelimiter = 4;`).
   - Because `lastgopbytes = encoder->lastgopbitcount >> 3` is `0`, the rate-limiter branch (`if (lastgopbytes && q->FSratelimiter)`) is skipped on Frame 1.
   - At `FSratelimiter = 4`, `scale = (16 + q->FSratelimiter * 2) >> 1 = 12` (`quantize.c:361`).
   - Subbands `8..16` (`MAX_SUBBANDS = 17` table indices corresponding to finest spatial Level 1 highpass bands `Y1LH, Y1HL, Y1HH, C11LH, C11HL, C11HH, C21LH, C21HL, C21HH`) have base `Default_subband_quant[0]` values `{4, 4, 6, 4, 4, 8, 4, 4, 8}`, which are scaled by `12 >> 3`:
     $$\text{Luma Level 1 (LH, HL, HH)}: \{4, 4, 6\} \times \frac{12}{8} = \{6, 6, 9\}$$
     $$\text{Chroma Level 1 (LH, HL, HH)}: \{4, 4, 8\} \times \frac{12}{8} = \{6, 6, 12\}$$
   - Encoding the canonical 4K frame with these Frame 1 quantizers yields exactly **`3,563,684` bytes** (`28,509,472` bits).

2. **On Frame 2+ (`SteadyState`, after Frame 1 sets `encoder->lastgopbitcount = 28,509,472`)**:
   - `EncodeSample` calls `QuantizationSetQuality` at the start of Frame 2 with `lastgopbytes = 28,509,472 >> 3 = 3,563,684`.
   - `QuantizationSetQuality` computes:
     $$\text{rawsize} = \frac{3840 \times 2160 \times 3 \times 12}{8 \times 1.5} = 24,883,200\text{ B}, \quad \text{compression} = \frac{24,883,200}{3,563,684} = 6.9824$$
   - Because `compression > 4.5` (`-1`), `compression > 5.5` (`-1`), and `compression > 6.5` (`-2`), `q->FSratelimiter` is decremented by `4` in one step (`4 -> 0`).
   - At `FSratelimiter = 0`, `scale = (16 + 0) >> 1 = 8`, so subbands `8..16` use their unscaled base quantizers:
     $$\text{Luma Level 1 (LH, HL, HH)}: \{4, 4, 6\}, \quad \text{Chroma Level 1 (LH, HL, HH)}: \{4, 4, 8\}$$
   - Encoding the canonical 4K frame with these steady-state quantizers yields exactly **`4,668,864` bytes** (`compression = 5.330:1`), and `FSratelimiter` remains `0` for all subsequent frames (`Frame 2, 3, ... N`).

### 1.2 Exact Comparison of A (`CFHD_EncodeSample`), B (HyperCine CPU/Ref), and C (HyperCine GPU)

| Resolution | Quantizer State | `FSratelimiter` | **A** (`CFHD_EncodeSample`) | **B** (HyperCine CPU/Ref) | **C** (HyperCine GPU All 4 Modes) | `B == C` Unmasked | `A == B == C` Same Handle | `A == B == C` Masked (Indep. Handle) | All 30 Subbands `A == B == C` | Decoded Pixel Diff (`max_abs_diff`) | Decoded PSNR (dB) |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **1080p** (`1920x1080`) | `Frame1Initial` | `4` | `900,396 B` | `900,396 B` | `900,396 B` | **YES (0 diff)** | **YES (0 diff)** | **YES (`e39bf63e...`)** | **30 / 30 YES** | **`0`** | `44.47 dB` |
| **1080p** (`1920x1080`) | `SteadyState` | `0` | `1,181,024 B` | `1,181,024 B` | `1,181,024 B` | **YES (0 diff)** | **YES (0 diff)** | **YES (`711164e6...`)** | **30 / 30 YES** | **`0`** | `44.58 dB` |
| **4K UHD** (`3840x2160`) | `Frame1Initial` | `4` | `3,563,684 B` | `3,563,684 B` | `3,563,684 B` | **YES (0 diff)** | **YES (0 diff)** | **YES (`05b9fb42...`)** | **30 / 30 YES** | **`0`** | `44.55 dB` |
| **4K UHD** (`3840x2160`) | `SteadyState` | `0` | `4,668,864 B` | `4,668,864 B` | `4,668,864 B` | **YES (0 diff)** | **YES (0 diff)** | **YES (`619210eb...`)** | **30 / 30 YES** | **`0`** | `44.68 dB` |
| **8K UHD** (`7680x4320`) | `Frame1Initial` | `4` | `14,009,028 B` | `14,009,028 B` | `14,009,028 B` | **YES (0 diff)** | **YES (0 diff)** | **YES (`17a98f0c...`)** | **30 / 30 YES** | **`0`** | `44.58 dB` |
| **8K UHD** (`7680x4320`) | `SteadyState` | `0` | `18,393,596 B` | `18,393,596 B` | `18,393,596 B` | **YES (0 diff)** | **YES (0 diff)** | **YES (`ff5a0fac...`)** | **30 / 30 YES** | **`0`** | `44.70 dB` |

### 1.3 4K Subband-by-Subband Breakdown (`Frame1Initial` `3,563,684 B` vs `SteadyState` `4,668,864 B`)

All 21 coarser subbands (Levels 3 and 2 across Y, C1, C2) have **0 byte difference** between `Frame1Initial` and `SteadyState`. Only the 9 finest spatial Level 1 highpass subbands change quantizers when `FSratelimiter` steps from `4` to `0`, accounting for the exact `+1,105,180 B` delta (`3,563,684 + 1,105,180 = 4,668,864 B`):

| Idx | Subband | Dimensions | `Frame1` `(Q, Scale)` | `Frame1` Bytes | `SteadyState` `(Q, Scale)` | `SteadyState` Bytes | `Delta_B` | `F1 A==B==C` | `SS A==B==C` | `SteadyState` Subband SHA-256 (Prefix) |
| :-: | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :--- |
| 0 | `Y3LL` | `480x270` | `(1, 64)` | `259,260` | `(1, 64)` | `259,260` | `+0` | YES | YES | `72a18519be7d46fb...` |
| 1 | `Y3LH` | `480x270` | `(16, 32)` | `14,420` | `(16, 32)` | `14,420` | `+0` | YES | YES | `fa78d613e1483c11...` |
| 2 | `Y3HL` | `480x270` | `(16, 32)` | `24,876` | `(16, 32)` | `24,876` | `+0` | YES | YES | `06cbcd9c295d5328...` |
| 3 | `Y3HH` | `480x270` | `(8, 16)` | `28,312` | `(8, 16)` | `28,312` | `+0` | YES | YES | `089638efb4ff7c2b...` |
| 4 | `Y2LH` | `960x540` | `(4, 8)` | `104,688` | `(4, 8)` | `104,688` | `+0` | YES | YES | `758517163b3718b8...` |
| 5 | `Y2HL` | `960x540` | `(4, 8)` | `117,444` | `(4, 8)` | `117,444` | `+0` | YES | YES | `0d0e731f0f2bbf44...` |
| 6 | `Y2HH` | `960x540` | `(2, 4)` | `152,732` | `(2, 4)` | `152,732` | `+0` | YES | YES | `29803a67a6dd34a7...` |
| **7** | **`Y1LH`** | `1920x1080` | **`(6, 2)`** | **`451,996`** | **`(4, 2)`** | **`643,616`** | **`+191,620`** | **YES** | **YES** | `724ce8b3057ba045...` |
| **8** | **`Y1HL`** | `1920x1080` | **`(6, 2)`** | **`464,120`** | **`(4, 2)`** | **`658,768`** | **`+194,648`** | **YES** | **YES** | `6082ec8b2c261b8a...` |
| **9** | **`Y1HH`** | `1920x1080` | **`(9, 1)`** | **`326,652`** | **`(6, 1)`** | **`456,940`** | **`+130,288`** | **YES** | **YES** | `d231829eac0645c5...` |
| 10 | `C13LL` | `240x270` | `(1, 64)` | `129,660` | `(1, 64)` | `129,660` | `+0` | YES | YES | `a131414dab0e3695...` |
| 11 | `C13LH` | `240x270` | `(16, 32)` | `12,408` | `(16, 32)` | `12,408` | `+0` | YES | YES | `68d188d6c97aac65...` |
| 12 | `C13HL` | `240x270` | `(16, 32)` | `12,776` | `(16, 32)` | `12,776` | `+0` | YES | YES | `62de7941435329b9...` |
| 13 | `C13HH` | `240x270` | `(8, 16)` | `18,164` | `(8, 16)` | `18,164` | `+0` | YES | YES | `4b0d2e2917497fbc...` |
| 14 | `C12LH` | `480x540` | `(4, 8)` | `68,936` | `(4, 8)` | `68,936` | `+0` | YES | YES | `3e2d1f57d6f08895...` |
| 15 | `C12HL` | `480x540` | `(4, 8)` | `69,260` | `(4, 8)` | `69,260` | `+0` | YES | YES | `2d34e55f9fa209f2...` |
| 16 | `C12HH` | `480x540` | `(2, 4)` | `101,392` | `(2, 4)` | `101,392` | `+0` | YES | YES | `f053be18606ed36c...` |
| **17** | **`C11LH`** | `960x1080` | **`(6, 2)`** | **`302,940`** | **`(4, 2)`** | **`424,600`** | **`+121,660`** | **YES** | **YES** | `0b17b9b0eb1e353c...` |
| **18** | **`C11HL`** | `960x1080` | **`(6, 2)`** | **`303,328`** | **`(4, 2)`** | **`425,016`** | **`+121,688`** | **YES** | **YES** | `2260da6ab88e6cd9...` |
| **19** | **`C11HH`** | `960x1080` | **`(12, 1)`** | **`215,452`** | **`(8, 1)`** | **`290,592`** | **`+75,140`** | **YES** | **YES** | `e03e617e84d655c5...` |
| 20 | `C23LL` | `240x270` | `(1, 64)` | `129,660` | `(1, 64)` | `129,660` | `+0` | YES | YES | `aa39907dfdc5ffe3...` |
| 21 | `C23LH` | `240x270` | `(16, 32)` | `4,468` | `(16, 32)` | `4,468` | `+0` | YES | YES | `22d2cb0887ac95b6...` |
| 22 | `C23HL` | `240x270` | `(16, 32)` | `2,604` | `(16, 32)` | `2,604` | `+0` | YES | YES | `fc5b07e1eb35dff6...` |
| 23 | `C23HH` | `240x270` | `(8, 16)` | `4,480` | `(8, 16)` | `4,480` | `+0` | YES | YES | `988d960fdbded890...` |
| 24 | `C22LH` | `480x540` | `(4, 8)` | `37,548` | `(4, 8)` | `37,548` | `+0` | YES | YES | `d77a7a55ce0f6986...` |
| 25 | `C22HL` | `480x540` | `(4, 8)` | `21,960` | `(4, 8)` | `21,960` | `+0` | YES | YES | `2478605bc4c20717...` |
| 26 | `C22HH` | `480x540` | `(2, 4)` | `45,568` | `(2, 4)` | `45,568` | `+0` | YES | YES | `b18166c3eebc258b...` |
| **27** | **`C21LH`** | `960x1080` | **`(6, 2)`** | **`70,952`** | **`(4, 2)`** | **`182,628`** | **`+111,676`** | **YES** | **YES** | `cd1d6edc746a5971...` |
| **28** | **`C21HL`** | `960x1080` | **`(6, 2)`** | **`58,860`** | **`(4, 2)`** | **`170,480`** | **`+111,620`** | **YES** | **YES** | `80ac8f7c18cab300...` |
| **29** | **`C21HH`** | `960x1080` | **`(12, 1)`** | **`7,548`** | **`(8, 1)`** | **`54,388`** | **`+46,840`** | **YES** | **YES** | `674950bf3be40c51...` |

---

## Part 2 — Production Metadata Safety (`deterministic_test_metadata = false` by Default)

- **Default Production Behavior (`deterministic_test_metadata = false`)**:
  - `Phase2CConfig::deterministic_test_metadata` and `Phase2DConfig::deterministic_test_metadata` both default to **`false`**.
  - Normal encoding preserves upstream's real per-clip `TAG_CLIP_GUID`, `TAG_ENCODE_DATE`, `TAG_ENCODE_TIME`, and `TAG_TIMECODE` metadata untouched.
- **Test-Only Mode (`deterministic_test_metadata = true`)**:
  - Used only when a test or benchmark explicitly requests reproducible unmasked SHA-256 digests across separate runs.
- **Volatile Metadata Masking Helper (`MaskCfhdVolatileMetadata`)**:
  - Zeroes only the value bytes of `TAG_CLIP_GUID` (16 B), `TAG_ENCODE_DATE` (10 B), `TAG_ENCODE_TIME` (8 B), and `TAG_TIMECODE` (4 B) inside the first 4 KB sample header so independent encoder instances can be verified for bit-exactness without corrupting production metadata.
- **Verification (`VerifyPhase2DMetadataSafety`)**:
  - `clip1_guid = 9d940957-429e7eb3-67c1fab3-43308130` vs `clip2_guid = 66926763-47e219f3-b2fbb4a6-3f44ab76` (`normal_mode_guids_distinct = PASS`, `normal_mode_unmasked_distinct = PASS`, `normal_mode_masked_identical = PASS`, `deterministic_mode_unmasked_identical = PASS`).

---

## Part 3 — Normalized CPU Utilization & Blocking Synchronization

### 3.1 CPU Utilization Accounting

Phase 2C reported raw `100.0 * proc_cpu_s / wall_s`, which exceeded `100%` on a 16-logical-processor system when 1 polling thread + 1 worker thread were active (`~115%–158%` of a single core = `7.2%–9.9%` of the 16-thread CPU). Phase 2D reports `Phase2DCpuAccounting`:
- `logical_processors`: `16`
- `active_host_threads`: `2` (1 GPU scheduler/D2H thread + 1 CPU lowpass/sample assembly worker thread)
- `normalized_process_cpu_pct`: `0–100%` across all 16 logical processors
- `single_core_equiv_pct`: `100 * process_cpu_ms / wall_ms` (`100% = 1 full logical core`)
- `main_thread_single_core_pct` and `worker_thread_single_core_pct`
- `main_thread_running_ms`, `main_thread_blocked_ms`, `main_thread_spin_ms`
- `cpu_core_sec_per_60fps_video_sec`: CPU core-seconds consumed per 1 second (60 frames) of encoded video.

### 3.2 Polling (`cudaEventQuery`) vs Blocking (`cudaEventBlockingSync` + `cudaEventSynchronize` + `std::condition_variable`)

| Resolution | Sync Mode | Slots | Sustained FPS | Mean / Med / P95 Latency (ms) | Normalized CPU % (16 Logical) | Single-Core Equiv % | Main Thread Spin (ms) | Main Thread Blocked (ms) | CPU Core-Sec / 60fps Video Sec |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **1080p** | `POLLING_SPIN` | 2 | `1250.55` | `1.51 / 1.51 / 1.70` | `5.1%` | `81.4%` | `3.81 ms` | `0.00 ms` | `0.039` |
| **1080p** | `BLOCKING_EVENT` | 2 | `939.78` | `1.68 / 1.65 / 1.97` | **`3.8%`** | **`61.2%`** | **`0.00 ms`** | `12.85 ms` | **`0.039`** |
| **4K UHD** | `POLLING_SPIN` | 3 | `392.77` | `6.80 / 6.84 / 7.90` | `8.5%` | `136.4%` | `19.02 ms` | `0.00 ms` | `0.208` |
| **4K UHD** | `BLOCKING_EVENT` | 3 | **`425.82`** | **`5.46 / 5.41 / 5.91`** | **`4.6%`** | **`73.9%`** | **`0.00 ms`** | `31.79 ms` | **`0.104`** |
| **8K UHD** | `POLLING_SPIN` | 2 | `70.38` | `25.62 / 20.44 / 44.10` | `8.0%` | `128.3%` | `133.42 ms` | `0.00 ms` | `1.094` |
| **8K UHD** | `BLOCKING_EVENT` | 2 | **`72.30`** | **`17.62 / 14.29 / 31.74`** | **`2.4%`** | **`37.7%`** | **`0.00 ms`** | `141.65 ms` | **`0.312`** |

At 4K and 8K, replacing busy-polling with blocking synchronization eliminates `100%` of spin-wait CPU burn, reduces CPU core-seconds per 60fps video second by **`2.0x–3.5x`**, and **improves** sustained FPS and latency by eliminating OS thread contention with the CUDA driver and assembly worker thread.

---

## Part 4 — Resolution-Aware & VRAM-Bounded Slot Policy

`SelectPhase2DFrameSlots()` (`src/core/phase2d_encoder.cpp:894-972`) queries free VRAM via `cudaMemGetInfo`, reserves `session_workspace_vram + 256 MB`, bounds `vram_max_slots = clamp(usable_vram / per_slot_vram, 1, 8)`, and selects:

| Resolution | Policy | Target Slots | VRAM Max Slots (16 GB GPU) | Selected Slots | Per-Slot VRAM | Session Workspace VRAM | Est. Total VRAM | Clamped by 128 MB Override Test? |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **1080p** (`1920x1080`) | `DEFAULT_AUTO` | **2** | 8 | **2** | `44.1 MB` | `55.7 MB` | `143.8 MB` | Clamps to `1` |
| **1080p** (`1920x1080`) | `LOW_LATENCY` | **1** | 8 | **1** | `44.1 MB` | `55.7 MB` | `99.8 MB` | Clamps to `1` |
| **1080p** (`1920x1080`) | `MAX_THROUGHPUT` | **2** | 8 | **2** | `44.1 MB` | `55.7 MB` | `143.8 MB` | Clamps to `1` |
| **4K UHD** (`3840x2160`) | `DEFAULT_AUTO` | **3** | 8 | **3** | `162.7 MB` | `127.0 MB` | `615.0 MB` | Clamps to `1` |
| **4K UHD** (`3840x2160`) | `LOW_LATENCY` | **1** | 8 | **1** | `162.7 MB` | `127.0 MB` | `289.6 MB` | Clamps to `1` |
| **4K UHD** (`3840x2160`) | `MAX_THROUGHPUT` | **3** | 8 | **3** | `162.7 MB` | `127.0 MB` | `615.0 MB` | Clamps to `1` |
| **8K UHD** (`7680x4320`) | `DEFAULT_AUTO` | **2** | 8 | **2** | `637.3 MB` | `411.7 MB` | `1,686.3 MB` | Clamps to `1` |
| **8K UHD** (`7680x4320`) | `LOW_LATENCY` | **1** | 8 | **1** | `637.3 MB` | `411.7 MB` | `1,049.0 MB` | Clamps to `1` |
| **8K UHD** (`7680x4320`) | `MAX_THROUGHPUT` | **3** | 8 | **3** | `637.3 MB` | `411.7 MB` | `2,323.6 MB` | Clamps to `1` |

---

## Part 5 — Dual Compressed D2H Modes (`THROUGHPUT` vs `LOW_LATENCY`)

Both compressed D2H transport strategies are preserved and selectable via `Phase2DD2HPolicy`:
1. **`FrameConsolidated` (`THROUGHPUT` default)**: On-device `ConsolidateFramePayloadGpu` gathers all 27 compressed highpass band payloads into a contiguous device buffer and performs **1 `cudaMemcpyAsync` D2H DMA per frame** on the dedicated `d2h_stream`.
2. **`BandStreamed` (`LOW_LATENCY` for 1080p/4K; `FrameConsolidated` for 8K)**: Records per-band completion events and streams 27 individual compressed band D2H transfers so the host can wrap subband headers incrementally.

| Resolution | Configuration | D2H Strategy | Slots | Sustained FPS | Mean / Med / P95 Latency (ms) | GPU Entropy (ms) | CPU Assembly (ms) | Normalized CPU % |
| :--- | :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **1080p** | `D2H_THROUGHPUT_SLOTS1` | `FrameConsolidated` | 1 | **`851.54`** | **`1.16 / 1.14 / 1.33`** | `0.51` | `0.11` | `6.9%` |
| **1080p** | `D2H_LOW_LATENCY_SLOTS1` | `BandStreamed` | 1 | `445.18` | `2.22 / 2.21 / 2.51` | `0.50` | `0.15` | `7.2%` |
| **1080p** | `D2H_THROUGHPUT_AUTO_SLOTS` | `FrameConsolidated` | 2 | **`936.61`** | `1.71 / 1.68 / 1.87` | `0.51` | `0.17` | `3.8%` |
| **1080p** | `D2H_LOW_LATENCY_AUTO_SLOTS` | `BandStreamed` | 2 | `548.58` | `3.30 / 3.36 / 3.75` | `0.83` | `0.17` | `6.7%` |
| **4K UHD** | `D2H_THROUGHPUT_SLOTS1` | `FrameConsolidated` | 1 | **`282.92`** | **`3.49 / 3.48 / 3.70`** | `1.83` | `0.61` | `1.5%` |
| **4K UHD** | `D2H_LOW_LATENCY_SLOTS1` | `BandStreamed` | 1 | `205.39` | `4.81 / 4.78 / 5.31` | `2.36` | `0.66` | `4.5%` |
| **4K UHD** | `D2H_THROUGHPUT_AUTO_SLOTS` | `FrameConsolidated` | 3 | **`402.32`** | `6.04 / 6.17 / 6.62` | `2.32` | `0.95` | `6.5%` |
| **4K UHD** | `D2H_LOW_LATENCY_AUTO_SLOTS` | `BandStreamed` | 3 | `313.53` | `8.42 / 7.69 / 12.81` | `2.17` | `1.27` | `6.8%` |
| **8K UHD** | `D2H_THROUGHPUT_SLOTS1` | `FrameConsolidated` | 1 | **`57.91`** | **`17.09 / 14.00 / 31.77`** | `9.96` | `2.61` | `1.9%` |
| **8K UHD** | `D2H_LOW_LATENCY_SLOTS1` | `BandStreamed (Forced)` | 1 | `49.70` | `19.82 / 20.72 / 23.36` | `11.55` | `2.88` | `2.4%` |
| **8K UHD** | `D2H_THROUGHPUT_AUTO_SLOTS` | `FrameConsolidated` | 2 | `64.56` | `20.15 / 17.22 / 32.63` | `11.28` | `3.55` | `2.1%` |
| **8K UHD** | `D2H_LOW_LATENCY_AUTO_SLOTS` | `FrameConsolidated (8K Policy)` | 2 | **`91.21`** | **`14.28 / 13.89 / 16.30`** | `8.06` | `2.44` | `2.2%` |

Note: Once CUDA Graph capture reduces the entire 27-band entropy DAG to `0.47 ms` (1080p) and `1.96 ms` (4K), `FrameConsolidated` stays completely inside a single `cudaGraphLaunch`, whereas `BandStreamed` cannot use a single frame-wide graph because it interleaves 27 per-band events and host-polled DMAs. Both paths remain fully supported and verified in `test_phase2d`.

---

## Parts 6, 7, 8 & 9 — Controlled GPU Entropy Kernel Evolution (`2D-A -> 2D-B -> 2D-C`)

### 9.1 Kernel Launch & Intermediate Buffer Accounting Table

| Stage | `GpuEntropyKernelMode` | CUDA Graph | Kernels / Band | Total Frame Kernels (27 Bands + 2 Cons.) | Host Launch Calls / Frame | Graph Nodes (Kernel / Total) | Eliminated Intermediate Global Buffers & Passes |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :--- |
| **`PHASE2C_REFERENCE_NO_GRAPH`** | `ReferencePhase2C` | `false` | **16** | **434** | **434** | `0 / 0` | Baseline Phase 2C reference (16 kernels/band + 2 consolidation kernels). |
| **`PHASE2D_A_GRAPH_REFERENCE`** | `ReferencePhase2C` | `true` | **16** | **434** | **1** | `434 / 434` | Captures all 4 streams + cross-stream fork/join events into per-slot `cudaGraphExec_t`; eliminates **433 host launch calls/frame** (`~2.5–3.9 ms` WDDM/CUDA driver enqueue overhead). |
| **`PHASE2D_B_GRAPH_FUSED`** | `Fused` | `true` | **13** | **353** | **1** | `353 / 353` | 1. **`GatherAndEmitTokensGpu`** fuses `GatherRunsGpu + EmitTokensGpu` + zero-fill (`-2` kernels/band), eliminating global writes/reads of `ws.run_idx`, `ws.run_zeros`, and `ws.run_vals`.<br>2. **`MapTokensToCodewordsGpu`** fuses `TokensToCodewordsGpu + PackWordsPass1Gpu` (`-1` kernel/band), eliminating global writes/reads of `ws.codewords` (`81` kernel launches/frame removed). |
| **`PHASE2D_C_GRAPH_FUSED_SCAN_OPT`** | `FusedScanOptimized` | `true` | **9** | **245** | **1** | `245 / 245` | 1. **`ScanRunsSinglePassGpu`** replaces `ScanBlockRunsGpu + ScanBlockCarriesGpu` (`-1` kernel/band) using warp-ballot run segmentation + shared-memory lookback.<br>2. **`BitScanBlock1024Gpu + BitScanTopAndFinalizeGpu`** replaces the 5-kernel Blelloch tree (`BitOffsetBlockScanGpu`, `BitOffsetTopScanGpu x2`, `BitOffsetAddBlockSumsGpu x2`) with a 2-kernel radix-1024 warp-prefix scan (`-3` kernels/band), eliminating `ws.bit_block_sums1` and `ws.bit_block_sums2` (`108` more kernel launches/frame removed; **189 total kernel launches/frame removed vs Phase 2C**). |

### 9.2 GPU Entropy Substage Breakdown & Speedup Across All Stages (`SteadyState`)

*(Note: `sub_tot` is the sum of per-band CUDA event timers across all 27 highpass subbands executed with `record_substage_events = true`; `gpu_ent` is the concurrent 4-stream frame entropy wall time measured by `entropy_start_ev -> entropy_done_ev`.)*

| Resolution | Stage | Launches / Frame (Calls) | Host Enqueue (ms) | Mean / Med / P95 Wall (ms) | Concurrent `gpu_ent` (ms) | Speedup vs `NO_GRAPH` | `sub_tot` (ms) | `blk` (ms) | `mrg` (ms) | `gth` (ms) | `tok` (ms) | `cbk` (ms) | `bsc` (ms) | `pck` (ms) | Pure Prefix Scan Share (`mrg+bsc`) | Byte Match (`B==C`) |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **1080p** | `PHASE2C_REFERENCE_NO_GRAPH` | `434 (434)` | `3.881` | `4.074 / 4.132 / 4.618` | `3.874` | `1.00x` | `5.620` | `0.791` | `0.641` | `0.611` | `1.544` | `0.582` | `0.923` | `0.528` | `27.8%` | **YES** |
| **1080p** | `PHASE2D_A_GRAPH_REFERENCE` | `434 (1)` | `0.062` | `0.962 / 0.964 / 1.005` | `0.765` | **`5.07x`** | `4.994` | `0.816` | `0.549` | `0.440` | `1.528` | `0.301` | `0.916` | `0.444` | `29.3%` | **YES** |
| **1080p** | `PHASE2D_B_GRAPH_FUSED` | `353 (1)` | `0.054` | `0.721 / 0.695 / 0.811` | `0.533` | **`7.27x`** | `3.906` | `0.303` | `0.750` | `0.376` | `0.964` | `0.122` | `0.856` | `0.535` | `41.1%` | **YES** |
| **1080p** | `PHASE2D_C_GRAPH_FUSED_SCAN_OPT` | `245 (1)` | `0.056` | **`0.675 / 0.667 / 0.813`** | **`0.470`** | **`8.24x`** | **`3.550`** | `0.413` | **`0.423`** | `0.335` | `0.980` | `0.144` | **`0.621`** | `0.634` | **`29.4%`** | **YES** |
| **4K UHD** | `PHASE2C_REFERENCE_NO_GRAPH` | `434 (434)` | `2.596` | `19.186 / 19.489 / 19.758` | `17.358` | `1.00x` | `72.060` | `6.632` | `4.628` | `8.697` | `23.963` | `4.950` | `14.607` | `8.582` | `26.7%` | **YES** |
| **4K UHD** | `PHASE2D_A_GRAPH_REFERENCE` | `434 (1)` | `0.098` | `4.117 / 4.172 / 4.256` | `3.034` | **`5.72x`** | `64.130` | `5.918` | `7.905` | `7.263` | `21.392` | `5.631` | `9.370` | `6.651` | `26.9%` | **YES** |
| **4K UHD** | `PHASE2D_B_GRAPH_FUSED` | `353 (1)` | `0.095` | `3.334 / 3.342 / 3.440` | `2.192` | **`7.92x`** | `6.610` | `0.802` | `1.183` | `0.782` | `1.391` | `0.205` | `1.106` | `1.142` | `34.6%` | **YES** |
| **4K UHD** | `PHASE2D_C_GRAPH_FUSED_SCAN_OPT` | `245 (1)` | `0.077` | **`3.001 / 3.064 / 3.159`** | **`1.963`** | **`8.84x`** | **`7.760`** | `0.600` | **`0.995`** | `1.139` | `2.620` | `0.126` | **`0.762`** | `1.519` | **`22.6%`** | **YES** |
| **8K UHD** | `PHASE2C_REFERENCE_NO_GRAPH` | `434 (434)` | `2.307` | `21.481 / 14.975 / 45.400` | `19.722` | `1.00x` | `166.025` | `41.539` | `38.951` | `16.470` | `28.001` | `7.316` | `13.857` | `19.891` | `31.8%` | **YES** |
| **8K UHD** | `PHASE2D_A_GRAPH_REFERENCE` | `434 (1)` | `0.134` | `12.345 / 12.304 / 12.830` | `10.765` | **`1.83x`** | `48.738` | `5.414` | `10.121` | `6.453` | `12.900` | `3.881` | `5.894` | `4.074` | `32.9%` | **YES** |
| **8K UHD** | `PHASE2D_B_GRAPH_FUSED` | `353 (1)` | `0.122` | `9.382 / 9.580 / 9.856` | `7.849` | **`2.51x`** | `32.111` | `3.359` | `7.548` | `3.053` | `8.851` | `0.144` | `5.214` | `3.941` | `39.7%` | **YES** |
| **8K UHD** | `PHASE2D_C_GRAPH_FUSED_SCAN_OPT` | `245 (1)` | `0.118` | **`8.372 / 8.209 / 9.063`** | **`6.742`** | **`2.93x`** | **`28.133`** | `2.812` | **`4.714`** | `3.914` | `7.994` | `0.594` | **`3.697`** | `4.408` | **`29.9%`** | **YES** |

---

## Part 10 — End-to-End Benchmark Progression (Apples-to-Apples `SteadyState` & `Frame1Initial`)

> **Historical Phase 2D Note:** The figures in this section are historical Phase 2D fixed-state (`SteadyState` / `Frame1Initial`) development snapshots. For the authoritative 5-trial `ProductionEndToEnd` sequence benchmark with dynamic per-frame `FSratelimiter` rate control on a clean working tree, see [`docs/PHASE3A1_BENCHMARK_HARDENING.md`](PHASE3A1_BENCHMARK_HARDENING.md) and [`benchmarks/phase3a2_release_candidate.json`](../benchmarks/phase3a2_release_candidate.json).

| Resolution | Configuration | Quant State | Slots | Sustained FPS | Speedup vs Upstream | Mean / Med / P95 Latency (ms) | GPU Wavelet (ms) | GPU Entropy (ms) | CPU Assembly (ms) | Norm. CPU % (16 Logical) | Core-Sec / 60fps Sec | Sample Bytes | VRAM (MB) |
| :--- | :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **1080p** | `UPSTREAM_DEFAULT` | `SteadyState` | 1 | `42.31` | `1.00x` | `23.63 / 23.60 / 24.26` | — | — | — | — | — | `1,181,024` | — |
| **1080p** | `UPSTREAM_DEFAULT` | `Frame1Initial` | 1 | `47.93` | `1.00x (F1)` | `20.86 / 20.23 / 24.18` | — | — | — | — | — | `900,396` | — |
| **1080p** | `PHASE2C_BEST (Dev)` | `SteadyState` | 2 | `168.33` | `3.98x` | `11.33 / 11.87 / 12.46` | `0.30` | `4.85` | `0.19` | `6.2%` | `0.352` | `1,181,024` | `174.0` |
| **1080p** | `PHASE2D_GRAPH (Dev)` | `SteadyState` | 2 | `257.45` | `6.08x` | `4.40 / 4.39 / 4.53` | `0.30` | `2.85` | `0.18` | `<0.5%` | `<0.02` | `1,181,024` | `178.0` |
| **1080p** | `PHASE2D_FUSED_GRAPH (Dev)` | `SteadyState` | 2 | `746.26` | `17.64x` | `2.05 / 1.98 / 2.77` | `0.36` | `0.71` | `0.16` | `6.1%` | `0.078` | `1,181,024` | `174.0` |
| **1080p** | **`PHASE2D_FINAL (Dev)`** | `SteadyState` | 2 | **`804.72`** | **`19.02x`** | **`1.89 / 1.85 / 2.23`** | `0.33` | **`0.64`** | `0.15` | `<0.5%` | `<0.02` | `1,181,024` | `174.0` |
| **1080p** | **`PHASE2D_FINAL (Host)`** | `SteadyState` | 2 | **`352.67`** | **`8.34x`** | **`3.32 / 3.32 / 3.61`** | `0.26` | `0.62` | `0.18` | `2.9%` | `0.078` | `1,181,024` | `174.0` |
| **1080p** | `PHASE2D_FINAL (Dev)` | `Frame1Initial` | 2 | **`900.44`** | **`18.79x (F1)`** | **`1.72 / 1.67 / 2.08`** | `0.32` | `0.54` | `0.11` | `3.7%` | `0.039` | `900,396` | `174.0` |
| **1080p** | `PHASE2D_FINAL (Host)` | `Frame1Initial` | 2 | **`368.11`** | **`7.68x (F1)`** | **`3.15 / 3.17 / 3.33`** | `0.25` | `0.56` | `0.13` | `<0.5%` | `<0.02` | `900,396` | `174.0` |
| **4K UHD** | `UPSTREAM_DEFAULT` | `SteadyState` | 1 | `10.73` | `1.00x` | `93.16 / 93.12 / 94.73` | — | — | — | — | — | `4,668,864` | — |
| **4K UHD** | `UPSTREAM_DEFAULT` | `Frame1Initial` | 1 | `12.74` | `1.00x (F1)` | `78.49 / 78.73 / 80.77` | — | — | — | — | — | `3,563,684` | — |
| **4K UHD** | `PHASE2C_BEST (Dev)` | `SteadyState` | 3 | `59.25` | `5.52x` | `45.58 / 61.34 / 68.74` | `2.01` | `14.40` | `0.80` | `6.4%` | `1.042` | `4,668,864` | `838.0` |
| **4K UHD** | `PHASE2D_GRAPH (Dev)` | `SteadyState` | 3 | `300.26` | `27.98x` | `7.45 / 7.16 / 8.71` | `0.47` | `2.96` | `1.01` | `3.3%` | `0.104` | `4,668,864` | `842.0` |
| **4K UHD** | `PHASE2D_FUSED_GRAPH (Dev)` | `SteadyState` | 3 | `368.95` | `34.38x` | `6.11 / 5.83 / 7.10` | `0.65` | `2.37` | `0.90` | `6.0%` | `0.156` | `4,668,864` | `838.0` |
| **4K UHD** | **`PHASE2D_FINAL (Dev)`** | `SteadyState` | 3 | **`439.12`** | **`40.92x`** | **`5.38 / 5.36 / 6.71`** | `0.53` | **`2.09`** | `0.67` | `4.8%` | `0.104` | `4,668,864` | `838.0` |
| **4K UHD** | **`PHASE2D_FINAL (Host)`** | `SteadyState` | 3 | **`273.17`** | **`25.46x`** | **`9.52 / 9.48 / 10.86`** | `0.33` | `1.56` | `0.81` | `7.4%` | `0.260` | `4,668,864` | `838.0` |
| **4K UHD** | `PHASE2D_FINAL (Dev)` | `Frame1Initial` | 3 | **`596.25`** | **`46.80x (F1)`** | **`4.02 / 4.04 / 4.43`** | `0.43` | `1.48` | `0.50` | `6.5%` | `0.104` | `3,563,684` | `838.0` |
| **4K UHD** | `PHASE2D_FINAL (Host)` | `Frame1Initial` | 3 | **`286.68`** | **`22.50x (F1)`** | **`9.18 / 9.29 / 9.52`** | `0.29` | `1.13` | `0.51` | `6.2%` | `0.208` | `3,563,684` | `838.0` |
| **8K UHD** | `UPSTREAM_DEFAULT` | `SteadyState` | 1 | `2.72` | `1.00x` | `367.67 / 367.72 / 375.51` | — | — | — | — | — | `18,393,596` | — |
| **8K UHD** | `UPSTREAM_DEFAULT` | `Frame1Initial` | 1 | `3.11` | `1.00x (F1)` | `321.28 / 322.89 / 333.04` | — | — | — | — | — | `14,009,028` | — |
| **8K UHD** | `PHASE2C_BEST (Dev)` | `SteadyState` | 2 | `69.41` | `25.52x` | `26.38 / 26.75 / 30.60` | `1.59` | `13.11` | `2.89` | `7.9%` | `1.094` | `18,393,596` | `2,604.0` |
| **8K UHD** | **`PHASE2D_FINAL (Dev)`** | `SteadyState` | 2 | **`103.57`** | **`38.08x`** | **`13.41 / 13.28 / 15.15`** | `1.21` | **`7.06`** | `2.92` | `3.4%` | `0.312` | `18,393,596` | `2,604.0` |
| **8K UHD** | **`PHASE2D_FINAL_MAX_TPUT (Host)`** | `SteadyState` | 3 | **`67.45`** | **`24.80x`** | **`34.84 / 35.44 / 35.58`** | `1.42` | `8.29` | `2.17` | `4.4%` | `0.625` | `18,393,596` | `3,288.0` |
| **8K UHD** | `PHASE2D_FINAL (Dev)` | `Frame1Initial` | 2 | **`136.95`** | **`44.04x (F1)`** | **`10.54 / 10.41 / 12.15`** | `1.21` | `4.86` | `2.65` | `5.6%` | `0.391` | `14,009,028` | `2,604.0` |

---

## Part 11 — Canonical PCIe Gen4 x8 Transfer Baseline (~13.3 GB/s)

| Baseline Classification | Host Memory Type | Direction & Mode | 4 MB (`1080p` Frame) | 32 MB (`4K` Frame) | 128 MB (`8K` Frame) | Status & Explanation |
| :--- | :--- | :--- | :---: | :---: | :---: | :--- |
| **Canonical Phase 2C/2D Baseline** | `cudaMallocHost` (Pinned) | `h2d_async` | `13.12 GB/s` (`0.320 ms`) | **`13.29 GB/s` med** (`2.526 ms`) | **`13.29 GB/s` med** (`10.237 ms`) | **Canonical Hardware Ceiling** on PCIe Gen4 x8 (`asyncEngineCount = 1`). |
| **Canonical Phase 2C/2D Baseline** | `cudaMallocHost` (Pinned) | `d2h_async` | `12.92 GB/s` (`0.325 ms`) | **`13.02 GB/s` med** (`2.648 ms`) | **`13.08 GB/s` med** (`10.514 ms`) | **Canonical Hardware Ceiling** for compressed D2H DMA (`~0.35 ms` for 4.67 MB 4K bitstream). |
| **Canonical Phase 2C/2D Baseline** | `cudaMallocHost` (Pinned) | `bidirectional_async` | `12.99 GB/s` | `12.95 GB/s` | `12.81 GB/s` | Single copy engine (`asyncEngineCount = 1`) serializes H2D and D2H on the same physical engine. |
| **Pageable Host Memory** | `std::malloc` (Pageable) | `h2d_sync` / `d2h_sync` | `10.30 / 11.15 GB/s` | `10.97 / 11.75 GB/s` | `11.26 / 10.76 GB/s` | Staging buffer overhead through WDDM driver. |
| *Historical Phase 2A/2B Figure* | *Mixed / Unwarmed* | *Historical Report* | *`3.3 – 6.6 GB/s`* | *`3.3 – 6.6 GB/s`* | *`3.3 – 6.6 GB/s`* | **Superseded**: Included cold page-fault / per-subband synchronous wait overhead before pinned pool warm-up. Preserved strictly for historical traceability. |

---

## Part 12 — Production-Readiness Assessment

### 1. Is the CUDA encoder now suitable as the canonical reference GPU implementation for HyperCine?
- **Yes.** The CUDA encoder produces **100% bit-identical subband payloads (`30/30` subbands)** and **100% byte-identical complete `CFHD` samples** relative to both the HyperCine CPU reference path (**B**) and pristine upstream `CFHD_EncodeSample` (**A**) across `1080p`, `4K UHD`, and `8K UHD` in both `Frame1Initial` (`FSratelimiter = 4`) and `SteadyState` (`FSratelimiter = 0`) quantizer states.
- All four GPU execution modes (`ReferencePhase2C` without graph, `ReferencePhase2C` with graph, `Fused` with graph, and `FusedScanOptimized` with graph) are preserved side-by-side in `src/cuda/entropy_gpu.cu` and verified by `ctest` (`test_phase2d`) to produce identical SHA-256 bitstreams.

### 2. Remaining Limitations Before Shipping in a Production SDK
1. **Pixel Format Frontend Coverage**: The current Phase 2C/2D full-GPU pipeline specializes in 8-bit `BGRA` (`CFHD_PIXEL_FORMAT_BGRA` -> 12-bit internal YUV 4:2:2). Production SDK users also require direct GPU frontend ingestion for 10-bit `v210`, 16-bit `RG48`/`B64A`, 12-bit `BYR4` Bayer RAW, and 4:4:4/4:4:4:4 alpha channels.
2. **Dynamic Rate-Limiter Feedback in Multi-Frame Streaming**: `Phase2DEncoderContext` supports both `Frame1Initial` and `SteadyState` quantizer tables. Phase 3A applies the per-frame `quant[36]` table in `HyperCine_GpuSlotKickWaveletWithQuant`, which is a normal kernel launch outside the entropy CUDA graph. The entropy graph reads already-quantized coefficient buffers at fixed device pointers and does not contain the quantizer table, so an `FSratelimiter` change does not recapture that graph and does not call `cudaGraphExecKernelNodeSetParams`. The authoritative description is in `docs/PHASE3A1_BENCHMARK_HARDENING.md`.
3. **Single-Copy-Engine H2DContention on Laptop GPUs**: On GPUs with `asyncEngineCount == 1` (such as RTX 4090 Laptop GPU over PCIe Gen4 x8), raw host-input frames (`33.18 MB` at 4K, `132.71 MB` at 8K) share the single DMA engine with compressed D2H (`4.67 MB` at 4K, `18.39 MB` at 8K), capping host-input throughput at `273.17 FPS` (4K) and `67.45 FPS` (8K) vs `439.12 FPS` (4K) and `103.57 FPS` (8K) for device-resident inputs.

### 3. What Should Be Ported Directly to Vulkan / Metal Later vs Remain CUDA-Specific
- **Port Directly to Vulkan Compute (SPIR-V) and Apple Metal (MSL)**:
  - The 9-kernel-per-band `FusedScanOptimized` algorithmic pipeline (`ScanRunsSinglePass`, `GatherAndEmitTokens`, `MapTokensToCodewords`, `BitScanBlock1024`, `BitScanTopAndFinalize`, `PackWordsPass1`, `PackWordsPass2`, `WriteBandTrailer`, `PatchBandBitCount`) and the 2-kernel frame payload consolidation (`ComputeFramePayloadOffsets`, `ConsolidateFramePayload`).
  - Subgroup / SIMD-group intrinsics (`__ballot_sync` / `__shfl_up_sync` -> `subgroupBallot` / `subgroupShuffleUp` in Vulkan GLSL/HLSL; `simd_ballot` / `simd_shuffle_up` in Metal).
  - The LPT 27-band multi-queue schedule, resolution-aware slot selection policy (`SelectPhase2DFrameSlots`), and blocking host-event synchronization architecture.
- **Keep CUDA-Specific**:
  - `cudaGraphCapture` / `cudaGraphLaunch` API calls (in Vulkan, this maps directly to pre-recorded reusable `VkCommandBuffer`s with `vkCmdPipelineBarrier`; in Metal, to a single `MTLCommandBuffer` or `MTLIndirectCommandBuffer`).
  - `cudaMallocHost` / `cudaEventBlockingSync` primitives.
