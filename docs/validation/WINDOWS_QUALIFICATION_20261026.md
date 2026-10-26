<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Windows independent qualification — 2026-10-26

This is a bounded Windows acceptance report for the development briefing. It
separates current measurements, retained historical evidence and unavailable
qualification. No production decoder, ISA dispatch, merge or release is approved.

Source baseline: `81d8a94` (merged CUDA A2 #249), isolated branch
`codex/windows-qualification-20261026`. Codec sources, public headers and frozen
`upstream/` are unchanged. The pre-existing dirty primary checkout and other
active PR branches were not edited.

Host/toolchain: Windows 11 x64, MSVC 19.44.35229.0, CMake 3.31.6, CUDA 12.8.61,
RTX 4090 Laptop GPU, compute capability 8.9, driver 610.62, 16,376 MiB reported
device memory. FFmpeg `n9.0.1-11-ge47273f4d9-20260831`. No Tesla M4/SM52 run.
The first sandbox CUDA compiler probe failed with MSVC C1902; native execution
outside the sandbox built successfully. This was not treated as a codec failure.

## 1. PR decisions

| PR / exact head | Independent Windows evidence | Decision |
| --- | --- | --- |
| [#263](https://github.com/louis-0hrs/HyperCine/pull/263), `fb0c2c2743e4aa9f9699f1899c39d3b9d81aebbd` | Release and Debug build; six-geometry capability/FFmpeg matrix PASS; independent non-30-fps contract FAIL in both configurations | **HOLD / reject requested-timebase promise** |
| [#261](https://github.com/louis-0hrs/HyperCine/pull/261), `48b43d791208e204ca826dfa2fbcd5022f5be6b3` | Native full Release CTest 61/61 PASS, zero skips, 731.11 s; independent concurrent-session/failure/packet harness PASS | **CPU behavior accepted within this scope; integration HOLD** |

Reviews were posted directly to [#263](https://github.com/louis-0hrs/HyperCine/pull/263#issuecomment-6421591173)
and [#261](https://github.com/louis-0hrs/HyperCine/pull/261#issuecomment-6421607673).
The elapsed full-suite time includes simultaneous host qualification and is not
a performance measurement.

**#263:** 24/1, 25/1, 50/1 and 60000/1001 all return
`allocation_permitted=1 / ExperimentallySupported`, despite the metadata-less
writer's fixed 30-fps TIMC arithmetic. 30/1 correctly permits allocation; 0/1
and 30/0 correctly reject. Reject unsupported timebases or explicitly model
external timing metadata before integration. No writer redesign was attempted.
The original matrix passes 128×24, 256×256, 1280×720, 1920×1080, 2048×1080 and
3840×2160, plus its malformed-storage, memory-budget, unqualified-geometry and
unsupported-backend cases. A valid geometry remains distinct from qualified
profile/backend support. The request object itself has generic dimensions;
its evidence whitelist is a separate implementation policy.

The PR description and earlier comments are stale about registration: root
CMake at this exact head already compiles `encoder_capability.cpp` into
`hypercine_standalone` and registers `encoder_capability_contract`. The prior
native full-suite record is 62/62; this review reran the matrix independently in
Release and Debug. Public legacy API headers are not changed by #263; the new
contract is internal. No shared-library binary ABI compatibility test was run.

**#261:** three independent encoder objects run simultaneously, one hash ON and
two OFF, four deterministic frames each. All 12 packets, quant/scale tables and
rate states agree. Full/Full accepts real evidence; OFF/OFF, ON/OFF, absent/absent
and a mutated Full hash reject. Invalid-input failures and a late sample-capacity
failure preserve packet/rate state; the next successful packet equals a fresh
session. Both hash modes reproduce the pinned autonomous 1080p flat packet.
Existing SDK-reference-manifest gates pass in the full suite; the complete
external raw SDK corpus was not available for a fresh binary re-audit.

GitHub reports #261 conflicts with current main after #262. Diagnostic serials
are distinct from rate state: `StampDiagnosticOff` runs before final sample
closure and can consume a serial on failure. No diagnostic rollback or permanent
tombstone uniqueness is asserted. Metal/Vulkan hash-off behavior and the newer
CUDA integration are not qualified by this old CPU-only head. Rebuild consumers
when changing these public C++ object layouts; no binary ABI promise follows.

## 2. Original-SDK corpus coverage (#36)

The shared `HYPERCINE_GOLDEN_DIR` location is **BLOCKED_SHARED_CORPUS_STORAGE**,
also when tested read-only outside the sandbox. Nothing was regenerated, moved
or overwritten. In particular the existing 20-part, 1,760-file YUV archive was
not recreated.

The retained F1 exporter at `f17355f2f391410fc2cf27065e54dcffe1ae45e7` verified all
**28 local diagnostic/smoke subsets** again. Each contains one exported case,
four transfer rows and five verified artifacts including its manifest. These
are repeated small investigations, not 28 unique format/profile qualifications.
The sanitized [inventory](windows-qualification-20261026/retained-f1-inventory.json)
retains case parameters, dimensions, pitch, input/sample/normalized/decode hashes,
SDK version fields, layouts, quality and measured comparison data.

| Family / capability | Retained native evidence | Current qualification limit |
| --- | --- | --- |
| YUV422 progressive GOP1 | Historical F0: BGRA/BGRa/YUY2/2vuy/v210/YU64/RG24, all six regular qualities; existing 1080p 16/120-frame reference manifests | Current source/coefficients/manifest checks pass; full external bytes cannot be reverified here. Coded p10 is distinct from input storage depth. |
| RGB444 | F0: RG24/RG48/BGRA/RG30/AB10/AR10/r210/DPX0. Historical F1: 128 px matrix and 16 canonical RG48 1080p cases, FS1/FS3 | RG48 top-down RGB16; RG24 bottom-up BGR8; SDK output RG48. No general pixel-oracle qualification. |
| RGBA4444 | F0: BGRA/BGRa/b64a/RG64. Historical F1: 128 px matrix and 14 RG64 1080p cases, FS1/FS3 | SDK output b64a is A,R,G,B16; alpha has a coded curve and is not a bit-exact 16-bit roundtrip. |
| Bayer | F0/F1 BYR4, including 14 historical 1080p FS1/FS3 cases; retained subsets reverified | Default CFA only; source storage16 versus reported coded log12. Dither makes hashes run-dependent. CFA variants and internal pre-quant stages remain BLOCKED. |
| YUVA4444 | F0 encoding returns data but decode fails (12 quality/source cases) | **Unsupported usable SDK mode**, not bidirectional support. |
| Unsupported inputs | yuyv, WP13, b48r, W13A, BYR2, BYR5 rejected at their measured F0 stages | Do not infer support from enums or PrepareToEncode success. |
| 4K RGB/RGBA/Bayer | [Draft #165](https://github.com/louis-0hrs/HyperCine/pull/165), `44961880e40e95a241191ae8b8a87472d3cd4992`: historical 10/10 native exports, FS1/FS3 | Reported source/normalized/output hashes exist in that PR; full raw files unavailable for this audit. It remains stacked on #156. |
| GOP2 / interlaced | Existing GOP2 inventory and original-stage contracts execute | Not a complete native encode/decode corpus or pixel qualification. |
| Stereo/3D, KEYING, uncompressed, 8K/CFA variants | No new end-to-end evidence | **INCONCLUSIVE / not qualified**; stereo is a separate capability axis. |

Historical totals must not be confused with today's local inventory:
F0 [#82](https://github.com/louis-0hrs/HyperCine/pull/82) records 168 probes:
120 bidirectional API successes, 36 unsupported, 12 decode failures.
F1 [#156](https://github.com/louis-0hrs/HyperCine/pull/156) records 222 small cases
and 44 canonical 1080p cases. API success and reported coded precision do not
independently prove correct pixels or internal signed-stage equivalence.

Provenance gap: retained F1 manifests bind artifacts, SDK baseline, compiler,
generator name and seed, but omit an exact exporter source commit. The current
exporter checkout is recorded separately; it cannot retroactively authenticate
the original generating binary. Preserve this distinction when accepting F1/F2.
Independent FFmpeg per-case qualification for RGB/RGBA/Bayer remains unavailable.

## 3. Build/test results and decoder evidence (#120/#195)

Initial unmodified SDK/CUDA Release run: 51 registered, **44 PASS / 1 FAIL /
6 BLOCKED**, 392.70 s. The failure was the Visual-Studio-blind SDK build-rule
test, fixed below. Initial SDK-free CUDA run: 70 registered, **69 PASS / 1 FAIL**,
615.51 s. Its failure was missing Windows symlink-creation privilege.
Both initial results are retained; a rerun does not erase a failure.

| Final complete Release run | Registered | PASS | FAIL | BLOCKED | Time |
| --- | ---: | ---: | ---: | ---: | ---: |
| SDK ON, CUDA ON, SM89 | 52 | 46 | 0 | 6 | 345.46 s |
| SDK OFF, standalone CUDA ON, SM89 | 71 | 70 | 0 | 1 | 555.76 s |

Final results are in [SDK/CUDA](windows-qualification-20261026/sdk-cuda-final.json)
and [standalone CUDA](windows-qualification-20261026/standalone-cuda-final.json).
CTest itself exits zero with these skips; the qualification summary deliberately
returns **BLOCKED**, not full acceptance. Its status concerns registered
contracts, not decoder production readiness.

Native CI Python discovery additionally ran 206 methods: 186 PASS, 17
platform/prerequisite skips, three FAIL. All three failures came from the
CUDA-inventory test's POSIX-only fake ctest executable. After correction that
entire eight-method group passes, including removal of a registration and
deliberately inconsistent CTest counts. The changed standalone required-inventory
group also passes all 11 methods. A second complete 206-method discovery was not
run; these targeted reruns do not retroactively turn the initial run green.

The bounded oracle protocol uses immutable SHA/length-bound packets and a fixed
number of fresh decoder instances. It records every output; there are no
retry-until-pass loops. Strict storage, parsing, allocation/API errors, geometry,
pitch, nonzero-output, coefficients, entropy and packet identity remain fatal.
Only reference-dependent pixel claims are blocked when an oracle is unavailable
or unstable. A later strict fixture failure dominates an earlier quarantine.

`test_phase2a`, `test_phase2b`, `test_phase2c`, `test_phase2d` and
`standalone_sdk_decode_3_packets` use return77 with `reference-oracle` labels.
`test_filmscan3_windows_decoder` can return77 on disagreement; stable repeats
prove repeatability only. `test_phase3a` retains its existing hard PSNR/frame-stamp
gate; a successful run does not independently qualify the SDK pixel oracle.
The new S3/scalar inverse tests from #195/#260 are bounded synthetic contracts,
not production whole-decoder parity.

FFmpeg qualification is packet/build/layout-specific. The existing harness
permits measured n8.0/9.0.2 builds on three pinned 128×128 YUV422 p10 fixtures with
wrong-packet and byte-mutation controls. This host's explicitly tested n9.0.1
build returns77 because it is off that measured list. The list was not relaxed.
Successful FFmpeg extent/structure tests for other frames are not full pixel
oracles, nor qualification of 1080p/4K/RGB/RGBA/Bayer/stereo.

## 4. x86 SIMD (#69/#65)

Main contains bounded CPUID/XGETBV capability detection and a validated-backend
registry containing scalar only. Production `Auto` resolves scalar. No global
unsynchronized override was introduced.

Existing [#77](https://github.com/louis-0hrs/HyperCine/pull/77) at
`3f612a83a286815a7969d1759b33753bb2f821a3` supplies explicit experimental
horizontal/vertical 2/6 AVX2 and AVX-512 kernels plus a test-only FilmScan3
frontend. It is not merged and conflicts with main. There is no newly qualified
AVX quantizer/color/entropy implementation; quantization in those injected
sessions remains the common path.

Fresh Release retest: **2,174 cases per scalar/AVX2/AVX-512, 6,522 total**, and
**16 ordered full FilmScan3 frames**, all PASS (25.34 s). The tests compare the
independent scalar kernels, negative/full-int16/extreme patterns, strides,
unaligned starts, vector tails, poisoned padding, redzones and inaccessible-page
boundaries. Both AVX2 and AVX-512 execute on this actual host after CPU/OS gates.
The earlier same-head ASan record is historical; no new sanitizer run is claimed.

The [performance data](windows-qualification-20261026/performance.json) separate
31 measured kernel calls (three warmups) at 1080p/4K from 16 measured complete
1080p frames (three warmups), one worker. Complete-frame means were scalar
135.405 ms, AVX2 108.606 ms, AVX-512 107.194 ms; these are one unpinned process,
OS-managed power/clocks and test injection. 4K complete SIMD sessions remain
unmeasured. No production acceleration multiplier is accepted.

The mandatory current `benchmark_codec` comparison measured 1080p 52.7194 FPS
versus historical 77.6958, and 4K 13.7653 versus 21.8373. Avg/min/max latency,
MPix/s and peak memory are retained in the JSON. The candidate changes only
tests/reporting and has no diff in codec/public/upstream/benchmark sources, so
this is not evidence of a regression caused by these changes. The lower
historical-baseline throughput still requires a controlled paired experiment;
performance acceptance is **INCONCLUSIVE**, and no optimization is promoted.

## 5. CUDA compatibility (#249)

Both SDK-ON and SDK-free CUDA-ON native Release builds succeed for explicitly
requested SM89. SDK-free target inventory contains no CFHDCodecStatic dependency.
The SDK-OFF/non-standalone CUDA mode still rejects at configuration before CUDA
setup. No general support claim is made for architectures not run on this host;
compiler-identification defaults are not target-architecture qualification.

The existing CPU/CUDA complete-packet gate, three stage/packet negative controls
and independent FFmpeg decode run on the physical GPU. Additional ordered
three-frame 1280×720, 2048×1080 and 3840×2160 comparisons and malformed/over-budget
geometry checks are [recorded separately](windows-qualification-20261026/cuda-geometry.json).
All three ordered sequences pass: 4,707,748 / 11,274,552 / 42,238,336 total packet
bytes respectively, with identical rate state and frame counters. Requests
1920×1081, 4096×2160 and 7680×4320 each reject with exit1; dimensions are not
silently rounded or cropped. This qualifies comparison with the
existing scalar implementation, not a fresh original-SDK 4K pixel reference.

Source inspection confirms checked CUDA allocation/copy/stream errors throw and
RAII owns streams/device buffers. There is no silent CPU fallback in this
private CUDA frontend. Conversion and prescale2 still use the existing CPU code;
do not call it an entirely GPU-resident encoder. Device-loss and allocation-error
injection were not performed: their runtime qualification is **INCONCLUSIVE**.
The common session/capability contract is not wired into this frontend, and no
new public CUDA API or fallback policy was invented.

## 6. Windows regression improvements

1. `test_upstream_off_contract.py` originally searched Makefile/build.ninja only,
   falsely failing SDK-ON under native Visual Studio. It now requests CMake's
   generated codemodel and checks exact executable name/type. Missing/malformed
   evidence remains fatal; library/similarly named targets fail the negative
   controls. Actual ON/OFF/CUDA-invalid configurations still run.
2. CUDA output-directory testing previously failed before any sentinel check on
   Windows error1314. Only that specific missing privilege yields return77,
   **after** all possible file/directory preservation checks. Live symlink
   checks remain explicitly unexecuted/BLOCKED on this host. Unrelated creation
   errors and sentinel corruption remain FAIL. Mutation-based negative coverage
   proves a corrupted sentinel dominates the missing privilege. No production
   output-directory helper changed; no live-symlink PASS is claimed.
3. `ci/summarize_qualification.py` combines matching CTest json-v1 inventory,
   JUnit and actual exit code. FAIL dominates partial evidence; labeled oracle,
   GPU and platform return77 become BLOCKED; missing/unregistered/disabled or
   unknown evidence is INCONCLUSIVE. Duplicate/malformed evidence fails closed.
   A nonzero process cannot become green through a partial result document.
   PASS is explicitly contract execution only. The helper preserves the existing
   stricter standalone CI policy that rejects any skipped required test.
4. The CUDA-inventory negative-control suite used a `/bin/sh` fake ctest and
   could not run on Windows (error193), obscuring the intended assertion. Its
   Windows-only launcher now invokes the same synthetic listing through Python
   and a native command file. Production inventory code and failure criteria
   stay unchanged; all eight original positive/negative cases run successfully.

New Python contracts are registered in both SDK and standalone modes and are
required by standalone CI's inventory. Standalone review harnesses remain
opt-in, because the #263 negative test is deliberately red on the rejected head.
No real codec failure was suppressed or strict encoder parity criterion changed.

## 7. Reproduction

Use an authorized native MSVC environment. Native build artifacts/logs remain
private in ignored build directories. To summarize one complete matching run:

```powershell
ctest --test-dir build -C Release --show-only=json-v1 > build/inventory.json
ctest --test-dir build -C Release --output-on-failure --output-junit results.xml
$testExit = $LASTEXITCODE
python ci/summarize_qualification.py --inventory build/inventory.json `
  --junit build/results.xml --ctest-exit $testExit `
  --require qualification_report_contract --require cuda_output_dir_platform_contract
```

The summary exits 0 PASS, 1 FAIL, 77 BLOCKED or 2 INCONCLUSIVE. It never promotes
decoder production status. Archive inventory and results from the same run;
do not mix configurations or reuse stale JUnit.

Independent unmodified-head reviews (set `REVIEW_SOURCE` to the pinned checkout):

```powershell
cmake -S tests/windows_review -B build-review -G "Visual Studio 17 2022" -A x64 `
  -DREVIEW_SOURCE="$env:REVIEW_SOURCE" -DREVIEW_KIND=hashing
cmake --build build-review --config Release
ctest --test-dir build-review -C Release --output-on-failure
```

For #263 use `REVIEW_KIND=capability`, provide FFmpeg/ffprobe through the
`HYPERCINE_FFMPEG_EXECUTABLE`/`HYPERCINE_FFPROBE_EXECUTABLE` environment variables,
and run both Release and Debug. Expect the unsupported-timebase test to fail on
`fb0c2c2`; the matrix should pass. This independent review project does not patch
the reviewed source or add registration to its normal suite.

## 8. Delivery, architecture decisions and next steps

The branch contains test/reporting changes only and is intended as a Draft PR.
No main merge, production promotion or existing PR branch rewrite is authorized.
Exact delivered commit and Draft URL are reported with the handoff.

1. Linux owner fixes #263 requested-timebase semantics, reconciles current main,
   refreshes stale description and runs exact-head registered CI. Windows then
   reruns this unchanged negative harness until the unsupported rates reject.
2. Linux owner resolves #261/#262 overlap and explicitly chooses diagnostic
   serial failure semantics. Keep full hashes mandatory for parity gates; Mac
   owns native Metal/Vulkan follow-up as applicable.
3. Restore read-only external corpus access, reverify the existing transfer SHA
   inventories, then independently accept the stacked #82/#156/#165 evidence.
   Record exact generator/build provenance in future exports. Do not regenerate
   existing YUV multi-GB material or assert unavailable CFA/stereo support.
4. Keep #120 decoder release blocked. Review an oracle adapter per packet/build/
   layout with genuine negative controls; qualify the local FFmpeg build before
   considering an allowlist extension. SDK API success or stable repeated hashes
   alone are insufficient.
5. Reconcile #77 through its owner and #259's per-session ISA policy. Retain
   capability+validated-backend gating, common scalar fallback and no process
   globals. Measure full 4K sessions and controlled paired performance before
   production dispatch acceptance.
6. Add bounded CUDA allocation/device-loss fault injection through private test
   seams, explicit requested profile/geometry errors and session-authorized
   fallback only. Run SM52 separately on a real M4 host; do not infer it from SM89.
