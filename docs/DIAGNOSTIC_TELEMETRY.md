<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under the MIT License or Apache License, Version 2.0.
     See LICENSE-MIT and LICENSE-APACHE for details. -->

# Diagnostic telemetry: bounded first milestone (#115)

This is a CPU benchmark adapter and telemetry gap inventory, not completion of
the CUDA diagnostic collector. No Tesla M4 is available in this validation lane.
The previously investigated M4 failure was reported as non-OOM; its cause is
not inferred here. No 8K workload is required or launched by the adapter.

## Existing collectors and missing evidence

| Existing source | Reusable evidence | Remaining work |
| --- | --- | --- |
| `benchmark_codec.cpp` | encode/decode FPS, MPix/s, min/mean/max latency, OS-specific process peak memory | source SHA/tree state, per-frame percentiles, explicit failure contract, bounded frame/warmup controls |
| `phase3a.h::Phase3ANvmlTelemetry`, `benchmark_phase3a.cpp` | device-wide utilization, memory-controller utilization, clocks, temperature, power, throttle mask | per-counter availability; aggregate NVML availability cannot prove every counter succeeded |
| `Phase3AResult`, `benchmark_phase3a.cpp` | before/init/mid/end/destroy VRAM and host RSS, slot/pinned memory, stage timings, median/p95 latency | free/total pairs, allocation-category high-water tracking, first-frame/failure snapshots, p99, original CUDA error identity |
| `src/cuda/phase2a_transfer.cu` | device properties, free/total VRAM, transfer timings | attach provenance and measurements to a bounded actual diagnostic run |

Global device use includes driver and other processes. Sparse snapshots are not
a true peak or process-owned allocation measurement. NVML utilization is not
kernel occupancy, cache misses or achieved DRAM bandwidth.

## Opt-in use

Run an existing CPU benchmark in a local ignored output directory, then adapt
one resolution and operation. The original JSON stays unchanged:

```powershell
New-Item -ItemType Directory -Force build/diagnostics
build/benchmarks/Release/benchmark_codec.exe --1080p-only --json build/diagnostics/cpu.json
python tools/diagnostic_summary.py build/diagnostics/cpu.json build/diagnostics/cpu-v1.json --index 0 --operation encode
python tests/test_diagnostic_summary.py
ctest --test-dir build -C Release --output-on-failure
```

The adapter uses Python standard library only, outside core codec libraries.
CTest includes its tests when Python is available, including standalone builds.
There is no collection thread, GPU call, production-path change or encoding
overhead. Conversion occurs after the benchmark. Output creation is exclusive:
existing files, including the source, cannot be overwritten.

## Version 1 adapter contract

One object contains `schema_version=1`, `status`, `provenance`, `device`,
`workload`, `memory`, `performance`, `samples`, `failure`, `unavailable_metrics`.
Only the existing BGRA / YUV422 / FilmScan3 CPU profile is accepted. Numeric
measurements must be finite and nonnegative; geometry/frame counts are positive
integers. Unsupported profiles and malformed measurements fail conversion.

Legacy CPU JSON has no verified source SHA, dirty flag, warmup count or explicit
failure contract. These values stay null with reasons. Status is `UNKNOWN`,
never inferred as success from the mere existence of JSON. Source revision
cannot be reconstructed from the adapter's current checkout. Percentiles are
not estimated from mean/min/max. GPU values remain null. No system metadata,
CPU brand, arbitrary strings, filenames or environment values are copied.
This allowlist reduces exposure; local privacy scan and manual review are still
required before exporting any report.

`host_peak_bytes` retains `windows_peak_working_set` or `linux_vmhwm` semantics;
an unavailable counter becomes null. It is process-lifetime peak at the end of
both operations, not per-resolution allocation peak or private resident memory.
It can include earlier resolutions. The adapter preserves legacy reported FPS
and latency without reinterpreting their timing boundary.

Two tested building blocks prepare the next collector: `memory_samples` accepts
at most 256 explicitly named stage snapshots, calculates global used bytes and
signed delta from the first snapshot, and retains teardown/failure stages.
`failure_record` preserves a supplied positive CUDA error code, symbolic name
and stage; only code 2 is classified as allocation failure. These helpers do
not collect snapshots or catch runtime errors and are not yet wired into a
CUDA failed-run summary. Tests use synthetic non-OOM code 209 without claiming
that this was the M4 incident's error.

Synthetic fragment illustrating unavailable data and signed global memory
changes (not a hardware result):

```json
{
  "schema_version": 1,
  "status": "UNKNOWN",
  "device": {"backend": "CPU", "gpu": null},
  "performance": {"latency_p99_ms": null},
  "unavailable_metrics": {
    "device.gpu": "cpu_only_adapter",
    "performance.latency_p99_ms": "legacy_cpu_json_has_no_frame_samples"
  }
}
```

Validation of this milestone: native Windows x64, MSVC 19.44, CMake 3.31.6,
Python 3.10.9, CUDA disabled. Full SDK Release build succeeded; CTest passed
11/11 including 10 Python cases. A real 50-frame 1080p CPU benchmark JSON was
converted successfully. Synthetic tests cover unavailable/Linux/Windows host
counters, metadata exclusion, bad profiles/numbers, signed global memory
deltas, stage limits, preserved non-OOM errors and exclusive output creation.
No GPU, sanitizer, M4, telemetry-overhead or optimization qualification is
claimed. Raw local benchmark data and logs remain in ignored `build/diagnostics`.

Next milestone: a native M4 owner should integrate these semantics into a
bounded opt-in Phase3A driver, preserve actual errors and partial summaries,
add provenance/per-counter null reasons, and measure telemetry overhead on the
same small workload. Then qualify one short M4 run with sufficient headroom.
Do not run the current broad Phase3A benchmark as an automatic diagnostic.
