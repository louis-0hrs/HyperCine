# Issue 259: padded-input extent regressions

Refs #259. Regression-only follow-up to merged #348, based on main
`8bd1ecba39e3e028781eaba74136d003a28b8654`.

## Contract and scope

For BGRA8, let `row = width * 4`, `stride = row + 64`, and
`extent = stride * (height - 1) + row`. The physical byte extent is neither
packed `row * height` nor full `stride * height`: the former omits row
padding and the latter includes unused padding after the final row.

Only `tests/test_encoder_backend_selection.cpp` and this note change. No
production codec, input validator, capability policy, API, build registration,
CI workflow, backend/profile admission, `upstream/`, or corpus changes.
This closes a discriminating regression-coverage gap, not a demonstrated
production input-validation defect.

## Added coverage in existing required CTests

- `encoder_backend_selection_contract`: 240 table-driven cases across
  128x24, 256x256, 1280x720, 1920x1080, 2048x1080 and 3840x2160. CPU scalar,
  synthetic present/absent Metal and CUDA, explicit fallback on/off, and byte
  counts packed / extent-minus-one / exact extent / full padded storage.
  All geometry, qualification and budget axes remain valid; assert the exact
  format or absent-backend refusal, zero scalar/Metal/CUDA factory calls on
  refusal, typed constructor rejection, and matching selected factory/fallback
  for admitted requests. Exact extent is permitted without final-row padding.
- `encoder_backend_session_packets`: actual CPU scalar sessions at 128x24 and
  256x256, each binding exact extent or full padded storage, directly or via
  explicit absent-CUDA scalar fallback. Requests below the physical extent
  reject. With a full bound, physically valid exact-extent and full-minus-one
  calls also reject: this distinguishes the session bound from the frontend
  validator. Successful exact/larger calls compare complete CFHD packet,
  quant/scale tables, carried state and all coefficient hashes against an
  independent instance of the unchanged scalar writer, before/after Reset.
  Rejected calls must not advance that observed writer state.

The caller-owned test vector allocates the full padded storage even when a
smaller byte count is supplied. This tests declared-extent semantics, **not**
sanitizer proof that a smaller allocation cannot be overread. The comparison
writer is the same implementation, not an independent codec oracle.

## Verification

Local Linux x86_64 CPU-only Release, GCC 14.2, CMake 3.31.6, Python 3.13.5,
FFmpeg 7.1.5. Upstream, CUDA, Metal and Vulkan OFF; external decode REQUIRED;
bounded build parallelism 2.

- Focused required CTests: **2/2 PASS**, zero failure/skip, 1.73 s.
- Full official strict standalone gate: **127 registered / 127 completed /
  127 PASS / 0 FAIL / 0 SKIP**, configure/build/gate exits 0; 404.76 s wall
  time (JUnit rounded 404 s). Registered/completed unique names, gate JSON
  and source/compile-database bindings independently verified. JUnit SHA-256:
  `4e7094ea6287d14e7e241db776fe75004aa5e98923f0377b5c983347f9dab5fc`.
- Independent read-only review: **ACCEPT**, no blocking defects. Reviewed test
  blob `2a663dcb0d514e750fbc5afca4d0f113c9050f81`; independent recompilation
  matched the parent test object byte-for-byte. Separate baseline contract and
  packet modes passed. Three scratch-only controls compiled/linked successfully
  and failed at the intended new assertions: capability floor weakened to packed
  bytes, capability floor tightened to full padding, and runtime declared-byte
  floor weakened to physical extent. No production source mutation was retained.

Portable reproduction (keep build and raw logs outside the source tree):

```sh
cmake -S . -B "$HYPERCINE_BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_ENABLE_METAL=OFF \
  -DHYPERCINE_ENABLE_VULKAN=OFF -DHYPERCINE_BUILD_TESTS=ON \
  -DHYPERCINE_BUILD_BENCHMARKS=OFF -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON
cmake --build "$HYPERCINE_BUILD_DIR" --parallel 2
ctest --test-dir "$HYPERCINE_BUILD_DIR" -C Release --output-on-failure \
  -R '^encoder_backend_(selection_contract|session_packets)$'
python3 ci/run_standalone.py --build-dir "$HYPERCINE_BUILD_DIR" \
  --system Linux --machine x86_64
```

## Limits and next owner

No native GPU, macOS/Windows, sanitizer, original-SDK/private replay,
historical binary provenance, performance, independent pixel oracle, product
or SMPTE conformance claim. Synthetic factories never execute a GPU transform.
The full standalone FFmpeg gates are separate from same-writer comparisons.
GPU admission remains FilmScan3-only, #187 HOLD; #259/#46/#321 stay open.
Mac CI follows current owner routing: no automatic hosted Mac execution, and
absent Mac rows are NOT RUN, never a pass. No hosted Mac fallback requested.

Next: independent exact-diff/receipt review after local verification and genuine
required publication privacy; maintainer owns acceptance and merge. No automatic
merge, release, branch-protection or service/scheduler change.
