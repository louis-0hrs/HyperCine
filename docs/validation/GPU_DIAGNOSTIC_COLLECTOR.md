# Bounded GPU diagnostic collector (#115)

Status: `MEASURED / PARTIAL` — actual native Tesla M4 evidence for the
observability slice, not a codec, performance or release qualification.

## Scope

`benchmarks/gpu_diagnostic_probe` wraps a short `RunPhase3A` call with:

- build/runtime provenance (source commit, compiler, build type, CUDA
  compiler/runtime/driver, backend);
- device facts that are safe to publish (GPU model, compute capability,
  multiprocessor count, total VRAM — no UUID, serial or PCI bus identifier);
- `cudaMemGetInfo` free/total/used snapshots before and after the run;
- the already-measured Phase3A VRAM snapshots (before allocation, after
  initialization, mid-run, end of run, after destruction) and the
  phase-estimated per-slot/workspace totals;
- host working-set snapshots;
- a versioned partial JSON report on failure, including exact CUDA error
  code/name plus the Phase3A error text.

The sidecar reuses existing Phase3A field names. It adds no second benchmark
contract and does not change codec behavior or the existing benchmark JSON.

Explicitly **not** collected here:

- process-owned allocator high-water bytes (requires allocator hooks);
- the failing CUDA call site inside `RunPhase3A` (`failing_operation` is
  reported as unavailable rather than guessed);
- NVML per-counter validity, sampling bounds, p99 serialization and
  measured-vs-estimated Phase3A writer changes (owned by the separate
  telemetry adapter work).

## Schema (version 1)

Top-level keys: `schema`, `schema_version`, `status`, `ok`, `provenance`,
`device`, `workload`, `memory`, `failure`, `unavailable_metrics`.

Unavailable values are emitted as JSON `null` with an entry in
`unavailable_metrics`; they are never reported as zero. Estimated values live
under `memory.estimated` and use the `*_vram_bytes` names, while measured
values are named `measured_*`.

`memory` contains:

- `cuda_mem_get_info[]`: device-wide free/total/used at probe boundaries;
- `result_vram_used_snapshots[]`: Phase3A used-byte snapshots;
- `measured_vram_delta_bytes`: the result-reported
  `after_init − before` delta;
- `measured_peak_device_delta_bytes`: maximum snapshot minus the
  pre-allocation snapshot. This is **device-wide** (includes the probe context
  and any other device user), not process-owned;
- `estimated.*`, `slot_vram_bytes`, `slot_pinned_host_bytes`,
  `host_rss_before_bytes`, `host_rss_peak_observed_bytes`.

## Build and run

```bash
cmake -S . -B <build-dir> -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_ENABLE_CUDA=ON -DHYPERCINE_BUILD_TESTS=ON \
  -DHYPERCINE_BUILD_BENCHMARKS=ON -DHYPERCINE_BUILD_UPSTREAM=ON \
  -DCMAKE_CUDA_ARCHITECTURES=52 \
  -DCMAKE_CUDA_COMPILER=<cuda-12.8>/bin/nvcc.exe
cmake --build <build-dir> --target gpu_diagnostic_probe
<build-dir>/benchmarks/gpu_diagnostic_probe \
  --width=1920 --height=1080 --frames=4 --warmup=2 --out=<out>.json
```

`--out` is required and is validated with the other arguments: otherwise valid
arguments without it exit `2` before any diagnostic run. The report is created
**exclusively**: if the path already exists the probe exits non-zero and leaves
the existing file byte-identical. `--overwrite` is the explicit opt-in that may
replace an existing artifact, and even then a directory is never replaced; an
overwrite that fails mid-write does not restore the previous artifact, which
matches "no partial evidence" but is not a backup. The opened stream must be a
regular file: device paths such as `NUL` (Windows) or `/dev/null` (POSIX) accept
writes while storing nothing, so they are refused instead of producing exit `0`
without an artifact. Exit codes: `0` passed, `2` invalid arguments (a partial
JSON is still written when the output path is new), `3` device or run failure,
or an output refusal after valid arguments. An argument error keeps exit `2`
even when its report cannot be written (for example an occupied path); the
argument error and the write refusal are both named on stderr, so `3` always
means the arguments themselves were accepted.

Options accept both `--name=value` and `--name value`. The separated form never
consumes a following token that begins with `--`, so `--out --overwrite` is a
missing `--out` value rather than a report named `--overwrite`; a path that
really begins with `--` uses the `--out=<path>` form.

Numeric arguments are bounded because this is a single short diagnostic run:
1–8192 px per dimension, 1–16 frames, 0–16 warmup frames and 0–4 slots.
Tokens outside `int` range are rejected instead of narrowed — `4294967297`
is an error, not `1`.

## Measured evidence

Native Windows x64 QEMU guest, AMD EPYC 7601, 8 logical processors; Tesla M4
compute capability 5.2, 3,953,131,520 B total VRAM; MSVC 19.44.35229.0
Release; CUDA compiler 12.8.93, runtime 12,080, driver 13,000; source commit
`193c238947de07196e9fec93b8b0ae82bb8bdb33` (`source_dirty=false`). The probe
context itself accounts for ~53.5 MB of device-wide usage before allocation.

| Workload | Frames / slots | `measured_vram_delta_bytes` | `measured_peak_device_delta_bytes` | `estimated.total_vram_bytes` |
| --- | --- | ---: | ---: | ---: |
| 1920x1080 FilmScan3, HostPageableEndToEnd | 4 + 2 warmup / 2 | 143,130,624 | **230,686,720** | 150,818,816 |
| 3840x2160 FilmScan3, HostPageableEndToEnd | 1 / 1 | 265,682,944 | **595,722,240** | 303,693,824 |
| 7680x4320 FilmScan3, HostPageableEndToEnd | 1 / 1 | 1,045,692,416 | **2,337,669,120** | 1,099,956,224 |

All three runs returned `PASSED` (exit 0) with a complete JSON report. Device-
wide snapshot sequences (before allocation, after initialization, mid-run, end
of run, after destruction):

- 1080p: `53.5 → 196.7 → 284.2 → 284.2 → 61.4 MB`;
- 4K: `53.5 → 319.2 → 649.3 → 649.3 → 59.6 MB`;
- 8K: `53.5 → 1099.2 → 2391.2 → 2391.2 → 59.6 MB`.

The 8K run used one slot and one frame; it did not reproduce the historical
non-OOM M4 failure and is not an 8K qualification. Peak host working set was
312.5 MB at 1080p, 719.8 MB at 4K and 2,550.3 MB at 8K.

### Observation

The result-reported init delta is **not** a peak:

- 1080p peak delta is 1.61 × the init delta and 1.53 × the phase estimate;
- 4K peak delta is 2.24 × the init delta and 1.96 × the phase estimate;
- 8K peak delta is 2.24 × the init delta and 2.13 × the phase estimate.

The transient between initialization and steady state is therefore the
largest recorded device-wide footprint in these runs. `mid_run` is currently
not serialized by the existing benchmark writers; this sidecar exposes it.
This is a single-run, unpinned-clock VM observation and is not a performance
result, a slot-sizing recommendation, or evidence about the historical
non-OOM M4 failure.

## Failure-path evidence

`--slots=5` produced `status=INVALID_ARGUMENTS`, exit code 2, and a complete
partial JSON with `failure.message` set and CUDA error fields `null` (no CUDA
call was made). Serializer regressions additionally cover a FAILED report
with a CUDA code/name/description, unavailable snapshots, missing measured
deltas, message escaping and deterministic output.

## Tests

`test_gpu_diagnostic_report` (CPU-only, registered with CTest) validates the
schema names, null-vs-zero semantics, measured/estimated separation, partial
failure reports, escaping and determinism. It also covers the writer contract
directly: create-new refuses a pre-existing file byte-identically, explicit
overwrite replaces it, an empty path and a missing parent fail without creating
an artifact, a directory is never replaced, and a device path is refused rather
than reported as a written report.

`gpu_diagnostic_probe_cli` (registered from `benchmarks/CMakeLists.txt`, so it
only exists in CUDA builds) is a real binary-level regression: it runs the
probe against a sentinel artifact on the invalid-arguments and valid-arguments
paths and asserts byte-identical preservation, proves that narrowing tokens
such as `--width=4294967297` are rejected with `INVALID_ARGUMENTS`, checks the
declared bounds for every numeric option, and confirms that only `--overwrite`
replaces an artifact.

2026-10-24 native Windows x64 Tesla M4 re-check at `ccf102dc0adadc86a43faf7895e0a8be41e7d9ac`
(`source_dirty=false`; MSVC 19.44.35229.0 Release; CUDA compiler 12.8.93,
runtime 12,080, driver 13,000): the full configured target set builds,
`test_gpu_diagnostic_report` passes, and `gpu_diagnostic_probe_cli` passes in
7.8 s including real device runs. Separately, a 1920x1080, 1-frame, 1-slot run
returned `PASSED` (exit 0) with report SHA-256
`7035769aa5764a4c895a78b3fd21941b2a3e0462a64eb9fda4871f7e78743d84`; a valid
run against an existing sentinel returned exit 3 and left the sentinel
byte-identical; and `--width=4294967297` returned exit 2 with
`INVALID_ARGUMENTS`. The full decoder suite was not rerun for this test-only
change.

The earlier 2026-10-18 pre-quarantine run, recorded in `e338873`, was **23/25**:
`test_phase2c` and `test_phase2d` failed with the reference-decoder
nondeterminism already documented in #120/#116, not with this change. Bounded
reruns produced distinct signatures: `test_phase2c` failed twice at 8K
DeviceInput (`QueueGpuEntropyFrame failed`) and once at 1080p with PSNR
20.378136; `test_phase2d` failed once at metadata encode and once at the 1080p
upstream comparison. That inventory predates the fail-closed reference-oracle
quarantine: it is historical and must not be read as the current tree's result.

At the source head this change is based on (`87247cc`, 2026-10-24, same native
Windows x64 hardware and toolchain) the suite registers **31** tests, with
**27 passed / 3 skipped / 1 failed**:

- the three skips are `test_phase2a`, `test_phase2b` and `test_phase2d`, which
  exit `BLOCKED_ORACLE_NOT_QUALIFIED` under CTest's `SKIP_RETURN_CODE 77` until
  an independent pixel oracle exists (#120);
- the single failure is `test_phase2c` at the 8K DeviceInput 3-slot x 4-stream
  capacity gate: `UNSUPPORTED_RESOURCE_BOUND`, 33,178,624 B requested against
  23,789,568 B free of 3,953,131,520 B (`cudaErrorMemoryAllocation`), kept fatal
  and not reclassified as a skip.

This diff adds only a benchmark target, its CLI contract test and this
documentation; it does not modify `src/`, the existing test sources or
`upstream/`. A skip is not a pixel pass, no green suite or release claim is
made, and the release gate stays unresolved.

## Limitations

- Device-wide `cudaMemGetInfo`, not process-owned; other device users would be
  included.
- One run per resolution (1080p, 4K, 8K); no soak, sanitizer, clock/power or
  performance qualification.
- Internal Phase3A boundaries record used bytes only, so free/total are
  unavailable there.
- `failing_operation` remains unavailable until the Phase3A call sites carry
  stage-tagged error context.
