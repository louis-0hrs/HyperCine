<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# CUDA session capability: preserve the caller's host-memory ceiling

Base: `815be4718220644cf7f158bfd2418c00c0955200`. Issue #259.

## Defect and bounded correction

The internal CUDA capability query selected the caller's GPU budget and probed
device memory, but its additional CPU-budget check applied only to Metal.
The CUDA frontend still performs CPU color conversion, the prescale-2 spatial
stage, host output-band storage and shared host packet assembly. A sufficient
GPU budget therefore cannot waive the separately declared CPU ceiling.

A compiled CPU-only query against unchanged base, with 128x24 geometry, padded
576-byte stride, bound storage, synthetic available-CUDA/device-memory facts,
and CPU budget zero returned `ExperimentallySupported allocation=1 memory=0`.
Zero is a real ceiling; the config uses UINT64_MAX for an unrestricted ceiling.

The correction applies the existing static planning ceiling to CPU memory for
both present GPU frontends. The ceiling remains `36*pixels + 1MiB + stride*height`;
it is conservative admission policy, NOT a measured RSS or exact allocator cap.
Metal working-set, GPU budget, probed CUDA memory, overflow, geometry, profile,
reference, backend identity and fallback rules remain independent and unchanged.
No device/kernel/writer implementation, public API or profile admission changes.

## Regression coverage

- Existing required capability test: six admitted geometries, padded strides,
  CPU ceilings zero and ceiling-minus-one rejected, exact CPU/GPU/device ceilings
  accepted, independent GPU/device-minus-one rejection.
- Existing required fake-factory test: insufficient CUDA CPU budget refuses
  before every factory even when fallback is explicitly enabled; exact host
  ceiling selects only CUDA. Absent CUDA still uses the scalar host boundary,
  and does not inherit the unused GPU budget.
- Synthetic CUDA facts/factories are contract tests only: no CUDA runtime probe,
  GPU allocation, kernel dispatch or native GPU qualification occurs.

Portable reproduction (CPU-only Release, all GPU backends and upstream OFF,
standalone/tests ON, required external decode ON):

```sh
cmake --build build -j2
python3 ci/run_standalone.py --build-dir build --system Linux --machine x86_64
ctest --test-dir build -R 'encoder_capability_contract|encoder_backend_' --output-on-failure
```

## Execution receipt

Linux x86_64 CPU Release, GCC 14.2.0, Python 3.13.5, CMake 3.31.6,
FFmpeg/ffprobe 7.1.5: fresh configure/build and official strict standalone
runner PASS, **126/126 tests**, zero failures/skips, JUnit time 202 seconds.
External decode required; upstream and all GPU backends OFF, parallelism 2.
Unique registered/completed inventories and all three tested source digests
were checked against final reviewed files. JUnit SHA-256:
`da28800e137c3c9598d95508f83998c9820a0dcb67065bd66c02fb53319fdca3`.
Optional original scalar replay NOT_RUN.
Focused fixed tests: 3/3 PASS (capability, backend selection, session packets).
Independent review: ACCEPT for the implementation and two changed tests.
Independent optimized (-O3 -DNDEBUG) compiled controls used the same new test
objects and SDK-free libraries with separately compiled base/fixed capability
objects: both suites fail at intended host-budget assertions with base, and
both pass with the correction. No source mutation or GPU execution was needed.

## Boundaries

No native CUDA/Metal, original SDK/corpus replay, sanitizer, performance,
independent decoded-pixel, historical compiler/ISA, product or SMPTE claim.
FilmScan3-only GPU profile admission and #187 HOLD remain unchanged.
Keep #259/#46/#321 open. No frozen upstream or corpus edits.
