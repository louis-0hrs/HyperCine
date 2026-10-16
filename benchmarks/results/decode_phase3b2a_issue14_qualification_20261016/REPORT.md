# Issue #14 qualification results

Dual-licensed under MIT OR Apache-2.0.

This report records the final performance, resource, and provenance qualification for Issue #14 on 2026-10-16. The primary checkout and job.txt were left untouched. The benchmark branch contains no production decoder changes.

## CUDA build and C1902

Baseline f1d0fe4258ad70113a9de57e85ab9834d29312cc and current 1d78fc183ea28c4268345521e83fb2bca3226381 were configured in separate clean worktrees with Visual Studio 17 2022 x64, v143 host=x64, Release, CUDA Toolkit 12.8.61, MSVC 19.44.35229.0, CUDA_ARCHITECTURES=89, and CUDA/upstream/tests/benchmarks enabled. Both CUDA compiler-identification and Release benchmark builds completed.

C1902 was reproduced only in the restricted process environment during CUDA compiler-ID/PDB creation (/Zi /RTC1 /FS, Debug/vc143.pdb). With normal local process access, both isolated configurations and builds passed. No decoder change was made.

## Progressive 1080p boundary

The former +5.144% progressive flat-Automatic case was rerun in two A-B-B-A cycles. Twelve 100-request block means per build yield a median change of +0.647% (1.053925 ms to 1.060740 ms). The approximate 95% interval for the mean difference is [-0.0083, +0.0453] ms, below the +0.052696 ms 5% limit. AC was checked before and after each timed repeat. Gate: PASS.

## Interlaced GOP-1

Three A-B-B-A cycles were run with the same new test-only harness and same frozen YUY2/FilmScan3 interlaced sample. The timer includes QueueDecode, WaitForDecode, device-to-host download, and ReleaseOutput. Each side has 600 requests per strategy. Sample hash 356ad5c204104b7d and upstream BGRA output hash 41a4e58b09555e3e matched on every request. AC was checked per block. The final cycle recorded GPU clocks, power, temperature, and P-state at roughly one-second intervals. One idle power.draw sample was a 593.51 W telemetry outlier; loaded samples were approximately 32–36 W. No strategy regressed: mean changes were -0.061% Lightweight, -0.291% Hybrid, and -0.157% Automatic. Gate: PASS.

The shared harness SHA-256 is DC742E168C912A2392C167966B72A0B2FEE5B7ECBBD555B57610E920562DAD96; the benchmark CMake SHA-256 is DA32A289D5B3B0BAE01DC47F712D64F00C7414B3F4A35BBB022F63A5A84CBEA2. Both matched across the two builds.

## Memory, allocations, and stage timing

The documented lazy per-slot FIELDPLUS allocation policy is retained. First-use growth and latency are recorded for 1080p and 4K, with zero warmed steady-state allocation growth. The source-level resource audit records 13 streams and 197 events per slot (zero change from baseline); runtime diagnostics do not expose native handle counters. 1080p and 4K memory snapshots, fresh 4K exactness, 1080p latency, and 5,000-output mixed stress are in resources/.

Interlaced GOP-1 stage timings were collected separately and matched the upstream BGRA output. FIELDPLUS per-stage timing is deferred because the current diagnostics do not expose those stages; Issue #14 explicitly makes this a non-blocking follow-up.

The complete CUDA CTest suite passed 44/44 in 767.02 s. upstream/ remained unchanged.

## Decision

PASS — Issue #14 performance, resource, and provenance gates. The progressive boundary and interlaced end-to-end gates are below the 5% limit, memory and warmed-allocation evidence is recorded, and both compared commits used verified CUDA-enabled builds with a byte-identical test harness.

PR #12 should remain Draft/HOLD pending the separate Compute Sanitizer and host memory-safety follow-ups tracked by Issues #13/#38. This result does not authorize merging, release, or promotion.