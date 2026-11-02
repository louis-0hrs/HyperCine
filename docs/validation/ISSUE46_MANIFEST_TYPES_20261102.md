# Issue #46: strict original-SDK sequence manifest types

## Bounded defect and correction

Base: `a2656342ef82501030f621d3b65d38d809e398c9`.

`tools/p6_sdk_gap_gate.py::validate_manifest` previously accepted JSON
`true` or `1.0` as schema version `1`, and floating-point dimensions such as
`[1920.0, 1080]` as `[1920, 1080]`, because Python numeric equality does not
distinguish those types. Non-object roots and missing `frames` could instead
raise incidental `AttributeError` or `KeyError`.

The sequence-manifest boundary now requires an object, an exact integer
schema version, and a two-element JSON array of exact integer dimensions.
Missing frames are rejected through the existing strict trace contract.
No coercion or optimized-away assertions are used. Existing SDK identity,
timebase, profile, kind and ordered frame checks remain in force.

This is a malformed-metadata acceptance defect, **not a demonstrated original
corpus discrepancy or encoder packet mismatch**. The independent transfer-index
pin and artifact hashes are still required. Root inventory and unrelated
manifest formats are outside this focused change.

## Focused evidence

Linux x86_64, Python 3.13.5:

- Existing plus seven new test methods: **20/20 PASS**, both normal and `-O`.
- Same regression tests against the unmodified base validator: **8 failing
  subtests and 8 erroring subtests** (20 methods run; intentionally RED).
- Three hash-pinned synthetic malformed manifests reach the real `measure`
  entry point and are rejected with the expected manifest error before
  encoder invocation or work-directory creation. These fixtures do not
  constitute successful packet replay.
- Independent read-only review: ACCEPT, no blocking findings; independently
  repeated normal/optimized tests and baseline sensitivity.
- Python 3.9 grammar check passes; no actual Python 3.9 runtime was available.

Read-only compatibility: **7/7** original sequence/negative-ramp manifests
containing **600 frame records** remain accepted. Their sizes and hashes were
verified against independently pinned transfer index
`dfb7cace17a73b7bfd1871be545d07acf14692adaafda25907370edec5b6dffb`.
This checked manifest compatibility only, **not fresh encoding, packet parity,
historical SDK provenance or native Windows execution**. No corpus files changed.

## Full standalone validation

Fresh Linux x86_64 CPU Release, GCC 14.2.0, CMake 3.31.6, Ninja,
Python 3.13.5, FFmpeg 7.1.5: configure/build/official strict standalone gate
all exited **0**. Complete CTest **124/124 PASS**, zero failures/skips,
**213.06 s**, external decoding REQUIRED, upstream and all GPUs OFF,
build/test parallelism 2. Completed JUnit, unique test names and gate JSON agree.

JUnit SHA-256:
`55d8d376c0d4c7bc3271ccf7cad5f944ca22f490762cb2a30d89d995b5362fb8`.

Tested implementation blob:
`b7da1b4c055456e8bb1d998e7413aa53d097db85`.
Tested regression blob:
`3d6e87b6e94f61ba67de5e58bd2204636e72ea87`.
Both files were hash-checked unchanged after the full run. This is execution
on the candidate source, not post-merge-main or hosted/native-other-platform
evidence. Optional private packet and original scalar-stage replay were not
configured; their absence is not counted as a qualification pass.

## Portable reproduction

```sh
python3 tests/test_p6_sdk_gap_gate.py
python3 -O tests/test_p6_sdk_gap_gate.py
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_ENABLE_METAL=OFF \
  -DHYPERCINE_ENABLE_VULKAN=OFF -DHYPERCINE_BUILD_TESTS=ON \
  -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build build --parallel 2
CTEST_PARALLEL_LEVEL=2 python3 ci/run_standalone.py \
  --build-dir build --system Linux --machine x86_64
```

Choose sufficient disk-backed temporary storage for FFmpeg geometry tests;
raw logs, JUnit and machine-local paths stay outside Git.

## Boundaries

No codec, profile admission, backend, CMake/CI inventory, frozen `upstream/`,
original exports or corpus changes. No new GPU, private packet replay,
Windows-native, sanitizer, performance, historical source-to-binary, pixel
oracle, product or SMPTE conformance qualification. Keep #46/#321 open.
Draft review only; no protected-main merge or release.
