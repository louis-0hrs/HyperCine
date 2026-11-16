<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Hosted CI performance pass 2

Owner-requested continuation of #391, tracked with infrastructure issue #349.
Base: `62ecb8af61b5edee475154ca01774a2bb27817f1`.
This changes execution and diagnostics, not codec correctness requirements.

## Changes

- Non-Mac standalone lanes use exactly **two** concurrent CTests. Linux
  ASan/UBSan stays at its already qualified two-slot bound; Linux and Windows
  Release change from one to two. The parser still rejects zero, unbounded,
  automatic or more-than-two settings. Mac fallback and local default are
  unchanged. No test, dependency, negative control or sanitizer flag is removed.
- Windows SDK-reference **builds** use `cmake --build --parallel 3`.
  Reference CTests deliberately remain serial: upstream SDK state/outputs need
  separate native concurrency qualification. An explicit 30-minute job timeout
  prevents unbounded reference jobs; the standalone 45-minute cap is unchanged.
- Hosted Linux setup probes `ffmpeg`, `ffprobe` and `ninja`. When all work, no
  apt refresh/install is needed. Missing executables map to only their needed
  apt packages; an authorized hosted Linux install must re-probe successfully.
  A present-but-failing/timed-out tool is a hard failure, not a cache hit.
  Self-hosted runners and local invocations cannot implicitly install packages.
- Build and test wall time, source commit, logical CPU count, configured CTest
  slots, JUnit hash, individual test durations and slowest tests are recorded
  in short JSON artifacts. CPU-reference output gains a JUnit file and
  `--no-tests=error`. Source tests and upstream invariance checks still execute.

The timing wrapper propagates child failures without retry, clears stale
reports before execution, and does not produce a qualification `PASS`.
`MEASURED` means a command completed and a timing receipt was written; it
is not SDK, hardware or VC-5 qualification. Skips/errors are counted explicitly.
Summed test elapsed seconds are not CPU utilization or a speedup estimate.

## Baseline and qualification

Observed main run [43778077269](https://github.com/louis-0hrs/HyperCine/actions/runs/43778077269),
2026-11-16, source above. GitHub job/step timestamps (whole-second resolution):

| Lane | Whole job | Full gate step | Linux apt step |
| --- | ---: | ---: | ---: |
| Windows Release | 356 s | 288 s | not applicable |
| Linux Release | 415 s | 346 s | 26 s |
| Linux ASan/UBSan | 550 s | 450 s | 19 s |

The full-gate step includes guard self-tests and preflight, not just CTest.
Compare the same step boundary; do not present the new wrapper's finer timing
as an identical baseline metric. Host variability means one run is preliminary,
not a guaranteed percentage improvement. Confirm at least all 158 inherited
standalone tests, zero skips in those lanes, explicit SDK reference skips,
actual native Windows behavior, all current privacy and compile gates, and
absence of resource contention or shared-output flakes on the final head.

`ci/test_ci_performance.py` adds executable missing/broken-tool, hosted-install,
re-probe, timing/XML, stale-result, true child-exit and no-synthetic-PASS controls.
The workflow invokes them before build on Windows/Linux. The existing
`ci/test_standalone_gate.py` scheduling expectation is updated, not deleted.

## Scope and next decisions

No compiler cache, build-directory restore, old JUnit restore, package archive
cache, workflow cancellation change, filter reduction or nightly sweep is
introduced. The observed Release build took 25 s (Windows) / 31 s (Linux),
versus several minutes of testing; adding a large cache before measuring
transfer overhead would not be justified. A future compiler cache requires
mode/toolchain isolation and trusted default-branch writes; never cache a PASS.

#259, #46, native Mac/GPU evidence, full VC-5 and GoPro qualification remain
separate. Frozen `upstream/` and private goldens are untouched. Roll back only
this performance change if concurrency causes flakes or memory pressure;
do not turn failed tests into skips or reduce the test domain.

References: [CMake parallel builds](https://cmake.org/cmake/help/latest/manual/cmake.1.html),
[CTest parallel execution](https://cmake.org/cmake/help/latest/manual/ctest.1.html),
[GitHub cache trust boundaries](https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching).
