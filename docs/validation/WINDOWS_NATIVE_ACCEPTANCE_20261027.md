<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Windows native integration acceptance, 2026-10-27

This is the follow-up to #276. Integration base is
`a267db4f1d9f41955c59675f2bd3690c30f7acb5`, with #276 head
`cedb892917797f86212e95d311111cd772806b59`. Tested source revision:
`37e6bcfcecc65bcf4945b96e99cbc2c6a0a5d5c3`. The native builds preceded sealing
this source snapshot as a commit; no
implementation changed afterwards. Configure-time HEAD was the main base with
integration changes pending. Executable digests bind the measured binaries. The
subsequent evidence commit changes documentation only. Historical results under
`windows-qualification-20261027/` still describe
their original `44827e8` baseline; the new results live under
[`windows-native-20261027/`](windows-native-20261027/).

## Platform and export boundary

Windows x64; Ryzen 9 7940HS, 8 physical cores / 16 logical processors;
RTX 4090 Laptop GPU, SM89, driver 610.62; MSVC 19.44.35229.0, Windows SDK
10.0.26100.0, CUDA 12.8.61, CMake 3.31.6, Release. The measured ISA inventory
reports SSE2, AVX2 OS state and AVX512F OS state. This is an x64 qualification,
not a separate Win32 build or an old-CPU hardware qualification.

The process runner accepts fixed stage/source/header labels, field-specific
integer bounds, null-only unobserved worker counts, validated hexadecimal masks
and a typed topology/ISA allowlist. Booleans and floating-point values cannot
stand in for integer fields. Duplicate JSON keys and nonfinite constants are
rejected. An inventory or process exception produces a fixed FAIL document;
exception text, stderr, arbitrary nested properties and process strings never
enter the exported report. Nineteen contract tests include forged valid JSON
containing synthetic path/host/credential markers and fixed-failure CLI output.

## Native execution

| Configuration | Final complete CTest result |
|---|---|
| SDK-free CPU, required FFmpeg | 75 PASS / 1 BLOCKED / 0 FAIL, 76 registered; 612.84 s |
| SDK-free CUDA ON / SM89, required FFmpeg | 81 PASS / 1 BLOCKED / 0 FAIL, 82 registered; 983.73 s |
| Frozen original SDK, CPU | 43 PASS / 2 BLOCKED / 0 FAIL, 45 registered |

The JSON summaries preserve each registered test's status, including skips.
The platform-only output-directory negative is BLOCKED on Windows; SDK pixel
gates remain BLOCKED because their reference oracle is unqualified.

The first SDK run exposed a Windows ANSI decoding failure in the configure
contract. The harness now explicitly decodes CMake diagnostics as UTF-8, with
replacement confined to diagnostic text; actual exit codes and target/registration
assertions remain strict. A second run then exposed Windows MAX_PATH in nested
compiler checks under the long worktree TEMP directory. A short local TEMP/TMP
directory fixed that prerequisite; the eight-case configure contract and a new
complete SDK suite passed. Both failed full-run summaries are retained separately
as `sdk-cpu-initial.json` and `sdk-cpu-long-temp.json`.

The first CPU full run overlapped the two other large standalone suites. Its 4K
external-decode test hit the unchanged 600-second timeout. That run is retained;
it is not promoted to PASS by the separate sequence measurements below. After
the other two large suites finished, the new complete CPU run passed with the
unchanged 600-second limit: the 4K test took 273.07 s. The initial timeout run
is separately retained as `standalone-cpu-concurrent.json`. Small benchmark
measurements overlapped the later, smaller tests, after the large 4K case passed.

Initial sandbox build attempts failed before useful qualification (MSBuild
process communication / CUDA PDB-manager error). Native builds used a local
temporary directory and disabled MSBuild node reuse. Raw logs and JUnit remain
in ignored build directories, not in the public reports.

## Packet, independent decode and reference results

[`cpu-cuda-sequences.json`](windows-native-20261027/cpu-cuda-sequences.json)
records 25 complete CPU/CUDA packet pairs: 3 at 1280x720, 16 at 1920x1080,
3 at 2048x1080 and 3 at 3840x2160. Every on-disk packet pair agrees. Each side
was decoded by a separately launched FFmpeg process: all 50 decoded extents
and CPU/CUDA output hashes agree. Comparator and FFmpeg executable SHA-256
values and every packet/output SHA-256 are recorded. This proves own-frontend
parity and independent decodability, not original GoPro encoder equivalence.

All four [native protection negatives](windows-native-20261027/native-probe-guards.json)
pass: invalid thread count, invalid packet, existing output and input/output
collision. The immutable sample digest and sentinel output bytes remain intact.

The fixed [fresh-process matrix](windows-native-20261027/reference-process.json)
ran 21 processes, three per preregistered cell, without retries or best-result
selection. All 21 API/extent observations succeeded, but three different pixel
hashes occurred, including differences within identical configurations:
**BLOCKED_NONDETERMINISTIC_REFERENCE**, exit 77. Requested threads are not
observed decoder worker counts. The output domain is exactly 8,294,400 image
bytes, YU64, pitch 7680. Other native qualification activity overlapped this
bounded run; no causal timing/worker-count conclusion is drawn.

## Remaining independent lanes

For #274, fetched exact head remains
`c52392e159ea7252d63a63d2f6f47fadd80662fa`. A native CUDA-ON MSVC recheck fails
with C1083 at `tests/test_cuda_multigeometry.cpp:10`, missing `sys/wait.h`.
POSIX process/temp operations and global cross-session fault-injection state
remain in that head. The target did not execute. Status:
**BLOCKED_LINUX_CORRECTION_NOT_PUBLISHED**. The exact result and next required
Linux-owner correction were reported in the #274 discussion. Main-based packet
equality above does not qualify that PR's reset/failure-isolation tests.

For #220, [read-only evidence](windows-native-20261027/descale-replay.json)
at `94ce0c97d0041d5dc36276259da97beafaaa96a2` is
**BLOCKED_GOLDEN_CORPUS_UNREACHABLE**, including outside-sandbox reachability
verification. Metadata/source preparation and 9 harness regressions pass; the
prepared-source SHA-256 is
`241d2dd1591f63793e74428e40e4e4448b52af289a61f47d69f976dae743a8dd`.
Thirty metadata bindings are not thirty authenticated original files: private
authentication did not complete and zero of six original numeric comparisons
ran. No replacement data, new golden export or corpus write was used. Archived
Windows compiler/ISA/buffer signoff remains separate. The isolated probe's
SSE2 arithmetic and 32-byte allocations/guard regions are source facts only.

PR #77 is qualified separately on branch
`codex/windows-simd-integration-20261027`, with a separate report and review
proposal. No production ISA activation or performance claim follows from this
test/evidence change. Frozen `upstream/`, the dirty primary checkout, private
corpora and the owner PR branches remain unchanged.

## Reproduction and publication

Configure Visual Studio 2022 x64 Release in three separate build directories:
standalone ON/upstream OFF with CUDA OFF and ON (architecture 89), and standalone
OFF/upstream ON/CUDA OFF for the SDK suite. Enable tests and required external
decode in standalone, passing locally installed FFmpeg/ffprobe paths privately.
Use a short local TEMP/TMP for nested MSVC configure probes. For each build run
`cmake --build <build> --config Release`, then
`ctest --test-dir <build> -C Release --output-on-failure --output-junit <local.xml>`.
Do not overlap the large geometry suites when measuring their timeout contract.

The native process runner command and fixed matrix are documented in
[`WINDOWS_REFERENCE_PROCESS_PROTOCOL.md`](WINDOWS_REFERENCE_PROCESS_PROTOCOL.md).
Use `HYPERCINE_GOLDEN_DIR` only as a private local binding for the Descale replay.
The complete installed-hook regression matrix passed 102/102 tests in 626.65 s
(SHA-1/SHA-256); the separate real-hook commit/push and missing-identity smoke
also passed 2/2. The full run had completed before a stop attempt; its initially
reported NOT_COMPLETED status is corrected from the final log and exit 0.
Publication requires installed hooks, staged/outgoing-commit privacy scans,
reviewed sanitized reports and explicit public Git identities. No automatic
owner-branch/main merge, release, tag or history rewrite is performed.
