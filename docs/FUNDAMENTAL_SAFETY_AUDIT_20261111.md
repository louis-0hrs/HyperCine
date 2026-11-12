# Fundamental safety audit — 11 November 2026

Copyright (c) 2026 HyperCine Contributors. Dual-licensed under MIT OR Apache-2.0.

## Scope and evidence boundary

Baseline: `main` commit `dca0c0707a0d460fb8cb7ca7d198c0a7aa4be8e7`.
Tracking: [#364](https://github.com/louis-0hrs/HyperCine/issues/364).

This review covers public API bounds and failure handling, encoder session
ownership, reference/evidence handling, build and CI gates, and documented
product limits. It combines source inspection, bounded adversarial reproductions,
independent review, existing tests and new regression tests. It is not an
exhaustive security certification or a new codec/backend qualification.

Execution environment: Linux x86_64, GCC/G++ 14.2.0, Python 3.12.14, CMake 3.31.6,
GNU Make and FFmpeg/ffprobe 7.1.5-0+deb13u1. Builds, raw logs and synthetic
reproduction data are outside the Git tree. No private reference corpus was
accessed. No native Windows, ARM64/NEON, Metal or CUDA runtime qualification was
performed. Existing SDK, product and SMPTE VC-5 gates stay separate.

Priorities below are engineering priorities, not CVSS scores. In particular,
an invalid direct library call is not evidence of a remotely exploitable media
parser. **Reproduced** means an actual local execution exhibited the failure;
**source-confirmed gap** means the code establishes a missing contract but the
affected native/hardware scenario was not executed.

## Findings and disposition

| ID | Priority | Finding | Evidence | Disposition |
| --- | --- | --- | --- | --- |
| A01 | P1 | CPU wavelet dimension arithmetic can overflow before rejecting input | Reproduced with UBSan | Implemented in [#366](https://github.com/louis-0hrs/HyperCine/pull/366), tracking #362 |
| A02 | P1 | 2D wavelet allocation errors escape a status-returning C API; allocation precedes full stride validation | Reproduced with deterministic allocation failure | Implemented in #366, tracking #362 |
| A03 | P2 | Public main header fails first-include C11 compile/link | Reproduced at `-O0` | Implemented in #366, tracking #362 |
| A04 | P1 | Frontend geometry can change after session admission; successful default encode does not retain a binding | Reproduced by an adversarial frontend and old-library regression | Implemented in [#365](https://github.com/louis-0hrs/HyperCine/pull/365), tracking #259 |
| A05 | P1 | General CI accepts a committed change to frozen upstream | Verified gate logic and temporary-Git controls | Implemented in [#368](https://github.com/louis-0hrs/HyperCine/pull/368), tracking #363 |
| A06 | P1 | Changes confined to `cmake/**` skip CUDA compilation | Reproduced with repository routing model | Implemented in #368, tracking #363 |
| A07 | P1 | CLI/parity outputs can destroy existing evidence, including inside a selected corpus root | Reproduced using disposable synthetic files | Open; [#367](https://github.com/louis-0hrs/HyperCine/issues/367) has reproduction and acceptance contract |
| A08 | P1 | SDK external-band wrapper indexes caller-supplied channel without a bound | Source-confirmed gap | Open; SDK-wrapper contract/native negative tests required |
| A09 | P1 | Encoder copies share mutable GPU frontend resources | Source-confirmed ownership gap | Open; explicit copy/threading contract required |
| A10 | P1 | CUDA device-zero capability data can differ from the current device used for work | Source-confirmed gap | Open; bind device identity and test on native CUDA |
| A11 | P1 | Main branch rules require publication privacy but not successful builds/CTest | Live ruleset read | Open under #349; stable aggregate gate proposed |
| A12 | P1 | Unsafe oracle-log extraction and optimization-sensitive validation remain on main | Source confirmed; existing #361 | Existing fix independently exercised; remaining acceptance gates retained |
| A13 | P2 | Linux ASan workflow disables leak checking with an obsolete Mac rationale | Source confirmed | Qualify Linux leak checking separately |
| A14 | P2 | Action version tags, implicit CPU job permissions and missing job timeouts weaken reproducibility/containment | Source confirmed | Bounded infrastructure follow-ups |
| A15 | Product gate | Long-session, timebase, SDK provenance and backend limits remain | Existing explicit code/docs/contracts | Preserve admission limits; no unqualified expansion |

## Implemented correction contracts

### A01–A03: public CPU API safety

Affected code: `src/cpu/wavelet_cpu.cpp`, `include/hypercine/wavelet.h` and
`include/hypercine/hypercine.h`.

The baseline inverse-horizontal check evaluates `2 * width` using `int` before
deciding whether the destination stride is sufficient. `width = INT_MAX`
therefore produces signed overflow in the validation itself. The inverse-2D
function similarly doubles subband dimensions before checking representability.
Other loops use signed `row * stride`; the internal SIMD dispatcher already
guards this extent, but direct public CPU entrypoints did not.

The correction validates every declared source and destination extent before
buffer access or allocation and checks doubled dimensions before arithmetic.
It preserves filter mathematics for valid inputs. A pointer-only API still
cannot prove the actual allocation size behind a caller's pointer: callers
must supply real storage covering the accepted geometry and strides.

The baseline 2D routines also allocate scratch before all strides have been
validated. Injecting an allocation failure into a 6-by-6 forward call with
`src_stride = 0` demonstrated `std::bad_alloc` escaping instead of returning
`INVALID_ARGUMENT`. The correction validates first, translates scratch
allocation failure to `OUT_OF_MEMORY`, and preserves caller output on refusal.
Tests exercise failure of each scratch allocation, cleanup and a successful
retry, rather than relying on a machine actually exhausting its memory.

The public main header separately failed a minimal C11 translation unit because
its frozen SDK includes require `size_t`. Supplying `<stddef.h>` exposed a
second failure: the non-static inline version function had no external C
definition at `-O0`. Including `<stddef.h>` in the HyperCine wrapper and using
`static inline` repairs both, without editing frozen SDK headers. C11 and C++17
unoptimized compile/link probes cover this boundary. SDK-dependent header tests
must not be disguised as SDK-free standalone compilation.

Direct ARM64 NEON entrypoints require a separate matching audit and native
acceptance; this CPU correction does not claim to have qualified them.

### A04: immutable session geometry without a public layout change

Affected code: `src/core/filmscan3_backend.{h,cpp}` and
`src/core/filmscan3_encoder.cpp`; regression:
`tests/test_filmscan3_geometry.cpp`.

The previous `FrontendAccess::Use` validates a virtual geometry declaration
once, but `FilmScan3Encoder::geometry()` asks the same mutable object again.
An implementation can consequently replace a previously accepted declaration.
The shipped CPU/Metal/CUDA implementations are not shown to mutate their
geometry; the reproduced defect is at the internal extension boundary.

There is also a concrete default-construction gap: a successful default encode
uses the shared CPU frontend while leaving `frontend_` null. A later injection
can then change dimensions while carrying the previous frame's rate state.

An internal adapter retains a geometry snapshot in the encoder's existing
private shared pointer and rejects a changed live declaration before dispatch.
The default binding becomes persistent only after successful encoding, so a
failed first call does not partially initialize the session. Refused injection,
Reset, recovery and same-size replacement are covered. The tests also use
valid changed geometry; rejection of an intrinsically invalid shape alone
would not prove an immutable binding.

Factories and `BackendSelection` retain their original frontend pointers.
Native timing/failure-injection helpers continue to receive those original
objects; the adapter delegates backend identity, ISA, timing and transformation.
No public class member or object layout changes. Native GPU acceptance remains
necessary before treating this as a qualified GPU release change.

### A05–A06: frozen reference and build coverage

The baseline frozen reference is intact:

```text
original SDK commit: 11574d0295771edccadd17af14af74a539f924c7
frozen tree:         837528ef9e9a55c416e9592ecb54e1c8f1ff9cf8
main:upstream:      837528ef9e9a55c416e9592ecb54e1c8f1ff9cf8
```

The old general checks compare only the worktree/index with HEAD. Once an
upstream modification has been committed, those checks no longer distinguish
it from the reference. The stronger verifier compares the committed subtree
with the pinned original tree, verifies index and materialized files, rejects
extra files and disables Git replacement objects. Materialized bytes are
checked without executing Git clean filters; only explicit text LF/CRLF
equivalence is accepted. Independent negative controls also cover hidden
executable-mode changes and case-colliding extra files. It works in shallow
checkouts without requiring the original SDK commit object. Real disposable
Git repositories test committed, staged, unstaged, ignored, replacement and
checkout-normalization cases.

The existing optional `tools/verify_windows_sdk_corpus.py` already contains a
reference tree comparison; this change supplies the missing general gate.
It does not modify the reference or create a new SDK baseline.

Both CUDA workflow event filters omitted `cmake/**`. Single-file changes to
`cuda_host_compiler.cmake`, `wavelet_simd.cmake` and
`threaded_encoder_prefix.h` therefore skipped the CUDA compiler lane while
the prior routing contract still passed. The correction adds that scope and
requires the routing model to reject its removal. Changes to frozen workflow
step receipts are explicitly limited to replacing the three weak upstream
checks; unrelated runner routing and approval policy stay bound to the
previous baseline.

## Open findings with concrete repair and acceptance plans

### A07: preserve output and reference evidence

Implementation task: [#367](https://github.com/louis-0hrs/HyperCine/issues/367), READY / unclaimed at this audit handoff.

Two failures were executed using disposable synthetic directories only:

1. `tools/hypercine_encode_filmscan3.cpp` opens `OUTPUT_DIR/trace.json` before
   validating the first input. A nonexistent input returns exit 1, but an
   existing trace has already been truncated/replaced. The reproduced trace
   changed from its original sentinel to 33 bytes of partial JSON.
2. `tools/p6_writer_parity.py` permits `--work` to resolve to `--corpus` and
   recursively removes an existing profile `samples` directory before invoking
   the encoder. With a deliberately failing encoder, the command returns exit
   1 after deleting the retained sentinel. No actual private corpus was used.

Repair the path/ownership contract before replacing individual file-open calls:
resolve and reject output/work/report locations within retained input roots;
validate profile/geometry selectors; create a unique, exclusively owned run
directory; reject existing destinations and symlink escapes; never recursively
delete a caller-owned directory to obtain freshness. Commit reports only after
successful completion and preserve prior evidence on input, encoder and write
failure. If rerun support is needed, make the new run destination explicit.
The path/ownership checks must still hold when files are created; a prior
`resolve()` alone does not prevent a concurrent symlink replacement.

Acceptance: existing-file and existing-directory sentinels, equal/nested
corpus/work roots, linked destinations, invalid first input, mid-sequence
failure and parallel runs must leave all prior input/evidence bytes unchanged.
Reconcile current tests that deliberately reuse output directories instead of
adding a destructive overwrite default to keep them passing.

### A08: bind the SDK entropy wrapper's array extent

`src/cpu/entropy_parallel.cpp::EncodeOneExternalBand` reaches
`transforms[coeff.channel]` after pointer/element/shape checks but without a
channel bound. The function's signature does not receive the array extent.
Caller-controlled subband values also reach coding-flag selection.

Add an explicit trusted channel extent or a validated view, then validate
channel, level, band, subband and lowpass/highpass relationships before any
index or SDK call. Do not assume all families have exactly three channels.
Test negative and oversized indices with the real SDK-wrapper target under a
supported toolchain/sanitizer. No remote exploitability or native crash was
established in this audit.

### A09: define copy and concurrency ownership

`FilmScan3Encoder` and internal `EncoderSession` are implicitly copy-constructible.
The copied shared pointer retains the same frontend while rate state is copied
separately. The Metal frontend holds mutable workspace, staging buffers, timing
and failure-injection state. Concurrent calls through copied encoders can
therefore share resources that separate freshly constructed sessions do not.

Choose an explicit move-only encoder or a real `Clone` operation that creates
new backend resources. A mutex around one method is not by itself a decision
about copied rate state and diagnostics. Document whether the same object may
be called concurrently. Define whether a clone starts new `hash_session_` and
`diagnostic_serial_` identities; implicit copies currently retain them. Add
compile-time copy/move contract checks and native
Metal/CUDA cases for permitted concurrent use, reset and failure isolation.
The geometry adapter in A04 deliberately does not decide this public API policy.

### A10: bind the actual CUDA device

`src/cuda/filmscan3_cuda_frontend.cpp` queries device-zero properties in both
the frontend constructor and `ProbeCudaEncoderDevice`. Stream creation,
allocation and launches use the caller thread's current CUDA device instead.
With another device current, admission limits and reported identity can refer
to a different GPU from the one doing the work.

Retain an explicit selected device identity with the session, query that
device's limits, and establish/restore its current-device context around
allocation, execution and destruction as required by CUDA. Define cross-thread
use. Acceptance requires native device switching and, where available, two
different GPUs; verify device identity, memory limits, failure cleanup and the
caller's restored device. No multi-GPU execution was available here.

### A11: make successful build/test acceptance enforceable

The live `Protect main` ruleset (ID `25220927`) requires a PR and the
GitHub-Actions-owned `Publication privacy (all new commits)` status. It does
not require CPU/Standalone/CUDA results or an approving review; strict status
checks are disabled. This permits a technical merge despite failing or absent
build evidence. It is not evidence that a defective merge has occurred.

Prepare a stable, always-present aggregate acceptance check that distinguishes
required tests from explicitly unaffected lanes and binds every result to the
candidate SHA and trusted reporter. Then make that check required through a
reviewed ruleset change. Simply marking every path-filtered job name required
can leave unrelated PRs permanently pending. Coordinate with #349 and the
existing Mac relay; no ruleset, service or runner setting was changed here.

### A12: finish the existing oracle-log hardening

Main's `tests/compare_sdk_oracle.py` writes log-provided filenames before
validating the full allowlisted inventory and uses `assert` for substantive
validation, which disappears under Python `-O`. This is already addressed by
Draft [#361](https://github.com/louis-0hrs/HyperCine/pull/361); do not create
another competing implementation.

This audit independently executed exact head
`b7933c842c3459ee0553a6b4f22f0dde29c8b02c`:

- 23 unittest methods passed normally and 23 passed under `-O`.
- An actual FFmpeg/ffprobe replay of all eight retained public records passed.
- A generated eight-record log from the same public fixture bytes passed real
  FFmpeg/ffprobe replay; a synthetic raw diagnostic canary did not reach output.
- Both replays produced the three expected sample comparisons and control
  comparison with FFmpeg `7.1.5-0+deb13u1`.

These are useful local checks at the recorded SHA, not completion of that PR's full
mutation, standalone-inventory, native-platform and Actions privacy gates.
Keep it Draft until its own acceptance contract is met. No original SDK
decoder qualification is inferred from the public-fixture replay.

### A13–A14: remaining CI containment and reproducibility

The non-Mac test step in `.github/workflows/ci-standalone.yml` sets
`ASAN_OPTIONS=detect_leaks=0` with an AppleClang rationale. Linux leak checking
should be qualified and enabled independently; retain the Mac-specific limit
in the actual Mac lane. The current execution environment also prevents
LeakSanitizer under tracing, so local ASan/UBSan results with leak checking
disabled must not be described as a leak-clean run.

Action dependencies currently use major-version tags. Pin audited full commit
SHAs with an explicit update process. Add explicit read-only token permissions
to the CPU workflow and bounded job timeouts where absent. These are
reproducibility/containment improvements, not evidence of compromised actions
or excess effective privileges today.

### A15: existing product limits remain real

The FilmScan3 session stops after 65,535 frames: 36 minutes 24.5 seconds at
30 fps. Automatic TIMC currently admits only 30/1. These are intentional,
documented boundaries in `docs/FILMSCAN3_RATE_STATE_TIMEBASE.md` and
`src/core/encoder_capability.cpp`, not newly discovered silent overflows.
Do not wrap the 16-bit header field or accept another requested timebase without
deciding and testing the corresponding wire/timecode semantics.

GPU profile admission remains narrower than the six-quality CPU writer.
`reference_qualified` remains false at the capability boundary. Original SDK
provenance/pixel-oracle work (#321/#120/#46), decoder qualification (#16/#144/
#195), other geometry families and SMPTE VC-5 conformance remain explicit
separate work. Legacy Phase 3A CUDA benchmark figures must not be represented
as throughput or qualification of the SDK-free session implementation.

## Validation receipts and review order

### Published source candidates

| Candidate | Exact source commit | Evidence |
| --- | --- | --- |
| [#365 — session geometry](https://github.com/louis-0hrs/HyperCine/pull/365) | `8dcb15c2e82e1d1dd7757ad68d4a9655ac54250e` | [Geometry contract and regressions](ENCODER_GEOMETRY_BINDING.md) |
| [#366 — CPU API boundaries](https://github.com/louis-0hrs/HyperCine/pull/366) | `cc8e11159fe37d01e7950feb5160bf4735bf4225` | [Validation note](validation/CPU_API_BOUNDARIES_20261111.md), [source/test receipt](validation/cpu-api-boundaries-20261111.json) |
| [#368 — frozen reference and CUDA triggers](https://github.com/louis-0hrs/HyperCine/pull/368) | `a0afd29703f006b84f5d662de7c701c9bd7638c6` | [Validation note](validation/frozen_reference_gates_363.md), [source/test receipt](validation/frozen_reference_gates_363.json) |

The coordinator branch `audit/fundamental-hardening-20261111` combines these
three source candidates with this report and a
[combined execution receipt](validation/fundamental-integration-20261111.json).
It is an integration review candidate, not a merge to protected main. The
independent source-union review verified all 26 contributed paths: 23 retain
byte-identical source content; the three shared CMake/standalone inventory files
contain the additive union of the accepted changes. All 88 required test names
and all 33 standalone-guard regression tests remain. The only textual conflict
was adjacent test-method additions; both methods were retained.

### Actual local executions

| Source scope | Completed strict Release CTests | Wall time |
| --- | --- | --- |
| Unmodified baseline `dca0c07` | 127/127 PASS, zero fail/skip | 393.90 s |
| Session candidate #365 | 127/127 PASS, zero fail/skip | 199.75 s |
| CPU API candidate #366 | 128/128 PASS, zero fail/skip | 222.67 s |
| Frozen-reference/trigger candidate #368 | 129/129 PASS, zero fail/skip | 343.55 s |
| Combined candidate | 130/130 PASS, zero fail/skip | 188.30 s |

These are separate runs over overlapping inventories; their counts must not be
added to claim that many unique tests. All full runs required real external
FFmpeg/ffprobe. Wall times describe test execution, not codec throughput.

Additional discriminating evidence includes the CPU bounds/OOM sanitizer run,
the C11/C++17 unoptimized header probes, geometry regressions failing against
the old library and a guard-removal mutant, real-Git and routing negative
controls, and the separate #361 executions described under A12. The focused
CPU AddressSanitizer/UndefinedBehaviorSanitizer run passed; LeakSanitizer was
NOT RUN because of the host tracing limitation. It is not a sanitized run of
every backend or the combined full suite.

The baseline sanitized full CTest log hashes to
`5cb19b6203a2a4ef8b9faed0a7b185d567b0b4ec11e224ccc6ce43b22815f0f9`;
the session candidate's sanitized full log hashes to
`a498c795c82a57066bd0edc457957e0ff6dc24bca9a4e0a1d50d2e8039777e12`.
The CPU, CI and combined receipts contain their own source and artifact hashes.
Full raw logs and disposable media remain outside Git.

All published candidates stay Draft. The existing hosted-compute constraint is
preserved with `[skip ci]`; no paid workflow was dispatched or retried. These
local runs do not satisfy pending exact-head Actions privacy checks or native
Windows/ARM64/Metal/CUDA/original-SDK acceptance. `AGENTS.md` requires review and
applicable gates before protected-main integration. No release, main merge,
ruleset change or other agent's branch rewrite was performed.

Recommended review order:

1. CPU API boundary/exception/header correction (#362): smallest direct
   crash/undefined-behavior boundary and deterministic regressions.
2. Session binding correction (#259): review adapter delegation and negative
   rollback/Reset controls, then native backend acceptance.
3. Frozen-reference and CUDA trigger correction (#363): verify real-Git
   negatives and the deliberately limited workflow receipt changes.
4. Finish existing #361 acceptance and repair A07 output ownership before
   using those tools on retained reference data.
5. SDK wrapper bounds, copy/device ownership and stable merge-gate enforcement.

Preserve frozen upstream and existing independent agent branches throughout.
Local test success is not an automatic merge, release or performance claim.
