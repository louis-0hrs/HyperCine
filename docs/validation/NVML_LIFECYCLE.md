<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Balanced optional NVML provider lifetime — #115

Child of PR #124, parent 17d60ca241a6fb2ed97e72f2e891812923136832.
The runtime loader initialized NVML even when nvmlShutdown was unavailable.
Destruction then unloaded the DLL without releasing a successful initialization.
The optional provider now requires init, shutdown and device lookup entry points
before invoking init; incomplete providers remain unavailable and cannot abort
encoding.

The synthetic boundary compiles the actual extracted production sampler. Tests
count library loads/unloads and initialization/shutdown calls for missing required
symbols, initialization failure and device lookup failure. The new missing-shutdown
vector failed before the guard. Existing sampler restart/Stop, independent counter
failures, one-million-poll bounded storage and both actual JSON writers remain
covered. Device lookup failure after successful init still shuts down exactly once;
failed init never invokes shutdown.

Native NVIDIA/NVML and full CUDA translation-unit execution are unavailable here.
This qualification is limited to the actual sampler against synthetic callbacks;
a short native NVIDIA build/run remains required before hardware acceptance.
No codec math, allocation policy, GPU performance or bitstream changes are claimed.

```sh
ctest --test-dir build -C Release --output-on-failure
ctest --test-dir build-asan -C Debug -R '^phase3a_telemetry' --output-on-failure
```

Native test environment: macOS ARM64, AppleClang 21, CMake 3.31.5, Python 3.9.6,
FFmpeg n8.0. Full Release configuration uses HYPERCINE_BUILD_STANDALONE=ON,
HYPERCINE_BUILD_UPSTREAM=OFF, HYPERCINE_ENABLE_CUDA=OFF,
HYPERCINE_BUILD_BENCHMARKS=OFF and HYPERCINE_REQUIRE_EXTERNAL_DECODE=ON.
Supply FFmpeg/ffprobe through PATH or the existing CMake executable cache options.
No protected-main merge or release; frozen upstream remains unchanged.

Native CI Python: 19/19 PASS. Debug ASan+UBSan actual-sampler/both-writer
CTest: 2/2 PASS, 1.29 seconds. This is synthetic provider coverage only.
Full native SDK-free Release required-external-decode CTest: 30/30 PASS,
zero skips, 76.33 seconds.

## Forward adaptation onto current main (2026-11-24)

This change was recovered from the reviewed stack ``951f371` (#132)` and forward-adapted
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

## Device binding and the 2026-11-27 review (item 1 and 3)

The provider used to resolve device index `0` unconditionally. On a host with
reordered visible devices or more than one GPU that can sample a different
physical card than the workload runs on. The sampler now takes the locally
observed identity of the active compute device (`cudaGetDevice` +
`cudaDeviceGetUuid`, formatted to the provider's own `GPU-<uuid>` text) and binds
only when `detail::SelectNvmlDeviceIndex` finds exactly one provider device with
that identity. Nothing about the identity is written to any report, log or JSON
field: only the resulting availability is published. Missing, empty, unmatched,
duplicate or over-counted enumerations all fail closed to `available = false` with
zero samples, so an unprovable mapping can no longer masquerade as a measurement.

Synthetic coverage at this head: two devices in reversed provider order (index 0
is the inactive card), an unmatched identity, an empty identity, a duplicated
identity, an enumeration failure, an identity-query failure, an empty host, and
the bound-index happy path. Mutating the binding back to a fixed index 0 fails
`sampled the active device, not fixed index 0` - run and observed, not asserted.
The real UUID/PCI mapping against a driver remains **NOT_RUN** here (no NVIDIA
device); it is the Windows/CUDA agent's receipt, not a PASS from this host.

Registration placement also changed: `cmake/TelemetryTests.cmake` is included
inside the standalone test branch, and the duplicate `HYPERCINE_BUILD_STANDALONE`
declaration is gone. That is what reconciles the previously confusing counts:
at `72a2a0c` the SDK/upstream lane registered **91** tests (two of them the
standalone-only telemetry tests, leaked by the early include) while the strict
standalone lane registered **171**. After this change the SDK lane registers
**89** with no telemetry test, the standalone lane **172** (the added optimized
Python mode), and quality-state-only **45** with no telemetry test.
