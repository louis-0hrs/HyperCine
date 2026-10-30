# Issue #16: concrete phase oracle-policy candidate

Copyright (c) 2026 HyperCine Contributors. Dual-licensed MIT OR Apache-2.0.

Status: **REVIEW / NO PRODUCTION PROMOTION**, native Windows, 2026-10-30.
This candidate is based exactly on the already-landed phase head
`b11b3da4c6be4cc7005cf634162e3bd1b662f387`, on a separate branch. It does not
merge or modify `decode-phase3b2a`, protected main, or another owner's branch.

## Corrected current state

- #187 is **merged** into `decode-phase3b2a`; repeating its old landing proposal
  would be wrong. Its resulting phase commit is `b11b3da`.
- #190 is **merged** into main at `638bcb705a29702e2e3e05eef6fa327aa9d9be5d`;
  the owner accepted the narrowly bounded two-packet signed-path reference.
  Do not reopen its child stack or export/copy reference corpora again.
- The A2 owner measured `b11b3da`: 44/44 CTest, zero skips, 12/12 actual Compute
  Sanitizer cells. This is separate, attributed Linux evidence, not a new
  Windows measurement or independent pixel-oracle qualification.
- The old Windows 44/45 result remains a historical failure. Attribution and
  later tests do not rewrite it as 45/45.
- #120's reviewed quarantine/release tools now exist on main, but were absent
  from the phase head. #335's independent 1080p observation retains a failed
  rejection sentinel and does not qualify a replacement pixel oracle.

Live evidence: [#16 resulting-head A2 report](https://github.com/louis-0hrs/HyperCine/issues/16#issuecomment-6495970618),
[#187](https://github.com/louis-0hrs/HyperCine/pull/187),
[#190](https://github.com/louis-0hrs/HyperCine/pull/190),
[#335](https://github.com/louis-0hrs/HyperCine/pull/335).

## Source selection and boundaries

Thirteen explicitly selected files are copied byte for byte from reviewed main
`04c348f848de1eb3a47d3b90fe191722c1ab559e`, without importing its ancestry or
replacing the phase CMake files:

- `tests/reference_oracle.h`, `test_reference_oracle.cpp` and Phase2A/B/C/D tests;
- `tests/diagnostic_decode_guard.h`, `smoke_phase2a_guards.py`,
  `test_reference_release.py`, and `tools/check_reference_release.py`;
- the existing publication scanner and two hooks, installed locally for this
  previously unhooked phase lineage. They are unchanged main blobs.

The phase test CMake file receives only additive oracle-contract/inventory
registration, the diagnostic geometry selftest, four `SKIP_RETURN_CODE=77`
properties with the reference-lane label, and the explicit release-gate target.
All original test declarations remain. No whole-file CMake transplant occurs.

**CUDA sources, CPU decoder sources, public headers, upstream, and the existing
19-case failclosed test remain unchanged from `b11b3da`.** The only source file
change is the reviewed #210 verification-helper condition inside
`VerifyPhase2DMetadataSafety` in `src/core/phase2d_encoder.cpp`:

```cpp
if (!r.bitstream_byte_match || !r.decode_ok || r.last_bitstream.empty()) return false;
```

This replaces the aggregate `r.ok` check, which also includes the unqualified
original-SDK PSNR. It retains encoded-byte, decode-API and nonempty-packet
requirements; later metadata equality checks are unchanged. No encoding math,
GPU lifecycle, dispatch, allocation policy or public API is modified. Because
this helper is under `src/`, the complete source tree is **not** claimed to be
identical to the phase parent; identities are recorded per unchanged scope.

## Gate behavior and retained caveats

Strict encoder coefficients, entropy/payload bytes, parser-attributed header
differences, geometry, metadata, resources and API failures retain exit 1.
An unqualified or unstable original-SDK pixel reference returns 77, recorded
as a CTest skip. Stable repeats alone cannot return pixel-quality PASS.
The release runner independently requires a reviewed implemented pixel oracle;
all-PASS CTest execution is insufficient and no bypass flag is added.

The existing Phase3A sequence test and its SDK-dependent pixel/stamp checks
are unchanged (callsite #5 in the mainline #120 inventory). This transplant
does not claim that every historical SDK pixel callsite has been migrated.
Its execution result cannot qualify pixels while the global release oracle
remains blocked; any strict failure there is retained rather than skipped.

The five historical #187 observations remain bounded as follows:

| Observation | Disposition for this candidate |
| --- | --- |
| DAG frame-error-copy failpoint absent | Already resolved in parent: nineteenth case is retained unchanged |
| Late synthetic failed-ticket async lifetime | Retain test-seam limitation; no concurrent/different-input lifetime claim |
| Global test error accessor attribution | Retain single-threaded test scope; no per-ticket concurrent attribution claim |
| Some submission failures have empty human diagnostic | Retain nonblocking observability limitation; error/ticket suppression still tested |
| Poll fast path under artificial post-complete failure | Retain synthetic late-injection limit; no arbitrary physical-driver-failure claim |

Existing #190 reference acceptance remains limited to exactly two original
1080p/4K progressive FilmScan3 FIELDPLUS/GOP2 packets, frames 0/1, YUY2 8-bit
source encoded to p10. Its 224+180 comparisons are named-stage records, not
404 independent samples. This candidate does not rerun or broaden that private
reference proof, repair original SDK threading, or qualify GOP1/general pixels.

The live issue states were checked separately: #2 and #13 remain **OPEN**;
#14 is **CLOSED / completed**. Retain #14's recorded performance/resource scope
without a new speed claim. This test-policy candidate does not close #2/#13,
resolve the historic heap failure or grant production acceptance through #16.

## Ancestry and mainline conflict preview

All adjacent phase ancestry relations are verified:

| Branch | Pinned head |
| --- | --- |
| decode-phase3af | `9eb9e3d5e39aceb4cf8b17083a83e431272a1aaf` |
| decode-phase3b0 | `7296f6f3b60ae4d9b902415f1a531f20f4ba70ad` |
| decode-phase3b1 | `f1d0fe4258ad70113a9de57e85ab9834d29312cc` |
| decode-phase3b2r | `f433a7491b12b7e053fec0bab48b4ba784e7d3d9` |
| decode-phase3b2a | `b11b3da4c6be4cc7005cf634162e3bd1b662f387` |

At pinned main `04c348f`, each phase lacks **181 main-only commits**;
phase-only counts are 34/37/40/44/57 respectively. These measured graph counts
replace the older 168-commit observation, without treating them as file counts.

`git merge-tree --write-tree --name-only --no-messages origin/main
origin/decode-phase3af` returns **1** and identifies five first-landing
conflicts: `benchmarks/CMakeLists.txt`, `benchmarks/benchmark_codec.cpp`,
`docs/UPSTREAM.md`, `src/CMakeLists.txt`, `tests/CMakeLists.txt`.
This creates only temporary Git objects, not a branch merge or reviewed result.
No conflict resolution or mainline integration is claimed here.

## Native validation protocol

Build separate CPU and native sm89 CUDA Release configurations, benchmarks OFF.
Keep the existing 19-failpoint suite and every test enabled by those settings.
Run the five-case diagnostic guard smoke on the actual candidate executable,
full CPU CTest, and exactly one full CUDA CTest pass. Retain failures and skips
separately; do not retry a failed full run to obtain a green report. Apply the
release runner after native evidence and retain its independent BLOCKED state.

With benchmarks OFF the baseline enables **44 CUDA tests**, plus three new
registrations here, totaling **47**. An initial declaration-based preflight
incorrectly expected benchmark-only `test_decode_phase2fb` as well. Inspection
of its unchanged `CUDA AND BUILD_BENCHMARKS` guard corrected that bookkeeping;
the final evaluated inventory retains all 44 baseline-enabled tests. The CPU
configuration enables its own nine tests, including two new contracts.

The native five-case smoke verifies new capture (77), valid bounded decode (0),
oversized input (1), wrong geometry (1), and existing capture rejection (1),
then rehashes both captures to prove they were not overwritten. This is a new
small synthetic diagnostic output, not a reference-corpus regeneration.

Builds retain existing upstream deprecation/runtime-library warnings. They do
not establish an all-path host-ASan result or fix the separately documented
Windows heap-corruption history. No fresh sanitizer or benchmark run is claimed;
unchanged decoder-source historical evidence keeps its original scope.

## Measured candidate results

Native Windows x64, RTX 4090 Laptop sm89, MSVC 19.44.35229.0, CUDA 12.8.61,
CMake 3.31.6, Python 3.10.9; both Release builds succeed.

| Check | Actual outcome |
| --- | --- |
| Complete CPU CTest | **9 PASS / 0 FAIL / 0 SKIP**, 5.98 s |
| Complete CUDA CTest, one full pass | **43 PASS / 0 FAIL / 4 SKIP**, 47 registered, 778.94 s |
| Native diagnostic smoke | **5/5**, exact 77/0/1/1/1 outcomes; original capture hashes unchanged |
| Release inventory Python contract | **27/27** SDK-free methods |
| Nineteen-case failclosed target | PASS; DAG copy hits=1, ready hits=0; unchanged test source |
| Explicit CUDA release target | MSBuild target exit **1**, underlying runner **77 / BLOCKED** |

The four CUDA skips are exactly Phase2A/B/C/D. Each completes its strict gates
before reporting `BLOCKED_ORACLE_NOT_QUALIFIED`. Stable reference observations
in this invocation do not remove historical nondeterminism or qualify pixels.
All phase decoder targets pass, including Phase2FA stress (461.05 s), mixed
stress, temporal/reconstruction and the retained failclosed target. A single
passing Phase3B0 invocation is not a heap-corruption fix.

The CUDA release inventory has complete 5/5 required registration/result coverage:
**1 PASS / 0 FAIL / 4 BLOCKED**, and independent pixel oracle false. The CPU
release inventory also returns 77 but explicitly lacks the four CUDA test
registrations. Neither is a release PASS. The custom target's nonzero build
result is expected enforcement of that condition, not a hidden successful gate.

CTest truncated successful failclosed JUnit output at 1024 bytes. After the full
suite, one focused execution (exit 0) retained the complete nineteen-case and
DAG-copy witness in a private log. The full CUDA suite was not repeated.
The release target's bounded reference-lane execution is recorded separately
from the full-suite results.

The tests ran before the evidence commit, on the modified worktree based at
`b11b3da`; final code bytes are identified by hashes, not mislabeled as a clean
pre-existing commit. Public evidence contains summaries, source/executable hashes
and registration, never raw logs or synthetic captures:

- [Source selection, preserved scopes and ancestry](evidence/issue16-phase-source-20261030.json)
- [Native per-test validation and guard results](evidence/issue16-native-validation-20261030.json)
- [CUDA release inventory](evidence/issue16-cuda-release-20261030.json)
- [CPU release inventory](evidence/issue16-cpu-release-20261030.json)

Reproduction from this candidate:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DHYPERCINE_ENABLE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89 `
  -DHYPERCINE_BUILD_BENCHMARKS=OFF
cmake --build build --config Release --parallel 4 -- /nodeReuse:false
ctest --test-dir build -C Release --output-on-failure
cmake --build build --config Release --target reference_release_gate -- /nodeReuse:false
```

Use a separate `build-cpu` with CUDA OFF for its full suite. For the manual
smoke, `python -B tests/smoke_phase2a_guards.py --exe
build/tests/Release/test_phase2a.exe --output-dir build/new-guard-evidence`
requires a new output directory and must exit 0. The explicit release target
must remain nonzero until a reviewed independent pixel oracle is implemented.

## Proposed disposition

Review this concrete child against `decode-phase3b2a`, not as a direct main
promotion. Recheck source identities, registration and native evidence before
any owner-approved phase merge. If accepted, record the resulting phase SHA;
no old A2 or Windows head is automatically relabeled as that SHA.

Keep #16 and pixel-release acceptance open. A future mainline proposal must
resolve the five actual first-landing conflicts, preserve main's newer build,
test and encoder contracts, and qualify its resulting tree. Then land milestones
in order, explicitly retargeting each next child to main as its parent lands.
Do not merge descendants into already-landed temporary parents and assume main
advanced. Independent pixel correctness remains a separate blocked condition.
