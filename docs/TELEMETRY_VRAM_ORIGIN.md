<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Measured VRAM versus estimated memory — issue #115

Parent: PR #121, `07f2b8fcf7420881aa841cfaded1568d47bb43a8`.

Phase3A previously replaced a zero or decreasing observed VRAM delta with the
slot-selection estimate. The canonical summary then called that value measured.
A failed `cudaMemGetInfo` query also became zero with no usable validity signal.

The before/init queries now retain success separately. The new optional
`Phase3AResult::measured_global_vram_delta_bytes` preserves positive, zero and
negative global device-use changes; it is absent when either query fails or
the difference is outside the signed representation. It is not process-owned
memory, a per-buffer allocation counter or a sampled high-water peak. Other
workloads can change the measured delta.

Both real JSON writers share the same memory serializer:

- `measured_global_vram_delta_bytes`: signed integer or null.
- `vram_measurement_scope`: `global_device_usage_delta`.
- `total_estimated_vram_bytes`: unchanged mathematical slot-selection estimate,
  always distinct from measured data.
- Compatibility `vram_used_bytes`: positive part of the measured delta, or null
  when unavailable; **never** a fallback estimate. In-memory legacy storage
  remains unsigned/zero when unavailable; consumers must check the optional.

Canonical trial summaries count successful measurements only and replace the
misleading `measured_vram_used_bytes_median` key with
`measured_global_vram_delta_bytes_median` (signed, null if no valid trials),
plus `measured_vram_trial_count`. Consumers of the experimental JSON must adopt
the new summary key and handle nullable memory readings. Historical JSON is not
rewritten. Legacy raw before/mid/end snapshot fields retain their existing
zero/error ambiguity; this change qualifies only the before/init delta.

## Verification

The actual production helper and both extracted real writers are compiled in
the existing synthetic-provider test. It verifies positive/zero/negative
changes, successful zero snapshots, independently missing endpoints, stale
state reset and signed-range bounds. Strict Python parsing checks eight memory
records, including separate estimates, nullable values and unique JSON keys.
The existing NVML/p99 coverage remains binding.

## Cross-trial aggregation and consumer scope (review `6048815961`, item 2)

`detail::VramDeltaSummary` is the single aggregation used by the canonical
slot-scaling summary. It counts only successful measurements, keeps a measured
zero as a real observation, preserves negative global deltas, and exposes the
values in trial order so the writer can publish the signed median. Mixed
valid/invalid trials therefore keep the success denominator instead of silently
shrinking it to the valid ones. The C++ test drives it directly with the same
four trials the writer emits (50, 0, −50, missing) plus an all-negative set; a
summary that clamps negatives or drops zeros fails.

The Python contract no longer uses `assert`: every check raises, so the file
means the same thing under `python -O`. Both modes are registered in the strict
standalone inventory (`phase3a_telemetry_json`, `phase3a_telemetry_json_optimized`),
and the verifier is exercised against eight malformed or incorrect writer
outputs (duplicate key, dropped case, sign flip, estimate substituted for a
failed measurement, removed nullable field, an absent provider reported as
available, a measured zero reported as missing, and an erased valid counter). A
verifier that stops looking now fails those controls instead of passing.

**Intentionally limited consumer scope.** Three consumers are covered: the two
extracted writers and the canonical summary. Deliberately *not* covered:
historical JSON already on disk (not rewritten), the Phase 2C/2D
`vram_used_bytes` fields (a different result type, still the unsigned legacy
estimate, unchanged by this work), and the M4 SM52 probe, which reads the
Phase 2C structure and never parses Phase 3A telemetry JSON. Any consumer that
starts reading `phase3a` memory records must treat
`measured_global_vram_delta_bytes` and the compatibility `vram_used_bytes` as
nullable and must not fall back to `total_estimated_vram_bytes`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_UPSTREAM=OFF -DHYPERCINE_BUILD_BENCHMARKS=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_BUILD_STANDALONE=ON \
  -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --parallel 4
ctest --test-dir build -C Release --output-on-failure
python3 -m unittest discover -s ci -p 'test_*.py'
```

FFmpeg/ffprobe must be discoverable or supplied through the existing CMake
executable variables. Native macOS ARM64, AppleClang 21, CMake 3.31.5,
Python 3.9.6, FFmpeg n8.0. Exact counts are in the PR handoff.
No NVIDIA toolkit/device is available here: the full CUDA translation unit and
real `cudaMemGetInfo` statuses remain **UNTESTED** on native CUDA. A short native
NVIDIA run, including a failed-query diagnostic and same-head CUDA build/tests,
is required before accepting this collector change. No allocation policy,
codec math, bitstream, upstream source or performance claim changes.

## Forward adaptation onto current main (2026-11-24)

This change was recovered from the reviewed stack ``17d60ca` (#124)` and forward-adapted
onto protected `main` `3c7030facd6edfbd604097682ece5ce0b91c47fd`; the stack
branches themselves are untouched. Conflict-free against current main apart from
the `.gitignore`/root `CMakeLists.txt` allowlist and registration points, which
were merged by hand (Vulkan option kept, telemetry registration added after it).

Environment of this adaptation: Linux x86_64, GCC 14.2.0, CMake 3.31.6,
Ninja 1.13, Python 3.13.5, FFmpeg 7.1.5. Full registered strict SDK-free suite
**91/91 PASS** (72.78 s). Both telemetry executables pass, including the strict
Python writer checks, also under Debug ASan+UBSan (2/2, 1.92 s). CI Python
discovery is 524 tests with **one pre-existing failure** on unmodified
`3c7030fa` (`compute_policy.tests.test_routing_scope.MacScopeTests.
test_native_job_steps_unchanged`, a recorded `ci-cpu.yml` job-step hash), which
this change does not touch.

Causal negative controls, each a one-line mutation of the restored fix in this
tree, all fail the suite at this head:

| Mutation | Failing assertion |
|---|---|
| init guard without the `nvmlShutdown` symbol | `missing required lifecycle symbol` |
| zero/decreasing delta replaced by the slot estimate | `measured zero is not estimate` |
| accumulator that retains every poll | `bounded streaming storage: million polls must not retain per-poll samples` |

The third control exposed a real gap in the recovered test: `sizeof()` of the
aggregate cannot see a heap container, so a growing vector passed it. The bound
is now measured as process RSS growth across one million polls
(`< 8 MiB`). That measurement is Linux-local (`/proc/self/statm`); on Windows and
macOS only the fixed-size check runs, and the storage claim is reported as
Linux-proven.

**Hardware status: NOT_RUN.** No usable NVIDIA device is present on this host.
Native NVML/CUDA execution, real driver return codes, a CUDA-enabled full build
and a Tesla M4 8K trace stay open and are not implied by these numbers.
