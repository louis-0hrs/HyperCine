<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under the MIT License or Apache License, Version 2.0.
     See LICENSE-MIT and LICENSE-APACHE for details. -->

# Independent telemetry source audit (#115)

Audit date: 2026-10-17. Baseline: main
`e87ef0258ba37f5c78f1557c59d9c7331df5521c`. Adapter reviewed separately:
[PR #117](https://github.com/louis-0hrs/HyperCine/pull/117),
`b33617cd7856b134b44d8fe337146e480905bd7c`.
This document records source observations and implementation handoff points;
it does not introduce a competing schema or collector.

The adapter owner retains implementation of
[issue #115](https://github.com/louis-0hrs/HyperCine/issues/115).
Actual Tesla M4 correctness investigation is separately assigned in
[issue #116](https://github.com/louis-0hrs/HyperCine/issues/116).
No M4 error code, root cause, capacity result or GPU performance is established
by this CPU-only audit. The incident must not be classified as OOM from a memory
snapshot or GPU capacity alone.

## Reusable measurements and their actual semantics

All source links below refer to the baseline SHA above. Symbols, rather than
line numbers, identify the inspected implementation.

| Source / symbol | Observed behavior | Consequence for the diagnostic collector |
| --- | --- | --- |
| [phase3a.h](../include/hypercine/phase3a.h), `Phase3AResult` | Holds submit-to-complete mean, median, p95 and **p99** latency, stage times, transfer byte counts, in-flight frames and memory snapshots. | Reuse these values; p99 already exists and needs serialization, not a second percentile implementation. |
| [phase3a_encoder.cpp](../src/core/phase3a_encoder.cpp), `RunPhase3A` | Computes p99 from sorted measured latencies using nearest rank `ceil(0.99*n)-1`. Records completion-period statistics separately. | Keep frame latency distinct from completion period and from throughput. Preserve the existing percentile definition. |
| [benchmark_phase3a.cpp](../benchmarks/benchmark_phase3a.cpp), `WritePhase3AResultJson` | Serializes median/p95 latency and many stage/memory fields, but omits p99. | The missing JSON p99 is an export gap, not a missing measurement. |
| [phase2a_transfer.cu](../src/cuda/phase2a_transfer.cu), `HyperCine_QueryUsedVramBytes` | Reads `cudaMemGetInfo`, returns `total-free`, returns zero on query failure. | Existing zero cannot distinguish failed measurement from genuine zero use. Collect free/total plus success/error directly at the diagnostic boundary. |
| [phase3a_encoder.cpp](../src/core/phase3a_encoder.cpp), `RunPhase3A` memory accounting | `vram_used_bytes` is a positive init-minus-before delta, otherwise `total_estimated_vram_bytes`. `slot_vram_bytes` is a formula multiplied by slots. | Neither field is a reliable measured process allocation peak. Preserve an explicit estimated/measured origin; never relabel the fallback as a measured delta. |
| Same file, `QueryProcessWorkingSetBytes` | Reads Windows `WorkingSetSize`; returns zero on other platforms or query failure. | `host_rss_*` names do not imply a portable RSS implementation. Use null plus a reason outside the supported provider. Windows working set is not private memory. |
| Same file, `ProcessCpuSeconds` / `CurrentThreadCpuSeconds` | Read Windows process/thread CPU times; return zero outside Windows or on failure. | CPU utilization also needs provider availability, not just NVML counters. |
| [benchmark_codec.cpp](../benchmarks/benchmark_codec.cpp), `GetPeakMemoryReport` | Windows peak working set; Linux `VmHWM` converted from KiB; unavailable elsewhere. Read after encode and decode. | Preserve OS metric and process-lifetime scope; the value can include earlier resolutions. It is not per-operation private RAM. |
| [phase2a_transfer.cu](../src/cuda/phase2a_transfer.cu), `HyperCine_QueryDeviceLink` | Reads model, compute capability, total physical VRAM, driver/runtime versions and device attributes. Some individual return codes are ignored. | Reuse the querying seam with per-field status. Do not export the entire struct: it also holds PCI bus/device identifiers excluded by #115. Runtime version does not establish the selected cubin/PTX architecture. |

## Collector gaps that can otherwise produce misleading evidence

### NVML availability, platform and sampling

`NvmlRuntimeSampler` in `src/core/phase3a_encoder.cpp` uses a Windows-only
dynamic library loader. Other platforms leave it unavailable. Device lookup
uses index zero; the CUDA query helper also queries device zero. A future
selectable-device run must bind telemetry to the actual execution device
instead of assuming both indexes identify it.

`SampleOnce` zero-initializes each sample. A missing function or a failed NVML
call leaves its field zero, but the sample is still appended. `Stop` aggregates
all samples and reports one overall availability flag. Consequently, overall
availability plus zero power, temperature, clocks or throttle mask is not proof
that each metric was measured. Retain per-counter success counts/reasons and
exclude unavailable samples from each counter's aggregate.

The sampling thread sleeps **15 ms**, and the vector grows until stopped.
This is neither the requested configurable 250–1000 ms diagnostic interval nor
a bounded sample inventory. Reuse the queries and names, but add opt-in interval
and sample limits at the collector boundary. Do not claim low overhead without
measuring the same small workload with telemetry off and on.

`pipeline_gpu_stage_occupancy_estimate_pct` is a ratio derived from pipeline
stage time and frame period. It is not an occupancy measurement from a kernel
profiler. NVML GPU/memory utilization is device-wide; neither counter establishes
cache misses or achieved DRAM bandwidth.

### Memory stages and failure handling

`RunPhase3A` takes its first VRAM snapshot before explicit encoder setup, its
init snapshot after session/slot/input allocation, and its mid-run snapshot at
a submission index. The first snapshot itself calls CUDA; it does not establish
a pre-CUDA-context baseline. These do not independently distinguish context
initialization, session allocation and first-frame costs. The mid-run snapshot
is not a synchronized stage boundary or a high-water measurement.

Normal pipeline teardown records end and after-destroy snapshots. Several
earlier initialization/allocation errors return before that common sequence.
Later pipeline errors can reach teardown snapshots but do not acquire a named
immediate failure snapshot before cleanup. A diagnostic collector therefore
needs a failure-safe finalization path without assuming every returned result
has all snapshots.

The explicit `cudaDeviceSynchronize` failure branch stores a generic message
and discards `sync_err` from the public result. Other wrappers also return
generic failures. Preserve the first numeric and symbolic error at its source,
with failing operation/stage, before telemetry queries or cleanup can overwrite
the evidence. Telemetry provider errors must remain secondary.

Use signed global deltas. For synthetic snapshots with total=1000 bytes and
free=800, 600, 900, 850 bytes, used is 200, 400, 100, 150 bytes and delta from
start is 0, 200, -100, -50 bytes. A negative delta is valid when another device
user frees memory; it must not trigger a positive estimate fallback. Allocation
category/reserved/active/high-water counters need allocator tracking. Neither
these snapshots nor the slot sizing formula supply that evidence.

### A bounded driver is still required

`benchmark_phase3a.cpp::main` runs a broad sequence, including 4K/8K matrices
and stability runs of 1000 frames at 1080p, 300 at 4K and 100 at 8K. Do not launch
that program as the default incident probe. `RunPhase3A` has a per-run config
seam, but the diagnostic driver still needs explicit geometry/frame/duration
bounds, an opt-in telemetry mode and safe headroom checks before device work.
Export actual effective warmup: generated input can increase configured warmup
to at least the selected slot count.

## Independent adapter check and handoff

PR #117's `adapt_cpu` is a post-processing allowlist, not a runtime collector.
Its source SHA/tree state remain unknown rather than borrowing the converter's
checkout identity. It preserves legacy performance values, labels success
`UNKNOWN`, excludes arbitrary system metadata, and uses exclusive output
creation. `memory_samples` accepts signed global changes with a 256-snapshot
limit. `failure_record` only formats a supplied error; it does not intercept
CUDA execution or produce a failed-run report on its own.

The adapter's existing ten synthetic tests were independently run on native
macOS ARM64 / Python 3.9.6 at the exact pinned PR revision: **10/10 PASS**.
The scripts were extracted unchanged outside the repository; no adapter or
test implementation files are included in this documentation branch.
One inventory correction for the owner: Phase3A p99 is already computed,
as shown above. Full schema validation, actual error capture, GPU telemetry
availability and overhead remain separate unfinished #115 gates.

On the documentation branch, native macOS 26.5.2 ARM64 / AppleClang 21.0.0 /
CMake 3.31.5 Release standalone build passed. Full registered CTest with
required FFmpeg/ffprobe n8.0 external decode: **28/28 PASS**, no skips,
76.59 seconds. Privacy unit tests: **13/13 PASS**; standalone CI inventory
tests: **6/6 PASS**. Reproduce from this branch with FFmpeg/ffprobe on PATH:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_TESTS=ON \
  -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --parallel 3
ctest --test-dir build -C Release --output-on-failure
python3 ci/test_privacy_guard.py
python3 ci/test_standalone_gate.py
```

Suggested collector verification order:

1. Add synthetic tests for individual unavailable NVML/CPU counters, signed
   deltas, estimate-vs-measurement labels, sample limits and early failures.
2. Exercise one CPU-only path with GPU fields null and a valid partial summary
   after a deliberate non-OOM failure. Preserve the original failure code.
3. On an available native CUDA device, use one bounded small workload and
   compare telemetry off/on with identical correctness checks. Export p99 and
   actual effective warmup from existing runtime measurements.
4. Let the #116 hardware owner qualify the M4 incident independently; do not
   wait for a wholesale monitoring framework or start high-resolution stress
   before deterministic correctness. Record only measured hardware results.

Publish only allowlisted summaries after the existing privacy checks and
manual review in [PRIVACY.md](PRIVACY.md). Keep raw diagnostics outside Git.
This audit makes no optimization, sanitizer, native CUDA/NVML or M4 pass claim.
