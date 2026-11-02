# Issue #46: strict top-level SDK corpus manifest contract

## Defect and scope

Base: `a9893c4` (includes merged #343). The earlier sequence-level fix
intentionally excluded the top-level inventory. Its CLI boundary still accepted
JSON `true` and `1.0` as schema version `1`, because Python numeric equality
aliases those values. Non-object roots and non-object frozen-source maps could
raise incidental `AttributeError`/`TypeError` rather than an intentional
validation error. A list containing the expected source names reached `.items()`.

The CLI now requires an object with an exact integer version and an object
for the frozen-source mapping. Existing ordered profile checks, capture-source
hash, exact frozen-source key set and every source digest remain mandatory.
Rejection precedes output-directory creation, packet measurement and encoding.
No coercion or optimized-away assertions are used.

This is metadata contract hardening, **not a demonstrated original corpus
discrepancy, encoder mismatch or provenance qualification**. A caller must still
supply the independent transfer-index hash; the change does not bypass it.

## Focused controls

Linux x86_64 / Python 3.13.5:

- Six additional regression methods; **26/26 PASS** normally and under `-O`.
- Exact final tests against the unchanged base implementation: **2 failing
  subtests and 11 erroring subtests**; 26 methods run, intentionally RED.
  Both numeric aliases reach the mocked measurement/output stage on the base.
- Controls execute real `main()` argument parsing, JSON loading, transfer-index
  pinning, per-artifact size/hash checks, and capture/frozen-source hashing.
  Only packet measurement is mocked. Valid integer/object input reaches all
  four selected profiles; malformed input raises a deliberate validation error
  before measurement, encoder invocation, work directory or report creation.
- Missing contract fields, wrong profile order and wrong capture digest remain
  rejected. These synthetic controls are **not successful packet replays**.
- Independent read-only review: **ACCEPT**, no blockers; separately repeated
  normal/optimized tests and baseline sensitivity. Reviewed implementation
  blob `0fcd3e35bb85d4f9504777963ab6471e50b780e3`, test blob
  `9e5f24875e68d2fcd9afa8aba78453bff69c5c64`.
- Python 3.9 grammar passes; an actual Python 3.9 runtime was not tested.

## Full validation

Fresh Linux x86_64 CPU Release, GCC14.2.0, CMake3.31.6, Ninja,
Python3.13.5, FFmpeg7.1.5: configure/build/official strict standalone
gate all exited **0**. CTest **124/124 PASS**, zero failures/skips,
**207.15 s**, external decoding REQUIRED, upstream/all GPUs OFF,
build/test parallelism2. Completed JUnit, unique registered/completed
inventory and gate JSON agree. Both tested blobs above were unchanged
after the run.

JUnit SHA-256:
`fd9d81ce006fd007fd24cdb9317f62193894c49300c64023514be3c0a8d97302`.

This is execution on the candidate source, not post-merge-main evidence.
Optional private packet/native-original scalar-stage replay was not configured
and is not counted as a qualification pass.

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

Use adequate disk-backed temporary storage for FFmpeg geometry tests. Raw logs
and generated build reports stay outside the Git tree.

## Limits

No codec, profile/backend admission, CMake/CI inventory, frozen `upstream/`,
original export or private corpus changes. No native Windows/GPU, private packet
replay, sanitizer, performance, historical binary provenance, pixel-oracle,
product or SMPTE conformance claim. Keep #46/#321 open. Draft review only;
no protected-main merge, release, force-push or history rewrite.
