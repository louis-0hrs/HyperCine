# Internal backend selection (#259)

The internal session implementation connects capability permission to an actual
frontend without changing public headers, legacy constructors or the FilmScan3
writer. The original CPU-first step is now extended by the bounded internal Metal
contract in [ENCODER_METAL_SESSIONS.md](ENCODER_METAL_SESSIONS.md). This is not a
public product backend API or original-reference qualification.

## Entry points

Internal header: 'src/core/encoder_backend_selection.h'.

- 'detail::SelectBackend(config, device)' returns a 'BackendSelection' containing
  the requested/effective capability results, a typed selection status, explicit
  scalar fallback origin and either an existing frontend or no frontend.
- 'detail::EncoderSession(config, device)' snapshots configuration and consumes
  that result **before** frontend construction. A refusal throws
  'BackendSelectionError' with typed status and capability reason. The exception's
  capability reason describes preflight; initialization failure is distinguished
  by 'BackendSelectionStatus::InitializationFailed', not a fictitious capability
  failure. Allocation/logic exceptions are not converted to availability failures.
- The session delegates to the existing 'FilmScan3Encoder'. Geometry, sample
  capacity, scalar ISA and coefficient-hashing policy remain session-bound.
  Bound storage requires the same pitch and at least the declared storage bytes
  on each Encode. Actual pointer alignment/extent are still validated each time.
  Reset resets existing writer state, not session policy. No mid-frame fallback.

## Fallback rules

| Condition | Rule |
| --- | --- |
| Requested backend absent, fallback disabled | Typed refusal; no frontend factory call |
| Backend absent, scalar fallback explicitly allowed | Existing capability fallback; CPU budgets, shape, profile and reference requirement must all pass |
| Backend present but geometry unqualified | Refuse by default; explicitly allowed fallback reruns the **complete** scalar capability query |
| Format/profile/timebase/sample or resource budget failure | Never repair it by switching backend |
| Required original reference missing | Never waived by fallback; scalar query must also reject |
| Scalar backend absent | Refuse, including GPU-to-scalar fallback attempts |
| Invalid backend enum | Format refusal, even when fallback is enabled |
| Frontend initialization failure/null or mismatched geometry/ISA | Typed initialization failure; no retry to another backend |

Both original and effective capability diagnostics are retained when a second
scalar query is needed. A fallback origin can be recorded even if that attempt
is subsequently refused. Only status Selected plus a non-null frontend is success.

## Deliberate qualification boundary

The capability table permits experimental scalar CPU and explicitly listed Metal
sessions; Metal additionally requires bound storage and concrete device limits.
Presence alone does not grant permission. CUDA/Vulkan and CpuSimd remain outside
this revision. An absent SIMD backend can use explicit scalar fallback, but a
present unimplemented SIMD request is refused. Explicit scalar fallback is CPU
evidence only. Metal factory identity and actual native dispatch are separately
checked; #304/#309 native backend-required gates remain mandatory. Public
CPU/Metal constructors keep their historical behavior and do not acquire the
new conservative internal policy. See the Metal contract for the exact limits.

The public legacy fixed-Q2 cap remains 2,097,152 pixels. FilmScan3 sample/wire,
quality, timebase, reference and pixel limits are unchanged. No original GoPro,
VC-5 conformance, 8K, performance improvement or release claim follows.

## Reproduction

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHYPERCINE_BUILD_STANDALONE=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
  -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_REQUIRE_EXTERNAL_DECODE=ON \
  -DHYPERCINE_ENABLE_CUDA=OFF -DHYPERCINE_ENABLE_METAL=OFF
cmake --build build
ctest --test-dir build -C Release --output-on-failure
python ci/test_standalone_gate.py
python ci/run_standalone.py --build-dir build --system Linux --machine x86_64
```

The new mandatory CTests are 'encoder_backend_selection_contract' and
'encoder_backend_session_packets'. The latter compares complete samples, state,
36 quant/scale entries and 30 coefficient hashes against the existing scalar
encoder for 128x24, 256x256, 1280x720 and 2048x1080; the first two run concurrently
for 16 ordered changing frames each. It checks padded storage, failed-input
rollback, Reset, fallback geometry and concurrent hash-on/off sample equivalence. Existing
full-suite external decode gates remain separate from this in-memory equivalence.
Use a sufficiently large scratch directory via the platform temporary-directory
setting when running the full independent-decode suite.

## Historical CPU-first validation snapshot (#308)

Linux x86_64, GCC 14.2, CMake 3.31.6, FFmpeg 7.1.5; CPU-only SDK-free
configuration above, without optional original-SDK corpora:

- Final strict standalone gate: **108 registered / 108 passed / 0 failed /
  0 skipped**, 143.66 seconds with four CTest workers. The final expanded tests
  are included; the earlier serial run also passed all 108 tests.
- Focused Debug AddressSanitizer + UndefinedBehaviorSanitizer: **2/2**,
  0 failures/skips, 56.78 seconds; leak detection enabled. Flags:
  `-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all`.
- Focused ThreadSanitizer: **2/2**, 0 failures/skips, 21.70 seconds;
  `-fsanitize=thread -fno-omit-frame-pointer`, `TSAN_OPTIONS=halt_on_error=1`.
  Sanitizer claims cover these two new tests, not the whole suite.
- CI inventory Python guardrails: **25/25**, also with Python `-O`.
- Tests-OFF standalone library build: PASS.
- Seven separately compiled mutants rejected against a passing baseline:
  remove allocation permission gate; silently enable fallback; bypass scalar
  fallback budget; accept an invalid backend enum; ignore factory sample
  capacity; ignore diagnostic policy; drop the bound-storage check.
- Independent read-only source review: no implementation blockers. Its three
  suggested falsifying tests were added before final validation.

The complete existing external-decode suite ran, but this does not make the
original GoPro pixel oracle qualified. No native GPU run or performance claim.
