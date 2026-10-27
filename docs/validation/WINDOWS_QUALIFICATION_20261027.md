<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Independent Windows qualification — 2026-10-27

Branch: `codex/windows-qualification-20261027`. Tested main baseline:
`44827e84f1658d67a506ecdd41d8d1c7ba7379c8`. The delivered commit is identified
in the Draft PR. This change adds reference-process tests and evidence, not an
encoder/decoder implementation or production acceptance.

## W0: baseline and actual platform

`origin/main` was fetched before qualification. PR #266 remains open/Draft at
`d67bb90338c9130620f887656392c7061d075928`; it is not part of this baseline.
PR #268 is already integrated as `7288e75`; its PASS/FAIL/BLOCKED/INCONCLUSIVE
reporting and strict reference gates are retained. Existing local Codex branches
and worktrees were inventoried before creating this isolated checkout. Neither
the dirty primary checkout nor existing owner branches were edited.
The [open PR inventory](windows-qualification-20261027/open-pr-inventory.json)
records the observed heads, including #270, #274, #77 and #142; it is a snapshot.

Actual host: Windows x64, AMD Ryzen 9 7940HS, 8 physical cores / 16 logical
processors; NVIDIA GeForce RTX 4090 Laptop GPU, compute capability 8.9,
16,376 MiB reported memory, driver 610.62. MSVC 19.44.35229.0 / Visual Studio
2022; Windows SDK 10.0.26100.0; CMake 3.31.6; CUDA Toolkit 12.8.61, explicitly
compiled for SM89. Native FFmpeg build `n9.0.1-11-ge47273f4d9-20260831`.
Only Release was qualified in this round. No A2/M4/Mac result is presented as
a measurement of this host. SDK reference build uses the frozen source baseline
`11574d0295771edccadd17af14af74a539f924c7`, default Release DLL CRT and OpenMP.
The linked SDK version query returns packed `655616` (10.1.0).

## W1: immutable sample, fresh processes, honest reference status

The existing fixture `tests/fixtures/macos-filmscan3/gradient.cfhd` has SHA-256
`5e8ea4785c00dd6406e85be7ddee426dd093e1007aca7effa9e1aff4f95aa1ca`.
It was neither generated nor modified. It is an autonomous Mac-produced sample,
not an original-GoPro-encoder golden. The unmodified original SDK decodes it.

The [protocol](WINDOWS_REFERENCE_PROCESS_PROTOCOL.md) fixes three processes per
cell before decoding: requests 1/2/4/8 at affinity `ffff`, then request 2 on one
logical CPU (`1`), two physical cores with one logical CPU each (`5`), and two
SMT siblings on one core (`3`). Exactly 21 fresh processes, full-resolution YU64,
pitch 7680, image-only hash domain 8,294,400 bytes. Every observation carries
input/executable/output SHA-256, SDK version, CPU/OS ISA availability, verified
affinity, requested threads, exit/error/timeout status. Actual decoder worker
counts and selected internal ISA are **unobserved**, not inferred from metadata
or CPUID. Worker-count causality is therefore INCONCLUSIVE.

The [final run](windows-qualification-20261027/reference-process.json) returns
**BLOCKED_NONDETERMINISTIC_REFERENCE**: all 21 API/decode/extent checks succeeded,
but two distinct output hashes occurred within identically configured cells.
The executable digest in that report identifies the final output-safe build.
An [earlier bounded run](windows-qualification-20261027/reference-process-initial.json)
of the pre-output-guard build produced six hashes across 21 processes and was
also BLOCKED. The second run qualified a concrete output-safety change; both
results are retained. No best-of-three selection or retry-until-pass was used.

The probe uses aligned storage, RAII SDK/file handles and CREATE_NEW output
creation. [Four native negative controls](windows-qualification-20261027/native-probe-guards.json)
reject invalid thread request, invalid packet, existing output and input/output
collision; the sample SHA and sentinel bytes remain unchanged. Portable unit
controls reject missing/duplicate observations, inconsistent SDK metadata,
malformed JSON, input-SHA mismatch and timeouts. No instability is reclassified
as an encoder failure, and no pixel threshold is weakened.

Publication/claim ordering, generation, scratch initialization, early free and
post-return overwrite remain separate candidate mechanisms. This experiment
does not instrument them and confirms none as a cause. Existing #159/#195 RCA
evidence is not replaced. Keep coefficient, full packet, bitstream structure,
FFmpeg decodability and original-SDK pixels as five distinct evidence classes.

## W2: actual Windows CPU/CUDA sequence evidence

[Per-frame hashes and sizes](windows-qualification-20261027/cuda-sequences.json)
bind the current-main comparator executable and FFmpeg build. The comparator
checks all 30 pre-quantization bands, quantized hashes, complete in-memory
packets, rate state and frame counters. A separate readback checks the entire
CPU/CUDA packet files, then starts FFmpeg separately for both sides of every
frame. Every decoded output has the exact requested p10 extent and equal SHA.

| Geometry | Different ordered frames | Full packet pairs | Independent decodes |
| --- | ---: | --- | --- |
| 1280x720 | 3 | 3/3 PASS | 6/6 PASS |
| 1920x1080 | 16 | 16/16 PASS | 32/32 PASS |
| 2048x1080 | 3 | 3/3 PASS | 6/6 PASS |
| 3840x2160 | 3 | 3/3 PASS | 6/6 PASS |

These are synthetic matched inputs and own-CPU versus own-CUDA parity, not
original-GoPro-encoder packet parity or a qualified SDK pixel oracle. Current
CUDA frontend remains internal; color conversion, prescale2, quantization and
entropy are host stages. The three registered deliberate stage/kernel/output
negative controls execute; their expected nonzero exits are CTest successes.

The existing reset/concurrent-session/mid-frame-error work is owned by Draft
[#274](https://github.com/louis-0hrs/HyperCine/pull/274). Independent native
review of exact head `c52392e159ea7252d63a63d2f6f47fadd80662fa` configured
SDK-free CUDA-ON/SM89 successfully, then **FAILED** MSVC compilation with C1083
at `tests/test_cuda_multigeometry.cpp:10` (`sys/wait.h`). Its executable did not
run. The maintainer already holds this head for POSIX-only subprocess code and
process-global failure injection that can affect another session. The Windows
evidence confirms that HOLD without rewriting its owner branch. Those reset,
real CUDA-error recovery and failure-isolation gates remain BLOCKED here; Linux
measurements are not substituted. Device-loss injection was not run.

## W3/W4: reference-backed next work, no duplicate engine

The [FilmScan2/RG48 contract](WINDOWS_PROFILE_REFERENCE_PLAN_20261027.md) records
all six existing profiles, the freshly verified FS2 16/120-frame manifests,
12 selected original artifact length/SHA checks and 136 state-row contracts.
FS2 is the next profile candidate: initial limiter 8 and profile-specific
tables/transitions must use the common quality state. Existing SDK-backed
six-profile code must not be reimplemented. No second SDK-free writer, profile
adapter or GPU RGB444 engine is added by this qualification PR. Full original
artifact replay and per-profile independent decode/stability qualification
remain the explicit prerequisites of the separate profile lane.

RG48 is top-down RGB uint16 storage; coded RGB444 p12, source endian handling,
stride/alignment/extent checks and full-width chroma planes are a separate
format contract from YUV422. Existing #142/#167/#156 work and negative tests
are inventoried rather than copied. No general RGB pixel oracle is accepted.

## W5: build matrix and complete test inventories

Machine-readable summaries below contain every registered name and its actual
status, not just a success percentage. CTest exit zero with SKIP77 remains
BLOCKED where prerequisites or reference qualification are absent. The new
portable reference-process contract is registered by the shared SDK-free
function and required by standalone CI; deliberate removal is a negative case.

- [Original SDK, CUDA OFF](windows-qualification-20261027/sdk-cpu.json): 42 PASS,
  zero FAIL, two BLOCKED out of 44 registered; full CTest 336.36 seconds.
- [Quality-only](windows-qualification-20261027/quality-only.json): 24/24 PASS;
  full CTest 211.88 seconds. Includes tests-disabled inventory and incompatible
  quality-mode negative configurations.
- [SDK-free CUDA/SM89](windows-qualification-20261027/standalone-cuda.json):
  77 PASS, zero FAIL, one platform BLOCKED out of 78; 1117.82 seconds.
- [SDK-free CPU](windows-qualification-20261027/standalone-cpu.json):
  72 PASS, zero FAIL, one platform BLOCKED out of 73; 904.71 seconds.
- The last two full-run snapshots predate registration of the new process
  contract. After regeneration, final inventories contain 79 CUDA / 74 CPU
  tests, with no duplicates and exactly that one addition. The new contract
  passes separately in both modes (13 unit cases), and again in SDK mode.
  These supplemental results are kept separate, not represented as a second
  complete 79/74-test run. The stricter standalone required-inventory checks
  accept the final inventories; removal of the new contract is rejected.
- SDK-free standalone with tests OFF builds successfully and registers zero
  tests. SDK-OFF/non-standalone CUDA rejects at configuration, tested by the
  SDK build-contract suite. Tests-on SDK and SDK-off target inventories are
  checked independently; no frozen archive is silently linked into standalone.

Raw CMake, JUnit and build logs stay in ignored build directories. Only
sanitized name/status summaries are published. Sanitizer/device-loss/debug
qualification is not inferred from these Release contract results.

## W6/W7 and integration decision

Current validated x86 production selection remains scalar. Existing AVX2/
AVX-512 Draft #77 conflicts with current main and must be reconciled with the
per-session ISA work (#269); no old kernel or process-global override is merged.
No AVX acceleration milestone is completed by this PR. Existing capability
tests cover unsupported timebases; timecode/GOP/interlaced/metadata/wrapping
remain their existing contracts, not new simultaneous codec features.

The required [reference benchmark comparison](windows-qualification-20261027/performance.json)
records FPS, MPix/s, avg/min/max latency and peak memory. Current 1080p encode
25.4576 FPS versus historical 77.6958, and 4K 6.73864 versus 21.8373, were measured
under concurrent qualification and sustained external GPU/thermal load (GPU
97-98%, observed 82-86 C). This is **INCONCLUSIVE**, not a controlled regression
attribution or optimization claim. Codec/benchmark sources are unchanged.

Integration proposal: review this test/evidence-only Draft separately; review
unmerged #266 independently and keep #274 HOLD until its own exact-head
requirements pass. Preserve
original-SDK pixel qualification as BLOCKED and stage FS2/RG48 work behind the
explicit reference contracts. No automatic merge, release, upstream edit,
reference regeneration or private-media publication was performed.


## PR #276 privacy-contract follow-up

The process runner now exports only fixed native stage, source and header labels,
bounded integer fields, validated affinity masks and a typed inventory allowlist.
Unknown strings, nested values and inventory keys are omitted even when the
subprocess returns syntactically valid JSON with FAIL. Invalid optional topology
remains INCONCLUSIVE. Synthetic absolute-path, hostname and credential-like
canaries exercise the entire exported result; a complete successful synthetic
probe plan still yields REPEATABLE_OBSERVATION_ONLY.

Validation of this follow-up: 16 process contract tests PASS. The existing CUDA
standalone Release build completed all 79 CTest registrations: 78 PASS, one
platform-required SKIP, zero failures (547.39 seconds; per-test timeout 180
seconds). TEMP/TMP pointed to a local build directory for this run; the initial
restricted run encountered temporary-directory access errors and was abandoned.
The final process contracts were rerun after the last validation tightening.
These checks do not requalify the integrated main branch or replace the original
baseline 44827e8 measurements, original-SDK BLOCKED conclusions or benchmark
INCONCLUSIVE status above. No new pixel oracle or RCA claim is made.
