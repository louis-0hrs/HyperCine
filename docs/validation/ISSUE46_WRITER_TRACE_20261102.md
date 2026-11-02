# Issue #46: strict six-profile writer trace indices

## Defect and bounded fix

Base: `a9893c42f9a986210f2886ef0344f6c4942af2ba`.
The older `tools/p6_writer_parity.py` gate checked frame correspondence
using Python list equality. JSON `false`/`true` and `0.0`/`1.0` therefore
aliased valid integer indices 0/1 and could yield a PASS with a malformed
encoder trace. This is a trace-contract defect, not evidence of an encoder
packet mismatch or corrupted original corpus.

Require an object document, a frames list, object frame entries and exact
integer frame indices before packet parsing/comparison. Existing exact
count/order, transfer size/hash, cache repair and packet comparison checks
remain in force. No coercion or removable acceptance assertions.
The SDK gap gate and root-inventory PR #345 are separate and unchanged.

## Regression evidence

`tests/test_p6_writer_trace.py` executes real `main()` CLI parsing, synthetic
filesystem manifests/inputs/packets, transfer-index hashes and trace JSON.
Only the encoder and CFHD parsing/normalization are mocked. The copied
synthetic packets are **not** real CFHD or codec/original parity evidence.

- Nine methods: valid integer trace, divergent packet failure, boolean/float
  aliases, missing/null/string indices, malformed frame/list/document shapes,
  missing frames, truncated/extra/reordered/duplicate indices.
- Candidate: **9/9 PASS** normally and **9/9 PASS** under Python `-O`.
- Unchanged base with final regression tests: **4 failing subtests and 16
  erroring subtests**, nine methods run, intentionally RED. All four numeric
  aliases reached the baseline's synthetic PASS path instead of being rejected.
- Malformed candidate traces are rejected before the mocked packet parser
  is called or a new report is written. Correct traces still reach comparison;
  a same-length changed emitted packet still returns failure.
- Both normal and optimized CTests are registered and required by the strict
  standalone runner. Removing either from a synthetic inventory is rejected
  (two removal controls PASS).
- Python 3.9 grammar checked for the three changed/new Python files; no actual
  Python 3.9 runtime qualification.

## Full validation

Fresh Linux x86_64 CPU Release / GCC14.2.0 / CMake3.31.6 /
Python3.13.5 / FFmpeg7.1.5: configure, build and official strict standalone
gate exited **0**. CTest **126/126 PASS**, zero failures/skips, **245.09 s**;
required external decoding ON, upstream/all GPUs OFF, parallelism2.
Completed JUnit, unique registered/completed inventory, gate JSON and all
four tested implementation/test/build/runner Git blobs agree.

JUnit SHA-256:
`54b1ca502f603f1035fbfffd32013d7961e91abb4c36b75a7a11c84e7d3a7d79`.

Independent read-only review: **ACCEPT**, no blockers. Reviewer separately
repeated normal/optimized9/9 controls, base4failing+16erroring subtests,
confirmed all four base aliases actually returned0/reached packet parsing,
and checked both mandatory-registration removal controls.
This is candidate-source execution, not post-merge-main or private original
qualification. Optional original scalar-stage replay was NOT_RUN.

## Reproduction

```sh
python3 tests/test_p6_writer_trace.py
python3 -O tests/test_p6_writer_trace.py
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

Use adequate disk-backed temporary storage for FFmpeg geometry tests.
Raw logs and generated build reports stay outside Git.

## Limits

No codec/backend/profile-admission, frozen `upstream/` or corpus changes.
No fresh private packet, native Windows/GPU, original binary provenance,
independent pixel-oracle, sanitizer, performance, product or SMPTE conformance
qualification. Keep #46/#321 open. Draft review only; no protected-main merge,
release, force-push or history rewrite.
