<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Bounded Phase3A telemetry follow-up

Refs #115 and the source audit in PR #118. This change addresses three remaining
benchmark tasks; it does not complete the full diagnostic collector milestone.

1. `benchmark_phase3a` now exports the already-computed `p99_latency_ms` in both
   detailed and compact trial results. The canonical `benchmark_phase3a1` writer
   already exported p99 and retains its existing field names.
2. The Windows runtime NVML sampler replaces its growing sample vector with
   constant-size streaming aggregates. All polls contribute to aggregates;
   no raw sample history is retained. The polling interval is now 250 ms,
   plus start/end boundary polls. Stop wakes the waiting sampler immediately;
   repeated Stop does not add another sample. No CUDA synchronization is added.
3. Each independently queried counter records successful samples separately.
   Failed/missing readings never enter means, extrema, temperature endpoints or
   throttle unions. A successful zero remains numeric zero. Each JSON writer
   emits `null` for a counter with no successful readings and adds
   `counter_status` with per-counter `valid_sample_count` and
   `unavailable_reason` (`provider_unavailable` or `no_successful_samples`).
   Successful counters use a null reason. `sample_count` counts poll attempts;
   compare it with valid counts to detect partially failed runs. Cross-trial
   clock/power averages exclude trials without successful readings. Temperature
   start/end are the first/last successful reading, not fabricated boundary data.

`nvml_available` / JSON `available` now describe successful provider/device
initialization, independently of whether any counter works. Existing numeric
C++ fields remain zero-initialized; consumers must consult the success counts.
JSON consumers must accept null measurements. Rebuild consumers of the extended
result structs. This does not add an NVML loader for Linux/macOS, change the
adapter from PR #117/#119, identify the Tesla M4 failure, preserve exact CUDA
failure context, or fix measured-versus-estimated VRAM accounting.

## Reproduction

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
python3 -m unittest discover -s ci -p 'test_*.py'
```

FFmpeg and ffprobe must be discoverable or configured through their existing
CMake cache options for the external-decode gate. The two new CTests run in
SDK-free and regular builds. Configure extracts the actual sampler and both
actual JSON writers verbatim into generated test includes. Test-only provider
stubs exercise the Windows callback path without loading a DLL or GPU. Tests
cover missing provider, unsupported counters, partial failures, genuine zeros,
success-only averages/extrema/endpoints, throttle unions, repeated Start/Stop,
restart reset, one million deterministic polls and strict JSON parsing that
rejects duplicate keys and checks exact p99 values.

Native NVIDIA execution, real driver return codes, CUDA-enabled full builds,
telemetry overhead and same-device benchmark comparison remain required before
claiming hardware qualification or a performance improvement. Synthetic callback
coverage and successful FFmpeg decoding do not establish those gates.

## Native validation

macOS ARM64, AppleClang 21.0.0, CMake 3.31.5, Python 3.9.6, FFmpeg n8.0:
Release SDK-free build and full required-external-decode CTest **30/30 PASS**, zero
skips (72.51 s). CI Python discovery **19/19 PASS**. Both new telemetry CTests
also pass separately; the same test executable and strict JSON checker pass
under AddressSanitizer plus UndefinedBehaviorSanitizer. Eight emitted JSON
records exercise both actual writers over four provider states. Sanitizer
coverage is limited to the synthetic sampler/writer harness; it is not a full
codec, real NVML, CUDA, driver, or Tesla M4 sanitizer qualification.

## Forward adaptation onto current main (2026-11-24)

This change was recovered from the reviewed stack ``07f2b8f` (#121)` and forward-adapted
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
