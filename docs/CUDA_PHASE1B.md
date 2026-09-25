# HyperCine CUDA Acceleration Phase 1B: Multi-level Pyramid & Quantization

## 1. Pipeline Completion
The Phase 1B acceleration milestones have been successfully completed:
1. **Resident Pyramid:** The GPU now runs multi-level cascaded spatial wavelets back-to-back natively, leaving intermediate LL bands resident in VRAM.
2. **Quantizer Kernel:** We implemented the CineForm `(abs(val) + offset) * mult >> 16` logic in a fused vertical-quantization CUDA kernel.
3. **Bit-Exact Verification:** A new test (`test_cuda_pyramid`) confirms bit-for-bit numerical equivalence to the upstream CPU production path (`FilterSpatialQuant16s`) across all subbands (LL, LH, HL, HH).
4. **Pipeline Benchmark:** The harness (`benchmark_phase1b.cpp`) has been added to test the full GPU-resident pyramid on 4K resolution.

## 2. Benchmark Validation & Profiling Analysis

Using the new harness (`benchmark_phase1b`), we measured the spatial wavelet transform + quantization pipeline across multiple configurations for a 4K image (3840x2160). 

### CPU Performance Baselines
1. **HYPERCINE_CPU_REFERENCE**: The portable, unoptimized 16-bit C++ reference implementation.
2. **UPSTREAM_CPU_PRODUCTION**: A wrapper calling the actual upstream production SIMD implementation (`FilterSpatialQuant16s`), representing the true target for our CUDA kernels to beat.
3. **FULL_UPSTREAM_ENCODER**: The time to run the complete, untouched `CFHD_EncodeSample` pipeline, providing the upper bound of the entire encoding process.

**4K Results (AMD Ryzen 9 7940HS / RTX 4090 Laptop):**
- `HYPERCINE_CPU_REFERENCE`: 265.51 ms (312.39 MP/s)
- `UPSTREAM_CPU_PRODUCTION`: 39.60 ms (2094.30 MP/s)
- `FULL_UPSTREAM_ENCODER`: 400.27 ms (207.22 MP/s)

### GPU-Resident Pipeline Performance
We benchmarked the GPU input -> multi-level pyramid -> quantization, measuring both raw kernel throughput and end-to-end PCIe latency.

**4K CUDA Results:**
- **CUDA Reference (Kernel only):** 2.46 ms (33,683.42 MP/s)
- **CUDA Reference (Host->GPU->Host):** 30.48 ms (2,721.31 MP/s)

### Profiling Insights
- **Speedup over C++ Reference:** The CUDA kernel is over **100x** faster than the portable C++ implementation.
- **Speedup over Upstream SIMD:** The CUDA kernel is **16x** faster than the highly-optimized upstream CPU production SIMD.
- **PCIe Bottleneck:** The End-to-End GPU pipeline (30.48 ms) is severely constrained by PCIe transfer times. The raw compute time is only 2.46 ms, meaning **>90% of the end-to-end time is spent on Host<->Device memory transfers**.
- **Net Gain:** Despite the massive PCIe penalty, the end-to-end GPU pipeline (2,721.31 MP/s) is still **faster** than the upstream production CPU implementation (2,094.30 MP/s). 

## 3. Next Steps & Architectural Boundary
Based on our findings, we must decide where the CPU/GPU boundary should be drawn before integrating the pipeline into the encoder. 
Because the PCIe transfers are the dominant bottleneck, we must pursue an architectural strategy that minimizes memory copies.

**Recommendation (Phase 2): Hybrid/Streaming Pipeline**
The GPU performs the heavy lifting (transform + quantization). The CPU streams the quantized coefficients back asynchronously while performing VLC (entropy coding) on the available subbands in parallel. This keeps the highly serial VLC on the CPU (where it is fast) and the heavy pixel math on the GPU, while aggressively overlapping the PCIe transfer latency with CPU encoding.
