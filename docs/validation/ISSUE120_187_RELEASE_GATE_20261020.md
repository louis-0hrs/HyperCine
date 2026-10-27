# Issue #120 / #187: native guards and explicit release inventory

Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0; see LICENSE-MIT and LICENSE-APACHE.

This child starts at #210 `1563608c7fa90db7f0172266f7d511edc9ce979f`,
retaining the ordered #206 → #207 → #208 → #209 → #210 test stack. It
adds validation tooling, a CTest regression test and a CMake qualification
target. It does not qualify an independent pixel oracle. No production codec,
CUDA kernel, threshold, original SDK file or packet format changes.

## Qualification entrypoint

With Python 3 available at configure time:

```powershell
cmake --build build --config Release --target reference_release_gate
```

The target builds the reference contract and (when enabled) Phase2a/b/c/d
executables, then runs only the `reference-oracle` CTest label. It writes
`build/reference-release.json`. The Python runner maps executed PASS to 0,
FAIL to 1, and skipped/not-run/missing required tests to BLOCKED=77. Failure
has precedence over BLOCKED, so an 8K capacity failure cannot be quarantined
as an original-SDK pixel signal. Missing/invalid result evidence and nonzero
CTest execution status fail closed. A fresh temporary JUnit report prevents
reuse of an old successful run. Duplicate results are rejected.

The CMake target fails on either 1 or 77; build tools may surface either as
their own exit 1. The JSON preserves the distinction. Ordinary CTest still
uses its normal semantics, including overall success with skips. Release
qualification must require this explicit target as well as the full native
suite. Schema 2 separates `reference_lane` execution from `release_gate`.
Even an all-PASS lane remains release BLOCKED=77: no reviewed independent
pixel-oracle adapter exists. `independent_pixel_oracle.qualified` is false.
No CLI switch, test name or self-asserted manifest grants qualification. A
future adapter requires reviewed same-packet evidence bound to implementation,
version, normalization, scope and thresholds before release can become PASS.
CPU-only hosted jobs
cannot qualify the four missing CUDA tests. The target is available only
when Python is found; absence of the target is not qualification.

Direct invocation, preserving runner exit 77:

```powershell
python tools/check_reference_release.py --build build --output build/reference-release.json
```

Eight deterministic regression cases exercise all-pass evidence without an oracle,
spoofed qualification test names, green CTest
with skips, missing CUDA tests, strict failure precedence, runner error,
not-run results and duplicate evidence. Python is validation tooling only.

## Native Windows evidence (2026-10-20)

RTX 4090 Laptop GPU, CUDA 12.8.61, architecture 89, MSVC Release, CMake
3.31.6. GPU queried idle before execution (0 MiB, 0% utilization).
Measurements use the #210 baseline plus this child's tooling/CMake changes.
The inherited Phase2a/b/c/d and reference decision sources are unchanged.

The reproducible five-case smoke is:

```powershell
python tests/smoke_phase2a_guards.py --exe build/tests/Release/test_phase2a.exe --output-dir build/new-guard-smoke
```

It requires a new output directory and leaves all loose synthetic captures
and local logs there. Results:

| Case | Actual exit | Evidence |
| --- | --- | --- |
| New capture directory | 77 | Exactly two 900396-byte packets; strict gates passed, pixel oracle blocked |
| Valid captured packet `--decode` | 0 | open/prepare/decode=0; 1920×1080 BGRA, pitch 7680, 8294400 bytes |
| Packet larger than 2 MiB | 1 | Read rejected before SDK preparation |
| Aligned width tag changed to 1280 | 1 | Header parsed as 1280×1080, rejected before SDK preparation |
| Reuse capture directory | 1 | Rejected; both original packet SHA-256 values unchanged |

Immutable packet SHA-256:

- Host: `fc046231223e537bb221524b2d6f9c0d9f24a71ea23f79deb140f44f7bd063c8`
- Device: `6c9d54cca51919b30b3ad09c62e57f99d8207201c8fd099f2d9d9588e161992f`

They differ in one parsed frame-number header byte; the inherited strict
packet equivalence reports `FRAME_NUMBER_HEADER_ONLY`. The valid decode's
BGRA SHA is `c5e0e052be84a807a899d0501fa73f93bbe9d67e0f1c9e5cfdef97a3ffee54a0`,
PSNR 44.466 dB. This single smoke does not establish repeatability or oracle
qualification. It validates the inherited #207 guards in the resulting
#210 lineage, not an exact-head execution of #207 alone.

Native reference lane, one invocation, 35.57 s:

| Test | Inventory | Gate exit |
| --- | --- | --- |
| test_reference_oracle | PASS | 0 |
| test_reference_release_inventory | PASS (7 regression cases) | 0 |
| test_phase2a | BLOCKED (CTest skipped) | 77 |
| test_phase2b | BLOCKED (CTest skipped) | 77 |
| test_phase2c | BLOCKED (CTest skipped) | 77 |
| test_phase2d | BLOCKED (CTest skipped) | 77 |

CTest exit was 0; the runner returned **77 / BLOCKED**. In particular, the
M4-specific strict 8K capacity failure did not occur in this RTX reference
lane; no inference about M4 remediation follows.

The completed native CMake target was then checked once to verify its build
dependencies and exit propagation: the same 2 PASS / 4 BLOCKED inventory,
28.97 s, runner exit 77 and build exit 1. This checks target wiring after
the final CMake change; it is not a retry to turn blocked pixels green.

Full CPU-only Release CTest: **16/16 PASS, zero skips**, 3.10 s; after adding
the CMake target, **16/16 PASS, zero skips**, 1.93 s. The CPU qualification
target was also executed: its two available tests passed, all four missing
Phase2 tests were inventoried BLOCKED, runner exit 77 / build exit 1 as
required. No full CUDA/A2 sweep, sanitizer or performance claim.

## Integration disposition for #187

**TEST_STACK_GUARDS_VALIDATED / RELEASE_INVENTORY_BLOCKS / PIXEL_ORACLE_UNQUALIFIED.**

1. Review this child against #210; integrate only within the existing ordered
   test stack. Keep the separate M4 strict resource failure visible.
2. Keep #187 `46fc642310de8a905245696f0a3f2112817de8d5` on HOLD. Its historical
   44/45 full run remains failed. This main-based stack has not been merged
   or cherry-picked into the separate decoder phase branch.
3. For a proposed phase integration, first review a selected test-support-only
   transplant and its ancestry/diff against #187, preserving every strict
   encoder and decoder gate. Require the resulting native full suite plus
   this explicit inventory. Four reference skips still block pixel-quality
   release qualification even if every strict stage succeeds.
4. A future independent oracle needs a pre-registered same-packet matrix,
   pinned implementation/version, matching geometry/range/matrix/bit depth
   and output normalization, repeatability and justified quality thresholds.
   The current FFmpeg/original SDK evidence is not automatically that oracle.

No protected phase/main merge, release, upstream modification, retry-to-green
or conversion of BLOCKED into PASS is authorized by these measurements.

## Follow-up: positive oracle requirement (2026-10-20)

The original runner could return release PASS from five all-PASS process
results without independent evidence of a qualified pixel oracle. Schema 2
closes that gap: `reference_lane` may be PASS, but `release_gate` is BLOCKED
until an actual reviewed independent adapter is implemented. Strict failure
continues to take precedence (exit 1). Historical seven-case measurements
above remain historical; current regression count is eight.

#187 remains on its separate phase ancestry. #205 / #206 attributes the
bounded same-packet BGRA divergence to original-SDK nondeterminism; it does
not qualify pixels or rewrite the original 44/45 full-suite outcome.

Current follow-up validation: Python regression **8/8 PASS**; complete native
Windows CPU-only Release CTest **16/16 PASS, zero skips, 1.89 s**. Direct
CPU reference runner: CTest **2/2 PASS**, four absent CUDA tests BLOCKED,
independent oracle BLOCKED, runner exit **77**. No GPU rerun was needed for
this Python-only behavior change. Frozen `upstream/` status is empty.

## Follow-up: registration-bound coverage (2026-10-20, schema 3)

Schema 2 still trusted the JUnit file alone. A `status="run"` entry for a
test the exact build never registered with the `reference-oracle` label and
`SKIP_RETURN_CODE=77` could therefore be read as execution coverage, and every
`<skipped>` entry was labelled `BLOCKED` without checking the skip reason.

Schema 3 binds each run to `ctest --show-only=json-v1` for the same build,
configuration and `reference-oracle` label:

- Each required test must be registered with the lane label; the four Phase 2
  tests must also carry `SKIP_RETURN_CODE=77`. A missing registration is
  `REGISTRATION_MISSING`; a changed label or skip property is
  `REGISTRATION_MISMATCH`. Neither is coverage.
- Every registered lane test must produce exactly one result. A registered
  test without a result is `NOT_RUN`.
- A result with no registration on this build is `UNREGISTERED_RESULT` and
  fails the lane with exit 1 (stale or foreign evidence).
- A skip is `REFERENCE_ORACLE_BLOCKED` only when its reason contains
  `SKIP_RETURN_CODE=77`. Any other skip is `UNEXPECTED_SKIP`. Both remain
  BLOCKED=77 and neither certifies pixels.
- An executed test is `PASS_EXECUTION_ONLY`. Execution provenance is not
  quality proof, and the release gate stays BLOCKED=77 without a reviewed
  independent pixel oracle.

The inventory now also emits `counts` and `coverage` (required registrations,
lane registrations, results, missing/mismatched entries, `complete`), and
itemises every test in the label, not just the five required gates. Each row
carries `required`. Strict failure and nonzero CTest exit still take precedence
at exit 1. Missing or malformed registration output fails closed. Regression
coverage is 15 cases.

### Native M4 evidence (2026-10-20)

Windows 11 x64 QEMU guest, AMD EPYC 7601 8 LP no SMT; Tesla M4 CC 5.2
(3,840 MiB, driver 582.78; 10 MiB used, 0 % utilization before the run);
CUDA 12.8.93 `sm_52`; MSVC 19.44.35229; CMake 3.31.12; Ninja 1.13.2;
Release with upstream SDK and tests ON.

- Registration for the exact build/label: 6 tests. All five required tests
  registered; the four Phase 2 tests carried both `reference-oracle` and
  `SKIP_RETURN_CODE=77`.
- Focused Python regression: **15/15 PASS**.
- One native reference-lane inventory (direct runner): **2 PASS, 1 FAIL,
  3 BLOCKED**, `coverage.complete=true`, runner exit **1**.
  `test_phase2a`, `test_phase2b` and `test_phase2d` were
  `REFERENCE_ORACLE_BLOCKED` via `SKIP_RETURN_CODE=77`; `test_phase2c`
  was a `STRICT_FAILURE` (the known M4 8K 3-slot/4-stream capacity gate);
  `test_reference_oracle` and `test_reference_release_inventory` were
  `PASS_EXECUTION_ONLY`. The independent oracle remains BLOCKED.
- This is inventory/classification evidence, not a pixel-quality or
  full-suite qualification. The strict resource failure stays visible and
  was not retried away. `upstream/` status is empty.

## Follow-up: error-path schema and explicit CTest (2026-10-21)

Independent review of schema 3 raised two non-blocking hardening items; both
are addressed without changing qualification semantics.

- A registration or JUnit failure now emits the complete schema-3 key set
  (`ctest_exit`, `reference_lane`, `independent_pixel_oracle`, `counts`,
  `coverage`, `tests`) with empty counts and `coverage.complete=false`,
  instead of a truncated document. The diagnostic carries the CTest exit code
  and both output streams, because CTest can report the failure on stdout.
  A CTest run that produces no JUnit report reports the same diagnostics.
- The runner accepts `--ctest` and otherwise honours `CTEST_COMMAND`, falling
  back to `ctest` on `PATH`. The CMake target passes the CTest of the
  configuring CMake installation explicitly. This project calls
  `enable_testing()` without `include(CTest)`, so `CMAKE_CTEST_COMMAND` may be
  unset; CMake then resolves `ctest` under `${CMAKE_ROOT}/bin` and fails at
  configure time if none is found.
- Regression coverage is now 19 cases, including the error path, a missing
  JUnit report, an explicit `--ctest` and the `CTEST_COMMAND` default.

Measured at this head: focused Python regression **19/19 PASS**; one native
M4 reference-lane run against the unchanged configured build reproduced
**2 PASS / 1 FAIL / 3 BLOCKED**, `coverage.complete=true`, runner exit **1**,
with the same `test_phase2c` strict 8K capacity failure. A fresh configure
generated the target command with the CMake installation's `ctest`
executable. This is tooling evidence only: the release gate remains BLOCKED,
the original-SDK pixel oracle stays unqualified and #187 remains on HOLD.

## Capacity-bound strict failures (2026-10-24)

The native M4 lane has one persistent red row, `test_phase2c` on the 8K
DeviceInput Consolidated 3-slot/4-stream configuration. Until now the
inventory reported it as an undifferentiated `STRICT_FAILURE`, so a machine
consumer could not separate a device-capacity limit from a codec regression
without reading the log. The runner now classifies it explicitly. This changes
no gate: the row remains `state=FAIL`, `gate_exit=1` and `release_gate=FAIL`.

The classifier accepts only the allocator contract documented in
`include/hypercine/entropy_gpu.h`:

```
UNSUPPORTED_RESOURCE_BOUND: <what> allocation failed: requested <N> B;
free <N> B of <N> B; CUDA <cudaErrorName> (<message>)
```

`free ... of ...` may be replaced by `device memory query failed`. A failure
that carries this exact contract is classified `STRICT_FAILURE_CAPACITY_BOUND`
and gains a `capacity` object with `requested_bytes`, `free_bytes`,
`total_bytes` (both `null` when the device query failed) and `cuda_status`.
Anything else — a truncated message, a marker in a *passing* test's output, a
failure from a different subsystem — stays `STRICT_FAILURE`, so a capacity
claim cannot be manufactured from log text. `counts`, `reference_lane` and
`release_gate` are computed exactly as before; a capacity-bound failure has the
same weight as any other strict failure.

Native evidence (Windows 11 x64 QEMU guest, AMD EPYC 7601 8 LP; Tesla M4
CC 5.2, 3,840 MiB; CUDA 12.8.93 `sm_52`; MSVC 19.44 Release), CUDA-ON build
from `471da206e02c9373b9a30cd2cc2d77af87c7b93c`:

- Full registered CTest: **34 tests, 30 passed / 3 SKIP77 / 1 failed**,
  413.61 s. The only failure is `test_phase2c`; the count is identical to the
  pre-change run at the same commit (376.60 s), so the classifier changed no
  outcome.
- Reference-lane inventory: **2 PASS / 1 FAIL / 3 BLOCKED**,
  `coverage.complete=true`, runner exit **1**, `release_gate=FAIL`.
  `test_phase2c` is `STRICT_FAILURE_CAPACITY_BOUND` with
  `requested_bytes=33178624`, `free_bytes=23789568`,
  `total_bytes=3953131520`, `cuda_status=cudaErrorMemoryAllocation`.
- The 8K row was not retried or swept again; the same bounded run produced
  both the CTest summary and the inventory.

This is representation and tooling evidence only. It does not qualify the
M4 for 8K 3x4, does not convert any failure to a skip or pass, and does not
change the original-SDK pixel-oracle status, which remains BLOCKED.
