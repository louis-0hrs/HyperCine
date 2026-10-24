<!-- Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under the MIT License or Apache License, Version 2.0.
See LICENSE-MIT and LICENSE-APACHE for details. -->

# Issue #16: scoped reference acceptance and phase integration proposal

Review date: 2026-10-20. Reviewer: Codex, acting on the repository owner's
request to work on issue #16 / PR #187. This is an attributable source-review
decision and concrete integration proposal, not maintainer merge authorization
or a cryptographic/human signature on a qualification exception.

**Reference disposition: ACCEPT the bounded original-SDK observer contract at
#190 `77acadbd497a490c034440709390807087147d64`. Recommend phase-only acceptance
of #187, conditional on resolving the current full-suite failure described
below, explicit maintainer merge authorization and resulting-head checks.**
The targeted follow-up below closes the previously
proposed DAG-copy coverage exception with a real Phase2FA-path test; no waiver
is now requested for that site. Keep #187/#190 Draft and #2/#13/#16 open pending
that decision. No phase/main merge, promotion, tag or release is performed.

This supersedes the previous review's private-byte/provenance HOLD: #200's
Windows archived readback and its separate #199 reconciliation are now in
#190. Neither a missing-stage export nor another unchanged A2 run is required.

## Exact inputs and evidence boundary

| Input | Revision |
| --- | --- |
| Reviewed decoder code | `7ed4a82ed97598454c2680bd264dedf259e6e7dc` |
| Reviewed #187 tip, before this documentation addition | `f6c04f2dea65ed412994a8a873b459291baae57e` |
| #187 target, `decode-phase3b2a` / #12 | `aec37c1f990a4b3dda3e3103154ec2b4e3bdfa2b` |
| Integrated #190 reference stack | `77acadbd497a490c034440709390807087147d64` |
| #196 representation verifier / historical #200 readback source | `4a9f3f116f1d9202c63b997a971dc5b7199d0e7c` |
| #199 compact row-length verifier | `9c37423eb459fb61ddde198e955577e8cfb53d32` |
| Historical #200 report | `8988b49109bced51f9e314894881112c7e435e35` |
| Main snapshot | `a987f679f7e17a88aebe60375c37c843d7095152` |
| Original SDK / frozen upstream tree | `11574d0295771edccadd17af14af74a539f924c7` / `837528ef9e9a55c416e9592ecb54e1c8f1ff9cf8` |

The #187 base delta contains 41 files, including #52's Linux compatibility
and test work. Its `upstream/`, encoder implementation and public
`include/hypercine/decoder.h` deltas are empty. The `7ed4a82..f6c04f2`
delta contains only the Windows evidence JSON and integration report.
The original review changed documentation only; the subsequent targeted
follow-up adds the single negative case described below, without codec changes.

## Five #187 review dispositions

Source checked: `NoteCudaFailure`, submission wrappers,
`CheckAdvanceInverseDag`, `AbandonSlot`, acquisition/publication,
`InjectTestCudaFailure`, `PollDecode`, `WaitForDecode`, `ReleaseOutput`,
and `test_decode_phase3b2a_failclosed.cpp`, at the decoder SHA above.
The [independent review](https://github.com/louis-0hrs/HyperCine/pull/187#issuecomment-6264359829)
is consistent with that source.

| Observation | Disposition for the proposed phase landing | Rationale / excluded claim |
| --- | --- | --- |
| `DagFrameErrorCopy` negative case absent at `7ed4a82` | CLOSED_BY_TARGETED_TEST in this follow-up | The added Phase2FA case reaches the copy failpoint exactly once with production guard disabled, sees no ready-event submission, preserves the direct error, returns ticket zero and reproduces the baseline image after same-slot recovery. The original 18 cases are preserved, now 19. The earlier `PHASE_ONLY_EXCEPTION_PROPOSED` is superseded; no coverage waiver is requested. |
| Synthetic failed-ticket async lifetime | ACCEPT_AS_TEST_ONLY_LIMIT | `InjectTestCudaFailure` records an error without draining work; `ReleaseOutput` synchronizes only Pending. Identical-input sequential recovery is not proof of safe different-input or concurrent staging-buffer reuse. Do not extend seam use without an explicit drain/lifecycle fix. Actual queue-time checked submission failure calls `AbandonSlot`, which attempts synchronization of all owned slot streams before reuse; this does not certify arbitrary driver/context failure recovery. |
| Global test error accessor | ACCEPT_AS_TEST_ONLY_LIMIT | Atomic storage prevents a plain data race, but concurrent queues can overwrite attribution and early invalid input can retain an old value. Only the single-threaded valid-queue assertions are accepted. Production first-error state is per slot. |
| Empty human diagnostic on some DAG failures | ACCEPT_NONBLOCKING_DIAGNOSTIC_LIMIT | This is a production observability limitation, not literally test-only. Stored first error still prevents successful ticket publication. Complete human-readable CUDA failure diagnostics are not claimed. |
| Poll Complete fast path after late injection | ACCEPT_AS_TEST_ONLY_LIMIT | Poll can remain Complete while Wait rejects a newly injected error after completion. The production submission-error path abandons before publication. This does not establish production success for that path; post-completion injection cannot support Poll/Wait equivalence claims. |

No source defect established by these observations requires changing decode
math for this proposal. The targeted Phase2FA negative test resolves the
unratified coverage-exception proposal. The phase landing remains HOLD for
explicit maintainer authorization. The other four limited dispositions remain
in force; this test does not establish arbitrary submission/driver-failure safety.

## Bounded original-reference decision

Accept the alternate original signed-V210 observer source contract and the
archived provenance evidence for exactly these packets, both frame indices:

| Geometry | Packet bytes | Packet SHA-256 |
| --- | --- | --- |
| 1920x1080 | 897552 | `fab22de68bc73eaf99d204a5a569e21540dc346d2e72d1c76de9afe401b858f2` |
| 3840x2160 | 2930956 | `1ab74a2f6f72009d0216763bd4d2d820a8491d84b198064a1bf58ee1522f3804` |

The original source is deterministic asymmetric **YUY2 8-bit input**, encoded
by the frozen SDK as progressive YUV422 FilmScan3 **10-bit-coded FIELDPLUS/GOP2**.
It is not a high-bit-depth-input qualification.

Source inspection of `prepare_gop2_stage_sdk.py`, `gop2_stage_extract.cpp`,
`gop2_stage_compare.cpp` and `gop2_stage_contract.py` confirms:

- Entropy observation is after successful `DecodeSampleSubband`, before inverse
  submission: 51 unique original bands per geometry. The two candidate frame
  requests materialize 42 bands each; their union covers all 51 originals.
- Signed planes come from the frozen original `TransformInverseSpatialToV210`
  strips before packing, preserving native signed16 saturation. They are not
  unsigned YU64 reinterpretations or reconstructed candidate expectations.
  Original alternate packed output must equal original default packed output.
- Original one-worker controls, 18 alternate/default checks, 12 archived frame
  checks, 198 resident checks and nine observations per band/row are required
  per geometry. This is bounded evidence, not a theorem about other paths.
- The candidate downloads signed device planes directly, with original Y/V/U
  mapped to candidate Y/U/V as 0/2/1, without clipping/scaling/repacking.
- #196 binds signed representation and capture boundaries; #199 binds each
  original and both comparison lengths to integer compact `pitch * height`.
  Equality/coverage checks and file hashing remain necessary alongside this.

Accept the [historical Windows readback](https://github.com/louis-0hrs/HyperCine/blob/77acadbd497a490c034440709390807087147d64/docs/validation/GOP2_ISSUE189_WINDOWS_READONLY_REVIEW.md)
at `4a9f3f1` and the [separate reconciliation](https://github.com/louis-0hrs/HyperCine/blob/77acadbd497a490c034440709390807087147d64/docs/validation/GOP2_REFERENCE_STACK_WINDOWS_RECONCILIATION.md)
at `9c37423`: 2,144 files / 1,377,743,781 bytes, unchanged snapshot inventory;
164 Phase-A original artifacts and 114 Phase-B original stages; 224 + 180
exact comparison records, with all 180 Phase-B pairs also directly byte-equal.
Private/public manifest identities, archived tool hashes and observer-source
reproduction belong to that recorded Windows review. This review relies on
those pinned records; it does not claim another private-corpus read or producer
runtime replay. The historical #200 execution did not include #199.

The 404 records cover two packets, not 404 independent samples. Acceptance
excludes other qualities/formats, high-bit-depth sources, interlaced FIELDPLUS,
arbitrary clipping-edge values and default multithread original SDK safety.
No bit-exact requirement or PSNR threshold is relaxed. Original-SDK #160/#195
silent-drop/race risk and `temporal.c:9606` ASan finding remain separate.

## Gate accounting

| Gate | Review result and remaining condition |
| --- | --- |
| #13 own CUDA safety | Scoped ACCEPT at `7ed4a82`: reported RTX4090 45/45, CPU 7/7, A2 44/44 zero skips + 12/12 real sanitizer cells, M4 scoped 3/3; own exercised host-ASan paths clean. Synthetic injection is not arbitrary physical-driver-failure proof. Retain original-SDK ASan finding separately. Formal issue closure remains with maintainer. |
| #2 external oracle | Bounded source/reference contract ACCEPT at integrated #190 head. No missing named stages or remaining #199/#196 integration task. Broader production promotion remains a separate decision. |
| #14 performance/resources | Retain existing closed, scoped qualification and merged #56 evidence. The test/documentation follow-up does not alter inverse math, dispatch, allocations or stream/event topology. Preserve lazy first-use versus warmed allocation distinction. No new throughput/latency/peak-memory claim; no optimization milestone declared. |
| #16 phase integration | Targeted test resolves the coverage exception. HOLD for the current full-suite `test_phase2a` comparison failure, explicit phase merge authorization and resulting-head checks. Three isolated passes do not turn the full run green. |
| Protected main / release | Separate HOLD; acceptance above does not authorize either. |

## Exact phase landing and resulting-head validation proposal

Ancestry checks succeeded for every adjacent pair:

`3AF 9eb9e3d -> 3B0 7296f6f -> 3B1 f1d0fe4 -> 3B2R f433a74 -> 3B2A aec37c1 -> #187 f6c04f2`.

1. Recheck live #187/#190 heads and bases before an authorized landing. Scope
   approval to the reviewed decoder code plus this isolated test follow-up and
   documentation-only descendants; other source drift requires fresh review.
   Confirm #56/#90 already in `aec37c1`
   and #52 already in #187; do not merge them again.
2. After explicit maintainer decision, merge #187 into **`decode-phase3b2a`**,
   preserving ancestry with a merge commit. Record both parents, resulting
   commit/tree, source/test diff and unchanged upstream tree. Do not retarget
   #187 to main. #190 remains a separate reference/test PR against main.
3. On the resulting phase checkout, configure/build and run full Release
   `ctest --test-dir build -C Release --output-on-failure` for the applicable
   CUDA configuration, report registered count/pass/skip explicitly (current
   Windows baseline 45), and run the complete CPU-only configuration (current
   baseline 7). This is a future resulting-head integration check, not a
   request to repeat A2 qualification at unchanged `7ed4a82` now. Record exact
   configuration, commands and source SHA; do not silently accept lost tests.
4. If source/test trees match the reviewed candidate, retain the existing
   SHA-scoped A2 sanitizer evidence. If conflict resolution changes CUDA,
   lifetime, decoder/test registration or resource behavior, review that delta
   and rerun affected native/sanitizer checks. Performance-affecting changes
   require `benchmark_codec` against `reference_windows_msvc.json`, with FPS,
   Mpix/s, average/min/max latency and peak memory plus #14 regression checks.
5. Only after separate #2/#13/#16 mainline authorization, land milestones in
   order: 3AF to main, then retarget each next child to main as its parent
   lands, through #12 (3B2A). #12's current base must remain 3B2R, not 3B2A.
   Use merge ancestry; do not squash the dependent stack. The prior main/3AF
   preview reported conflicts in `benchmarks/CMakeLists.txt`,
   `benchmarks/benchmark_codec.cpp`, `docs/UPSTREAM.md`, `src/CMakeLists.txt`;
   recompute against then-current main and preserve its newer contracts.
   Validate each actual resulting tree before commit/merge. Keep milestone
   branches until descendants and provenance are safe. No release is implied.

## Validation in the original documentation-only review (`ee0ef05`)

- Complete PR-branch CPU Release CTest: **7/7 PASS**, zero skips, 4.57 s.
- Integrated #190 optimized Python stage contract: **17/17 PASS**; Phase-A
  reference integrity: **20/20 PASS**. These validate the current public
  contracts and rejection cases, not another private-byte readback.
- All five adjacent ancestry edges verified; #187 base/code/evidence deltas
  and integrated #190 source identities inspected.
- Root and candidate `git status --porcelain upstream/` empty. Existing root
  worktree edits preserved. No codec, test, upstream or corpus file changed.

Existing GPU/sanitizer/reference-run numbers above are explicitly attributed
to their historical evidence. No new GPU, sanitizer, benchmark or export run
was performed for the original documentation-only review at `ee0ef05`.

## Targeted Phase2FA follow-up after independent next-step review

The [new review request](https://github.com/louis-0hrs/HyperCine/pull/187#issuecomment-6287302822)
was addressed by adding `CheckGpuIndexFrameErrorCopy` to the existing failclosed
test. Production codec/header/build files are unchanged. All original 18
negative cases remain; the added case raises the total to 19, not the number
of CTest targets.

The test initializes one slot in `Phase2FAGpuIndexBatch`, decodes and releases
a baseline frame, then installs a hook for `DagFrameErrorCopy`. The failing
queue disables the production guard so that `GpuIndex()` is the actual gate
to the targeted branch. It requires exactly one copy-site hit and zero
`DagReadyEvent` hits. It checks ticket zero, the exact `cudaErrorInvalidValue`,
Invalid Poll and unsuccessful Wait despite a previously completed ready event.
After removing the hook, the same pipeline's only slot must decode/release
successfully and reproduce the baseline BGRA hash.

The hook models a direct CUDA API error before that copy is submitted. It does
not deliberately break a physical driver, inject after publication or rely on
the synthetic failed-ticket release seam discussed above. Normal abandonment
and same-slot recovery execute through production queue handling.

Test identity: parent `ee0ef05151f7effc1f3c5f73bd2ac3ef7390ac21` plus Git blob
`2ec81a715a3d224261654a21ee410cb57b4ca96e` for
`tests/test_decode_phase3b2a_failclosed.cpp`. Decoder implementation remains
`7ed4a82ed97598454c2680bd264dedf259e6e7dc`; the new test is not claimed to have
run in the historical Linux A2 qualification.

Native Windows RTX4090 Laptop, CUDA 12.8.61, MSVC 19.44.35229, Release:

- Targeted native CTest: 1/1 PASS, zero skips, all 19 cases exercised; output
  explicitly reports `copy hits=1, ready hits=0` and same-slot hash parity.
- Targeted MSVC host-ASan CTest: 1/1 PASS, zero reports, all 19 cases.
- Compute Sanitizer: four actual runs of the updated failclosed executable,
  `memcheck`, `initcheck`, `racecheck`, `synccheck`, each with
  `--error-exitcode 99`, exit zero and parsed zero-error/hazard summaries.
  This is four tools on one target, not a replacement 12-cell A2 matrix.
- Full CPU-only Release CTest: 7/7 PASS, zero skips, 4.61 s.
- Full CUDA Release CTest: **44/45 PASS, one FAIL, zero skips**, 753.65 s,
  exit 1. All decoder targets, including the extended failclosed test and
  Phase2FA corpus (461.23 s), pass. See the unresolved encoder-test result below.

Native test executable SHA-256:
`20c7058661f81e85a1b94b94f276dec2e5250420314996f8dab030a05a2622e8`.
Host-ASan executable SHA-256:
`60403b05a24d4da59b3ec47859eb1077480fc8f5f1e64f58a4c0058eb6e97f22`.
Compute Sanitizer version 2025.1.0.0, NVIDIA driver 610.62, sm_89.

The first sandbox build was interrupted before compilation; a sandbox
Compute Sanitizer attempt was interrupted before target output/summary.
Neither counts as a successful instrumented run. Successful native builds
and all four reported sanitizer runs used the permitted native execution path.
No benchmark, reference export or original-observer replay is claimed. The
native suites do invoke the original SDK as part of their existing tests;
those calls are not another execution of the pinned reference producer.

### Unresolved full-suite encoder comparison failure

`test_phase2a` failed in the full run with:

```text
device-resident decode differs from host compact (1 bitstream bytes)
```

Its preceding host/device encoder checks reported success and zero coefficient
or expansion mismatches. This assertion compares outputs of two fresh original
SDK BGRA decoders; the one differing bitstream byte is reported by the test,
not proof of which component caused the decoded-image difference.

The same executable then passed **3/3 isolated CTest invocations**, with no
source, executable, input or test-criterion change (1.06 / 0.70 / 0.60 s).
Its binary timestamp is 2026-10-19, before this follow-up, and its SHA-256 is
`d0b0b654130c54da56986fed57f3b54cf71387efbeba1ae4a7b2fd0aef31fe2a`.
`tests/test_phase2a.cpp`, all codec sources, headers and build registrations
have empty diffs against parent `ee0ef05`.

This establishes an intermittent result in an unchanged, separate executable;
it does not establish a root cause or authorize dismissing the failed full
run. Known original-SDK threading/dither caveats in `docs/UPSTREAM.md` are
context, not a demonstrated diagnosis of this occurrence. Do not report 45/45,
weaken equality, count retries as a clean full pass, or clear the integration
gate from those retries. Keep this Draft test-only follow-up reviewable while
the independent encoder/oracle comparison is investigated. The narrow DAG-copy
test request is fulfilled; resulting-head/native integration acceptance remains
HOLD. No unrelated encoder or frozen SDK fix is included here.
