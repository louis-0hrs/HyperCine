<!-- Copyright (c) 2026 HyperCine Contributors. Dual-licensed under the MIT License or Apache License, Version 2.0. See LICENSE-MIT and LICENSE-APACHE for details. -->

# Issue #16: isolated integration candidate for the remaining #52 lane

Date: 2026-10-19
Issues: #16 (decoder mainline integration), #13 (safety qualification), #52 (Linux A2 sanitizer parent)

Scope: one reviewable candidate that combines the current `decode-phase3b2a`
parent with the still-draft PR #52 branch and the fail-closed CUDA submission
fix requested by the #16 handoff. Nothing is promoted to `main`, no tag or
release is created, and `upstream/` is untouched.

## 1. Exact provenance

| Item | SHA |
| --- | --- |
| Parent / base of this branch (`decode-phase3b2a`, = PR #12 head) | `aec37c1f990a4b3dda3e3103154ec2b4e3bdfa2b` |
| Fail-closed submission fix (new commit) | `13d6331` |
| Merged PR #52 head (`codex/linux-decoder-sanitizer-p0`) | `1c34eabeda584ac88ab0624d60d3828600bbf080` |
| **Candidate SHA under test** | **`0a67e673f87269dea88fe176e0e9de9bf7159edc`** |

Branch: `integration/issue16-pr52-failclosed-20261018`
Merge base of parent and #52: `1d78fc183ea28c4268345521e83fb2bca3226381`
(the parent advanced by eight commits through merged PR #56 and PR #90).

Delta from the parent is 32 files: PR #52's 31 files plus
`src/cuda/decode_phase2e_pipeline.cu`. There is no `upstream/`, encoder, or
production-API change; the only production source change is the fail-closed
submission handling described below.

## 2. Fail-closed CUDA submission audit

The #16 handoff asked for an audit of the unchecked CUDA event and async-copy
calls in `src/cuda/decode_phase2e_pipeline.cu::CheckAdvanceInverseDag()`.
The audit confirmed the concern: all four DAG stages discarded the results of
`cudaStreamWaitEvent` and `cudaEventRecord`, and the color stage discarded the
`cudaMemcpyAsync(host_frame_error, ...)` result and the `ev_ready` record
result. A failed submission still set the stage flags and could leave
`ev_ready` showing a previous frame's completed record on the reused slot
event. `WaitForDecode`/`PollDecode` could then report `Complete` from stale
state.

The candidate changes only the failure behavior:

- `DecodeSlot2E` keeps an atomic `first_cuda_error` per acquired frame, reset
  on slot acquisition. `NoteCudaFailure()` preserves the first error and marks
  the frame failed.
- Every DAG stream wait, event record, kernel-launch result
  (`cudaGetLastError`) and the color launch is checked. A failure stops DAG
  advancement immediately, so later stages are not enqueued.
- `ev_ready` is recorded only after successful submission. When any submission
  fails, the slot is abandoned and the queue call returns ticket `0` instead
  of a publishable ticket.
- `WaitForDecode` and `PollDecode` report `Failed` whenever a submission error
  is stored, so a stale completed event cannot be interpreted as success.
- The blocking pipeline modes and the progressive FIELDPLUS path check their
  ready publication and color/error-copy submissions as well.

The success path is unchanged: no extra device work, no API change, and no
change to the event/stream topology (26 streams / 394 events for two slots).

## 3. Qualification on the candidate SHA

Environment: Windows 11 Home 25H2 build 26200 (WDDM), NVIDIA GeForce RTX 4090
Laptop GPU 16,376 MiB, driver 610.62, CUDA Toolkit 12.8.61, MSVC
19.44.35229.0, CMake 3.31.6.

| Check | Configuration | Result |
| --- | --- | --- |
| Native CUDA build | VS 17 2022 x64, Release, `HYPERCINE_ENABLE_CUDA=ON`, `CMAKE_CUDA_ARCHITECTURES=89` | Build succeeded, no warnings in the changed file |
| Full CUDA CTest | same build, `ctest -C Release --output-on-failure` | **44/44 passed**, 0 skipped, 797.18 s |
| CPU-only build and tests | VS 17 2022 x64, Release, `HYPERCINE_ENABLE_CUDA=OFF` | **7/7 passed**, 5.08 s |
| Compute Sanitizer memcheck | production reconstruction default mode, `--leak-check full` | 0 errors, 0 bytes leaked |
| Compute Sanitizer initcheck | mixed acceptance matrix `--sanitizer` | see section 3.1 |
| Host AddressSanitizer | not rerun on this candidate | not claimed |

The native run is the first full-suite result on the **combined** parent plus
PR #52 tree. It is a Windows result and does not replace the Linux A2
qualification.

### 3.1 Compute Sanitizer initcheck

`compute-sanitizer --tool initcheck --error-exitcode 99` ran the mixed
acceptance profile (`test_decode_phase3b1_acceptance --sanitizer`) on the
candidate and reported **0 errors**. All cases passed: `frozen64`,
`frozen1080`, `field_luma_sanitizer`, `field_chroma_sanitizer`,
`noise_sanitizer`; the steady-state allocation delta was zero for C++
allocations, CUDA calls, device, pinned and pageable memory. The matching
memcheck run of the production reconstruction test reported 0 errors and
0 bytes leaked.

## 4. Parent/child evidence boundary

- PR #52's historical child evidence is a Linux A2 **43/43** CTest pass at
  `1c34eab`, with the 12/12 GPU sanitizer evidence from its parent `a6f050a`.
  That evidence belongs to the child head alone.
- The parent branch advanced afterwards through merged PR #56 (`c18fe4d6`) and
  PR #90 (`aec37c1f`), so the historical child pass is not proof of the
  combined tree.
- This candidate is the combined tree plus the fail-closed fix, qualified on
  native Windows CUDA at the exact SHA `0a67e67`. It makes no Linux A2 claim.

## 5. Remaining gates before a new integration decision

- One uncontended full A2 test/sanitizer acceptance of exactly `0a67e67` by
  the Linux owner. Do not start that window while the A2 is allocated to other
  workloads.
- External real GOP-2 1080p/4K golden parity remains open. It is a reference
  limit and is not closed by this candidate, by the Windows CTest pass, or by
  the Linux child evidence.
- Host ASan/sanitizer coverage of the changed submission-failure paths, if
  #13 requires it beyond the existing `af9550f` matrix at the parent.
- The maintainer promotion decision for #2/#12/#16. This candidate creates no
  `main` merge, tag, or release.

## 6. Reproduction

```
cmake -B build-candidate -S . -G "Visual Studio 17 2022" -A x64 \
  -DHYPERCINE_ENABLE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89 \
  -DHYPERCINE_BUILD_TESTS=ON -DHYPERCINE_BUILD_BENCHMARKS=ON
cmake --build build-candidate --config Release --parallel
ctest --test-dir build-candidate -C Release --output-on-failure
```

Raw Compute Sanitizer logs stay local; only the bounded summary is recorded
here.
