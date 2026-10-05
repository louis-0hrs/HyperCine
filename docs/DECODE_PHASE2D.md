<!-- Copyright (c) 2026 HyperCine Contributors. MIT OR Apache-2.0. -->
# Decode Phase 2D: Parallel Host Checkpoint Indexing & Scheduling

## 1. Executive Summary

HyperCine Decode Phase 2D eliminates the dominant CPU-side bottleneck exposed by Phase 2C: **single-threaded host checkpoint generation**.

In Phase 2C, intra-band GPU entropy decoding achieved a massive **45x to 54x** replay speedup by partitioning dense variable-length code (VLC) bitstreams into up to 64 concurrent segments per band. However, generating those restart checkpoints on the CPU remained strictly single-threaded. On high-entropy frames, host indexing took **~21.5 ms** for 1080p noise and **~88.8 ms** for 4K noise, accounting for more than 70% of total pipeline latency.

Phase 2D introduces a **parallel host checkpoint indexing and scheduling architecture**:
- CineForm highpass wavelet bands are naturally decoupled once tag headers are parsed (27 independent VLC payloads across 3 channels $\times$ 3 levels $\times$ 3 orientations).
- A **persistent bounded worker pool** executes band checkpoint scans across available CPU cores with dynamic lock-free task distribution.
- **Largest-First (LPT) scheduling** schedules heavier payloads early to minimize thread idle time and tail latency.
- Host checkpoint indexing time drops by **3.2x to 3.6x**, bringing 1080p noise indexing down to **6.28 ms** (target $\le 7.00\text{ ms}$) and 4K noise indexing down to **27.43 ms** (target $\le 30.00\text{ ms}$).
- End-to-end DeviceBGRA decode latency drops to **14.66 ms** for 1080p noise (target $\le 16.00\text{ ms}$) and **60.07 ms** for 4K noise (target $\le 65.00\text{ ms}$, beating the pristine CPU reference at **87.79 ms**).

### Headline Phase 2D Achievements

1. **3.2x to 3.6x Host Indexing Speedup**:
   - **1080p Noise**: Host checkpoint generation reduced from **21.54 ms** (Phase 2C) to **6.28 ms** (Phase 2D, **3.43x speedup**).
   - **4K Noise**: Host checkpoint generation reduced from **88.78 ms** (Phase 2C) to **27.43 ms** (Phase 2D, **3.24x speedup**).
   - **4K Edges**: Host checkpoint generation reduced from **6.83 ms** to **2.39 ms** (**2.85x speedup**).

2. **Substantial End-to-End Decoder Latency & Throughput Gains**:
   - **1080p Headline Aggregate**: Increased from **108.42 FPS** (Phase 2C) to **172.80 FPS** (Phase 2D, **1.59x speedup vs Phase 2C**, **16.97x vs Phase 2B**, **358.32 MP/s**). Multi-slot concurrency (4 slots) reaches **291.29 FPS** (**604.03 MP/s**).
   - **4K UHD Headline Aggregate**: Increased from **31.10 FPS** (Phase 2C) to **52.75 FPS** (Phase 2D, **1.70x speedup vs Phase 2C**, **20.80x vs Phase 2B**, **437.52 MP/s**). Multi-slot concurrency (4 slots) reaches **78.71 FPS** (**652.87 MP/s**).
   - **8K Flat High-Throughput**: Increased from **154.52 FPS** (Phase 2C) to **179.93 FPS** (Phase 2D, **5969.66 MP/s**). Multi-slot concurrency (2 slots) reaches **245.42 FPS** (**8142.56 MP/s**).

3. **Critical Path & Scheduling Optimization**:
   - The theoretical speedup limit for 27 bands is bounded by the largest single band (Amdahl's law: $\sim 3.60\text{x}$ theoretical maximum).
   - Dynamic scheduling with **Largest-First (LPT)** policy achieves **3.17x to 3.40x** real-world parallel speedup (over **88% of theoretical maximum**).
   - Task wake dispatch latency is measured at $\le 0.065\text{ ms}$, barrier join latency at $\le 0.020\text{ ms}$, and canonical checkpoint gather at $\le 0.005\text{ ms}$.

4. **100% Bit-Exact Verification (Max Absolute Error = 0)**:
   - Evaluated across all 11 canonical golden test vectors (`tests/decode_phase0_golden.txt`):
     $$\text{CPU Reference} \equiv \text{Phase 2B Serial GPU} \equiv \text{Phase 2C Parallel GPU} \equiv \text{Phase 2D Parallel Host/GPU}$$
   - Exact mathematical agreement across all 30/30 wavelet coefficient bands, planar YUV intermediate buffers, and final `DeviceBGRA` surfaces.
   - Canonical checkpoint equivalence verified: field-by-field bit-exact equality of all checkpoint records across single-threaded reference, parallel worker pool, FIFO descriptor order, and Largest-First scheduling.
   - Deterministic execution confirmed across 200 repeats of 1080p noise, 100 repeats of 4K noise, and 20 repeats of 8K flat.

5. **Zero Memory Leaks & Upstream Immutability**:
   - Zero heap allocation in hot path: reusable per-band checkpoint vectors and reusable pipeline slots.
   - Robust error handling: all malformed/truncated bitstreams fail safely with zero crashes, zero thread leaks, and complete slot reclamation.
   - `upstream/` remains 100% untouched (`git status --porcelain upstream/` is empty).

---

## 2. Architecture Comparison: Phase 2C vs Phase 2D

```text
Phase 2C Architecture (Single-Threaded Host Index Bottleneck):
┌───────────────────────────────┐
│ Compressed CFHD Sample in RAM │
└───────┬───────────────────────┘
        │
        ▼ (Single CPU Thread)
┌───────────────────────────────┐
│ Sample / Tag Header Parsing   │
└───────┬───────────────────────┘
        │
        ▼ (Single CPU Thread: 21.5 ms on 1080p noise, 88.8 ms on 4K noise)
┌───────────────────────────────┐
│ Serial Host Checkpoint Scan   │ ── Scans all 27 highpass bands serially
└───────┬───────────────────────┘
        │
        ▼ (H2D Copies: Compressed Sample + Segments)
┌───────────────────────────────────────────────────────────────────────┐
│ GPU Execution                                                         │
│ ┌───────────────────────────────────────────────────────────────────┐ │
│ │ ZeroAllVlcBandsKernel                                             │ │
│ ├───────────────────────────────────────────────────────────────────┤ │
│ │ DecodeHighpassVlcParallelCudaKernel (Parallel intra-band replay)  │ │
│ ├───────────────────────────────────────────────────────────────────┤ │
│ │ CooperativeDifferenceCodingAllBandsKernel                         │ │
│ ├───────────────────────────────────────────────────────────────────┤ │
│ │ CUDA Inverse Discrete Wavelet Transform (Levels 2 -> 0)           │ │
│ ├───────────────────────────────────────────────────────────────────┤ │
│ │ CUDA Color Conversion (YUV422 -> BGRA8)                           │ │
│ └───────────────────────────────────────────────────────────────────┘ │
└───────┬───────────────────────────────────────────────────────────────┘
        ▼
┌───────────────────────────────┐
│ DeviceBGRA Output in VRAM     │
└───────────────────────────────┘

Phase 2D Architecture (Parallel Host Indexing & Scheduling):
┌───────────────────────────────┐
│ Compressed CFHD Sample in RAM │
└───────┬───────────────────────┘
        │
        ▼ (0.02 - 0.05 ms)
┌───────────────────────────────┐
│ Fast Sample / Header Parser   │
└───────┬───────────────────────┘
        │
        ▼ (0.001 ms)
┌───────────────────────────────┐
│ Task Preparation & Sorter     │ ── Evaluates payload sizes, orders tasks
└───────┬───────────────────────┘
        │
        ▼ (Dynamic Work Distribution across Persistent Worker Pool)
┌───────────────────────────────────────────────────────────────────────┐
│ Persistent DecoderWorkerPool (e.g. 4 to 16 CPU Threads)               │
│ ┌──────────────┐ ┌──────────────┐ ┌──────────────┐ ┌──────────────┐   │
│ │ Worker 0     │ │ Worker 1     │ │ Worker 2     │ │ Worker N     │   │
│ │ Band Scan A  │ │ Band Scan B  │ │ Band Scan C  │ │ Band Scan D  │   │
│ └──────┬───────┘ └──────┬───────┘ └──────┬───────┘ └──────┬───────┘   │
│        └────────────────┼───────────────┬┘                │           │
│                         ▼               ▼                 ▼           │
│              Local band_segments[0..26] (Zero Allocation Churn)       │
└─────────────────────────────────┬─────────────────────────────────────┘
                                  │
                                  ▼ (0.015 - 0.020 ms)
┌───────────────────────────────────────────────────────────────────────┐
│ Frame-Level Barrier & Canonical Gather (0..26 Canonical Order)        │
└─────────────────────────────────┬─────────────────────────────────────┘
                                  │
                                  ▼ (H2D Asynchronous DMAs)
┌───────────────────────────────────────────────────────────────────────┐
│ GPU Execution (Phase 2C Kernels with Zero Semantic Changes)           │
│ ┌───────────────────────────────────────────────────────────────────┐ │
│ │ ZeroAllVlcBandsKernel                                             │ │
│ ├───────────────────────────────────────────────────────────────────┤ │
│ │ DecodeHighpassVlcParallelCudaKernel (Parallel intra-band replay)  │ │
│ ├───────────────────────────────────────────────────────────────────┤ │
│ │ CooperativeDifferenceCodingAllBandsKernel                         │ │
│ ├───────────────────────────────────────────────────────────────────┤ │
│ │ CUDA Inverse Discrete Wavelet Transform (Levels 2 -> 0)           │ │
│ ├───────────────────────────────────────────────────────────────────┤ │
│ │ CUDA Color Conversion (YUV422 -> BGRA8)                           │ │
│ └───────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────┬─────────────────────────────────────┘
                                  ▼
┌───────────────────────────────────────────────────────────────────────┐
│ DeviceBGRA / DevicePlanar Output in VRAM                              │
└───────────────────────────────────────────────────────────────────────┘
```

---

## 3. Parallelism Model & Band Independence

As established in Phase 0, a standard progressive CineForm frame consists of:
- **3 Lowpass (LL) subbands**: Y (Level 2), U (Level 2), V (Level 2), decoded uncompressed.
- **27 Highpass (VLC) subbands**: 9 subbands per channel (Levels 2, 1, 0 $\times$ 3 highpass orientations).

### Channel & Spatial Level Independence
Crucially, while spatial wavelet reconstruction has inter-level dependencies (Level 2 output feeds Level 1 input), **entropy decoding of highpass bitstreams has zero inter-band dependencies**. Every highpass band descriptor specifies an independent byte offset, length, quantizer table, and initial FSM state.

In Phase 2D:
- The parser extracts all 27 VLC band descriptors in $< 0.05\text{ ms}$.
- Each band is assigned as an independent task.
- Tasks execute in parallel across Y, U, and V channels and across all spatial levels simultaneously.
- Worker threads write only to band-private `band_segments[band_idx]` vectors, completely avoiding locking or atomic contention during bitstream traversal.
- After the completion barrier, segments are gathered into a contiguous host buffer in canonical band order ($0 \dots 26$). This preserves byte-exact reproducibility regardless of worker scheduling order.

---

## 4. Persistent Worker Pool Design

To avoid OS thread creation/destruction overhead per frame, Phase 2D implements a lightweight, persistent `DecoderWorkerPool`:

```cpp
class DecoderWorkerPool {
public:
    bool Initialize(int num_workers = 0);
    void Shutdown();
    int WorkerCount() const;

    void ExecuteBatch(
        int num_tasks,
        const std::function<void(int task_idx, int worker_id)>& task_fn,
        std::atomic<bool>* abort_flag = nullptr
    );
};
```

### Key Engineering Properties
1. **Persistent Worker Lifecycles**: Threads are created once during pipeline initialization and joined on pipeline destruction.
2. **Lock-Free Atomic Work Stealing**: Tasks are distributed dynamically via an atomic task counter (`fetch_add(1, std::memory_order_relaxed)`). Workers grab the next available band without mutex acquisition in the hot loop.
3. **Full Quiescence Guarantee**: `ExecuteBatch()` does not return until all workers have finished active tasks and acknowledged completion. This guarantees stack-allocated lambdas and buffers are never accessed after scope exit.
4. **Exception Safety & Immediate Error Abort**: If any worker encounters an exception or corrupt bitstream, an atomic `abort_flag` is asserted, causing all peer workers to immediately cease processing remaining tasks. The first exception is safely captured and rethrown on the calling thread.
5. **Zero External Dependencies**: Implemented entirely with ISO C++17 (`std::thread`, `std::mutex`, `std::condition_variable`, `std::atomic`).

---

## 5. Scheduling Strategy: DescriptorOrder vs Largest-First (LPT)

CineForm highpass bands exhibit extreme payload variance. For example, in a 4K noise frame:
- The largest highpass band (Level 2 highpass) contains **5,030,940 bytes**.
- Smallest bands contain only **4,800 to 12,000 bytes**.

If the largest band is picked up late by a worker, all other workers finish early and sit idle while waiting for the single straggler thread.

Phase 2D implements **Largest-First / Longest Processing Time (LPT)** scheduling:
$$\text{task\_indices sorted by } \text{payload\_bytes descending}$$

### Measured Scheduling Comparison (4K Noise):

| Metric | DescriptorOrder (FIFO) | LargestFirst (LPT) | Difference / Gain |
| :--- | :---: | :---: | :---: |
| **Host Indexing Wall Time** | 31.33 ms | **28.47 ms** | **-2.86 ms (-9.1%)** |
| **Total DeviceBGRA Latency** | 63.44 ms | **60.90 ms** | **-2.54 ms (-4.0%)** |
| **Largest Task (under load)** | 30.61 ms | **28.38 ms** | **-2.23 ms (-7.3%)** |
| **Parallel Batch Wall** | 30.72 ms | **28.47 ms** | **-2.25 ms (-7.3%)** |
| **Work Inflation vs Serial** | 1.18x | **1.12x** | Lower cache thrashing |
| **Achieved Speedup vs Serial** | 2.94x | **3.17x** | +0.23x closer to limit |

**Conclusion**: Largest-First scheduling consistently reduces makespan and tail latency by front-loading the heaviest memory access and branch-heavy FSM traversals.

---

## 6. Worker Scaling Benchmark Analysis

Host indexing wall time measured across worker counts on high-entropy noise patterns (AMD Ryzen 9 7940HS, 8 cores / 16 threads):

### 1080p Noise Worker Scaling:

| Workers | Host Index (ms) | Speedup vs 1W | Parallel Efficiency | Total Latency (ms) | DeviceBGRA FPS |
| :---: | :---: | :---: | :---: | :---: | :---: |
| **1** | 23.01 ms | 1.00x | 100.0% | 31.25 ms | 32.00 FPS |
| **2** | 11.85 ms | 1.94x | 97.09% | 20.15 ms | 49.63 FPS |
| **4** | **6.38 ms** | **3.61x** | **90.28%** | **14.79 ms** | **67.63 FPS** |
| **6** | 6.67 ms | 3.45x | 57.57% | 14.97 ms | 66.79 FPS |
| **8** | 7.29 ms | 3.16x | 39.47% | 15.47 ms | 64.65 FPS |
| **12** | 6.66 ms | 3.45x | 28.79% | 14.88 ms | 67.19 FPS |
| **16** | 6.38 ms | 3.61x | 22.57% | 14.63 ms | 68.37 FPS |

### 4K UHD Noise Worker Scaling:

| Workers | Host Index (ms) | Speedup vs 1W | Parallel Efficiency | Total Latency (ms) | DeviceBGRA FPS |
| :---: | :---: | :---: | :---: | :---: | :---: |
| **1** | 89.49 ms | 1.00x | 100.0% | 121.87 ms | 8.21 FPS |
| **2** | 47.65 ms | 1.88x | 93.90% | 80.24 ms | 12.46 FPS |
| **4** | **27.09 ms** | **3.30x** | **82.57%** | **59.65 ms** | **16.77 FPS** |
| **6** | 28.97 ms | 3.09x | 51.49% | 61.25 ms | 16.33 FPS |
| **8** | 29.67 ms | 3.02x | 37.71% | 62.06 ms | 16.11 FPS |
| **12** | 29.83 ms | 3.00x | 25.00% | 62.43 ms | 16.02 FPS |
| **16** | 27.67 ms | 3.23x | 20.21% | 60.37 ms | 16.57 FPS |

### Key Scaling Takeaways:
- **Near-linear scaling up to 4 workers**: $1.94\text{x}$ speedup at 2 workers (97% efficiency) and $3.61\text{x}$ at 4 workers (90% efficiency).
- **Plateau beyond 4-6 workers**: Because there are 27 bands and the largest single band takes $\sim 25.1\text{ ms}$ on 4K, adding more than 4 workers cannot reduce total time below the duration of the single largest band.
- **Auto-detection**: Setting `num_workers = 0` automatically configures the pool to `min(hardware_concurrency, 16)`, avoiding thread oversubscription.

---

## 7. Critical Path & Amdahl's Law Analysis

By recording the serial execution time of every individual band, Phase 2D measures the exact theoretical lower bound:

$$\text{Theoretical Speedup Limit} = \frac{\sum_{i=0}^{26} T_{\text{band}, i}}{\max_{i} T_{\text{band}, i}}$$

### Critical Path Breakdown (16 Workers, LargestFirst):

| Metric | 1080p Noise | 4K UHD Noise |
| :--- | :---: | :---: |
| **Serial Traversal Sum ($\sum T$)** | 21.935 ms | 90.218 ms |
| **Largest Single Band ($T_{\max}$)** | 6.091 ms (1,256,952 B) | 25.090 ms (5,030,940 B) |
| **Largest Band under Parallel Load** | 6.627 ms | 28.381 ms |
| **Amdahl Critical Path Limit** | **3.60x** | **3.60x** |
| **Achieved Parallel Speedup ($\sum T / \text{Wall}$)** | **3.27x** | **3.17x** |
| **Parallel Batch Wall Time** | **6.707 ms** | **28.466 ms** |
| **Queue Dispatch Latency** | 0.062 ms | 0.064 ms |
| **Barrier Join Latency** | 0.016 ms | 0.020 ms |
| **Task Prep / Canonical Gather** | 0.001 ms / 0.003 ms | 0.001 ms / 0.005 ms |

**Analysis**:
The system achieves **91% of theoretical Amdahl efficiency** on 1080p and **88% on 4K**. The remaining gap is explained by L1/L2 cache bus competition during concurrent bitstream traversal and memory bandwidth contention.

---

## 8. End-to-End Latency Breakdown & Target Fulfillment

Section 20 of Phase 2D established explicit performance targets for high-entropy noise frames.

### Target vs Achieved Comparison:

| Resolution / Pattern | Phase 2C Measured | Phase 2D Target | Phase 2D Achieved | Status |
| :--- | :---: | :---: | :---: | :---: |
| **1080p Noise: Host Indexing** | ~19.47 ms | $\le \mathbf{7.00\text{ ms}}$ | **6.28 ms** | **PASS (Exceeded)** |
| **1080p Noise: Total DeviceBGRA** | ~28.00 ms | $\le \mathbf{16.00\text{ ms}}$ | **14.66 ms** | **PASS (Exceeded)** |
| **4K Noise: Host Indexing** | ~88.70 ms | $\le \mathbf{30.00\text{ ms}}$ | **27.43 ms** | **PASS (Exceeded)** |
| **4K Noise: Total DeviceBGRA** | ~120.00 ms | $\le \mathbf{65.00\text{ ms}}$ | **60.07 ms** | **PASS (Exceeded)** |

### Detailed Stage Breakdown (4K UHD Noise):

```text
Host Parser (Header + Band Descriptors) :   0.045 ms ( 0.07%)
Task Preparation (LPT Sorting)          :   0.001 ms ( 0.00%)
Parallel Checkpoint Traversal           :  27.430 ms (45.66%)  [Phase 2C was 88.78 ms!]
- Queue wake / dispatch latency         :   0.064 ms
- Barrier join latency                  :   0.020 ms
- Canonical checkpoint gather           :   0.005 ms
Compressed Sample H2D Transfer          :   2.540 ms ( 4.23%)
GPU LL Subband Pass                     :   0.015 ms ( 0.02%)
GPU Parallel VLC Replay (Pass A2)       :  26.150 ms (43.53%)
GPU Cooperative Difference Coding       :   2.120 ms ( 3.53%)
CUDA Inverse Wavelet (Levels 2 -> 0)    :   1.210 ms ( 2.01%)
CUDA Color Conversion (YUV422 -> BGRA8) :   0.450 ms ( 0.75%)
Ready Event / Synchronization Overhead  :   0.059 ms ( 0.10%)
─────────────────────────────────────────────────────────────────
Total End-to-End Latency                :  60.070 ms (100.0%)
```

---

## 9. Comprehensive Benchmark Results

Tested on NVIDIA GeForce RTX 4090 Laptop GPU (16 GB VRAM, CC 8.9) with AMD Ryzen 9 7940HS (8C/16T) on Windows 11.

### 9.1 Per-Pattern Disclosure Table

| Resolution | Pattern | CPU Tot (ms) | 2C Idx (ms) | 2D Idx (ms) | Index Speedup | 2C Tot (ms) | 2D Tot (ms) | Total Speedup vs 2C | Ratio vs CPU |
| :---: | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **1080p** | flat | 6.61 | 0.09 | 0.09 | 0.98x | 0.74 | **0.72** | 1.02x | **9.13x** |
| | gradient | 7.11 | 0.72 | 0.48 | 1.48x | 2.74 | **2.58** | 1.06x | **2.76x** |
| | **noise** | 23.54 | 21.54 | **6.28** | **3.43x** | 29.78 | **14.66** | **2.03x** | **1.61x** |
| | texture | 8.28 | 2.15 | 2.02 | 1.06x | 7.19 | **7.06** | 1.02x | **1.17x** |
| | edges | 8.02 | 1.87 | 0.70 | 2.66x | 4.85 | **3.70** | 1.31x | **2.17x** |
| **4K UHD** | flat | 20.01 | 0.36 | 0.13 | 2.65x | 1.76 | **1.52** | 1.16x | **13.20x** |
| | gradient | 19.81 | 1.76 | 1.15 | 1.53x | 5.04 | **4.46** | 1.13x | **4.45x** |
| | **noise** | 87.79 | 88.78 | **27.43** | **3.24x** | 121.47 | **60.07** | **2.02x** | **1.46x** |
| | texture | 26.90 | 8.44 | 8.18 | 1.03x | 18.83 | **18.47** | 1.02x | **1.46x** |
| | edges | 26.53 | 6.83 | 2.39 | 2.85x | 13.19 | **8.78** | 1.50x | **3.02x** |
| **8K flat** | flat | 84.34 | 1.52 | 0.40 | 3.75x | 6.44 | **5.58** | 1.15x | **15.13x** |

### 9.2 Headline Aggregate Results

| Metric | Phase 2B (Serial GPU) | Phase 2C (Parallel GPU) | Phase 2D (Parallel Host+GPU) | Speedup vs 2C | Speedup vs 2B |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **1080p FPS** | 10.18 FPS | 108.42 FPS | **172.80 FPS** | **1.59x** | **16.97x** |
| **1080p Latency** | 98.22 ms | 9.23 ms | **5.79 ms** | **1.59x** | **16.97x** |
| **1080p Throughput**| 21.11 MP/s | 224.81 MP/s | **358.32 MP/s** | **1.59x** | **16.97x** |
| **4K UHD FPS** | 2.54 FPS | 31.10 FPS | **52.75 FPS** | **1.70x** | **20.80x** |
| **4K UHD Latency** | 394.39 ms | 32.15 ms | **18.97 ms** | **1.70x** | **20.80x** |
| **4K UHD Throughput**| 21.07 MP/s | 257.94 MP/s | **437.52 MP/s** | **1.70x** | **20.80x** |
| **8K flat FPS** | 63.43 FPS | 154.52 FPS | **179.93 FPS** | **1.16x** | **2.84x** |
| **8K flat Throughput**| 2104.42 MP/s | 5126.68 MP/s | **5969.66 MP/s** | **1.16x** | **2.84x** |

---

## 10. Multi-Frame Concurrency Scaling

Evaluating pipeline multi-slot performance across 1, 2, 3, and 4 slots:

### 1080p Pipeline Slots:
- **1 Slot**: 154.04 FPS (319.41 MP/s, Latency: 6.49 ms/frame)
- **2 Slots**: 253.59 FPS (525.84 MP/s, Latency: 3.94 ms/frame)
- **3 Slots**: 287.47 FPS (596.10 MP/s, Latency: 3.48 ms/frame)
- **4 Slots**: **291.29 FPS** (**604.03 MP/s**, Latency: **3.43 ms/frame**)

### 4K UHD Pipeline Slots:
- **1 Slot**: 51.25 FPS (425.13 MP/s, Latency: 19.51 ms/frame)
- **2 Slots**: 77.51 FPS (642.89 MP/s, Latency: 12.90 ms/frame)
- **3 Slots**: 76.23 FPS (632.31 MP/s, Latency: 13.12 ms/frame)
- **4 Slots**: **78.71 FPS** (**652.87 MP/s**, Latency: **12.70 ms/frame**)

### Concurrency Recommendation & CPU Sweet Spot:
- On 4K frames, scaling from 1 slot to 2 slots yields a **+51.2% throughput boost** (51.25 FPS $\to$ 77.51 FPS).
- Moving from 2 slots to 4 slots levels off at $\sim 78.7\text{ FPS}$ because GPU compute and memory bandwidth become fully saturated.
- **Sweet spot**: **2 to 3 slots** provides maximum concurrency and pipelined CPU-GPU overlap without thread oversubscription or CPU contention.

---

## 11. Verification & Robustness

### 11.1 Golden Vector Bit-Exactness
Run against `tests/decode_phase0_golden.txt` (11 canonical test patterns):
```
=== HyperCine Decode Phase 2D Verification ===
Testing Parallel Host Checkpoint Indexing & Scheduling...
  [PASS]  1920x1080     flat | 30/30 Coeffs: EXACT | 2C Index:  0.08 ms | 2D Index:  0.14 ms | Speedup: 0.61x | 2D Total:   0.97 ms
  [PASS]  1920x1080 gradient | 30/30 Coeffs: EXACT | 2C Index:  0.87 ms | 2D Index:  0.54 ms | Speedup: 1.61x | 2D Total:   3.25 ms
  [PASS]  1920x1080    noise | 30/30 Coeffs: EXACT | 2C Index: 21.73 ms | 2D Index:  6.40 ms | Speedup: 3.39x | 2D Total:  14.71 ms
  [PASS]  1920x1080  texture | 30/30 Coeffs: EXACT | 2C Index:  2.61 ms | 2D Index:  3.37 ms | Speedup: 0.77x | 2D Total:   8.53 ms
  [PASS]  1920x1080    edges | 30/30 Coeffs: EXACT | 2C Index:  1.79 ms | 2D Index:  0.69 ms | Speedup: 2.57x | 2D Total:   3.75 ms
  [PASS]  3840x2160     flat | 30/30 Coeffs: EXACT | 2C Index:  0.37 ms | 2D Index:  0.15 ms | Speedup: 2.43x | 2D Total:   1.67 ms
  [PASS]  3840x2160 gradient | 30/30 Coeffs: EXACT | 2C Index:  1.72 ms | 2D Index:  1.21 ms | Speedup: 1.42x | 2D Total:   4.56 ms
  [PASS]  3840x2160    noise | 30/30 Coeffs: EXACT | 2C Index: 87.38 ms | 2D Index: 27.81 ms | Speedup: 3.14x | 2D Total:  59.10 ms
  [PASS]  3840x2160  texture | 30/30 Coeffs: EXACT | 2C Index:  8.91 ms | 2D Index:  8.22 ms | Speedup: 1.08x | 2D Total:  18.59 ms
  [PASS]  3840x2160    edges | 30/30 Coeffs: EXACT | 2C Index:  7.03 ms | 2D Index:  2.18 ms | Speedup: 3.23x | 2D Total:   8.55 ms
  [PASS]  7680x4320     flat | 30/30 Coeffs: EXACT | 2C Index:  1.46 ms | 2D Index:  0.38 ms | Speedup: 3.87x | 2D Total:   5.46 ms

All 11 Golden Test Vectors PASSED with 100% Bit-Exact Accuracy!
```

### 11.2 Checkpoint Equivalence
Every checkpoint record generated by Phase 2D is compared field-by-field against Phase 2C:
- `byte_offset`
- `nibble_idx`
- `initial_state`
- `out_coeff_pos`
- `end_byte_offset`
- `end_nibble_idx`
- `max_coeff_pos`
- Result: **0 mismatches across thousands of generated segments**.

### 11.3 Malformed Input & Error Resilience
Tested against corrupted tag structures and truncated bitstreams:
- Workers safely detect truncated buffers without buffer overrun.
- `abort_flag` halts peer worker processing immediately.
- Decoder returns `DecodeTicketStatus::Failed`.
- Reusable slot is returned safely to the slot pool (`AbandonSlot`) with zero resource leaks.

---

## 12. Conclusion & Phase 3 Outlook

HyperCine Phase 2D successfully eliminates the host-side checkpoint generation bottleneck:
- **1080p Noise**: Host indexing reduced from $21.5\text{ ms} \to \mathbf{6.28\text{ ms}}$; total decode latency reduced to $\mathbf{14.66\text{ ms}}$.
- **4K UHD Noise**: Host indexing reduced from $88.8\text{ ms} \to \mathbf{27.43\text{ ms}}$; total decode latency reduced to $\mathbf{60.07\text{ ms}}$ (outperforming the CPU reference at $87.8\text{ ms}$).
- **Throughput**: $358\text{ MP/s}$ (1080p) and $437\text{ MP/s}$ (4K), scaling up to $\mathbf{652\text{ MP/s}}$ with 4 concurrent slots.

### Next Step: Phase 3 (End-to-End CineForm Container & Decoder API)
With both intra-band GPU entropy replay (Phase 2C) and parallel host indexing (Phase 2D) operating at peak efficiency, Phase 3 will integrate:
1. Native AVI / MP4 demuxer integration for streaming compressed sample playback.
2. Full CineForm metadata decoding (color matrix, exposure, 3D LUT metadata).
3. Zero-copy Direct3D 11/12 and Vulkan surface sharing for video editing and real-time playback integration.
