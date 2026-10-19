<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# GOP2 comparison verification: independent macOS follow-up

Parent: PR #190, source `6aa64bfbea7770ba93fb2ce2d47fd6b7e56bc938`.
Issue #189 remains open with the pre-inverse entropy and unclipped final signed
plane oracles missing. This change repairs a test-only integrity verifier; it
provides no new codec/reference qualification.

## Reproduced defect

The parent `verify-comparison` rehashed files and sidecars but returned the
index's `mismatched` value without recomputing equality and its aggregate. A
synthetic candidate can differ from its reference, with updated file/index/report
hashes, while `equal=true` and `mismatched=0` are accepted. An index is an integrity
record, so its internally contradictory claims must fail even if its own hash
matches. The parent also accepted a different candidate SHA, changed original
manifest, inconsistent geometry, report/index disagreement, misbound reference
IDs, stale per-artifact input SHA, and Python bool/float identity aliases.

The corrected verifier binds the available reference manifest, candidate revision,
geometry, canonical frame/stage reference mapping and original report. It derives
equality from authenticated file digests, checks each result and aggregate, and
preserves a truthful nonzero mismatch count. This is consistency verification;
hashes and a claimed producer status alone cannot authenticate an SDK producer.

The parent registered the Python integrity suite only in `tests/CMakeLists.txt`,
which the SDK-free standalone path bypasses. Register it in that path and require
its name in the normal CI inventory. Keep the existing SDK registration and
standalone tests-OFF behavior.

## Native verification

Native macOS 26.5.2 ARM64, AppleClang 21.0.0, CMake 3.31.5, Python 3.9.6,
FFmpeg n8.0. Optimized Python is enabled for the full standalone gate.

- Updated integrity suite: 20/20 methods PASS. Against the exact parent helper,
  12 new rejection cases fail and the remaining eight controls pass (no errors).
  Tiny synthetic files and locally resealed hashes exercise the real verifier;
  they never serve as codec goldens. Honest nonzero mismatch remains nonzero.
- Standalone inventory helpers: 7/7 methods PASS, including a missing-integrity
  registration rejection. Standalone tests OFF: zero registrations.
- Full SDK-free Release CTest: 33/33 PASS, zero skips, 73.61 seconds.
  The CTest integrity entry executes all 20 updated methods. Required external
  decode and all registered tests are run;
  no private corpus, original SDK, Metal or CUDA execution is claimed.

Portable reproduction (FFmpeg and ffprobe must be on PATH):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_UPSTREAM=OFF -DHYPERCINE_BUILD_STANDALONE=ON \
  -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --parallel 4
PYTHONOPTIMIZE=1 python3 ci/run_standalone.py --build-dir build \
  --system Darwin --machine arm64
python3 -B -m unittest discover -s ci -p test_standalone_gate.py
```

A separate public-manifest review made 1,278 assertions across the two pinned
parent manifests and comparison indices: 82 unique original artifacts and 66
resident stages per geometry, exporter source digest, compact dimensions/pitches,
full reference digests for 224 comparison entries, equal/packet identity and
remaining oracle gaps. These are checks of public metadata, not rehashing private
files or independently replaying the owner's Windows/CUDA comparisons. The
existing originals, candidate outputs and all committed manifests are unchanged.
