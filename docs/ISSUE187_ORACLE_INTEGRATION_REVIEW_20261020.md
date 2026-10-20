# PR #187: oracle disposition and proposed test integration

Copyright (c) 2026 HyperCine Contributors.
Dual-licensed under MIT OR Apache-2.0; see LICENSE-MIT and LICENSE-APACHE.

## Decision

**PHASE_LANDING_HOLD / ATTRIBUTION_COMPLETE / PIXEL_ORACLE_UNQUALIFIED.**

#205 and diagnostic #206 `5ac10a55a48bf06fa4ad78adb6da3f5244ec4076`
attribute the captured 1080p failure to original GoPro SDK nondeterminism:
900396-byte packets differ only at frame-number header byte 71; the same
immutable host SHA yields divergent BGRA across fresh processes (5/14),
and the device packet 1/14. This is a bounded attribution, not a general
pixel qualification or proof of all CUDA output. The historical full native
CUDA result at #187 `46fc642310de8a905245696f0a3f2112817de8d5` remains
44/45, zero skips. Its 19-case failclosed, host-ASan, four sanitizer tools,
and two-packet 404 original-stage comparisons retain their recorded scope.

#211 first revision `de3617e8b6cf68979038c2491ed99be22a2c5763` supplies
the five-case native diagnostic smoke in the #210 lineage, not at #207's
exact SHA. Its follow-up `a66e694` closes an additional inventory gap: all
CTest processes passing cannot qualify an independent pixel oracle.
Schema 2 distinguishes reference-lane execution from release qualification;
no independent adapter is implemented, so release remains BLOCKED=77.
Strict failures retain exit 1 and take precedence. Current follow-up native
CPU suite 16/16 and eight inventory regressions pass on that separate stack.

## Verified ancestry and selected proposal

#187 targets `decode-phase3b2a` at
`aec37c1f990a4b3dda3e3103154ec2b4e3bdfa2b`.
#210 targets the separate main-based diagnostic lineage. Local Git merge-base
of #187 and #210 is `51435890ba5853e87e8cf1ddc7cc747954bd9e80`;
#206 through #211 are not #187 ancestors. A whole branch merge would carry
unrelated ancestry and is not proposed.

If the owner elects a test-only phase transplant, review the following
ordered source commits on an isolated candidate based exactly on #187:

1. #206 `5ac10a55a48bf06fa4ad78adb6da3f5244ec4076`: Phase2a diagnostics.
2. #207 `ddcdb58e96599e786048d334b86047d124b14734` and
   `2f97480e65f448def6f97ed9ce04c5299f5407e3`: bounds and capture guards.
3. #208 `3819700fb1d0bb9e3743df176e6ecd5e49276c39`: reference contract.
4. #209 `3af5d4dd1f3ca24fb565c3a4ff3c00fb25167fa2`: Phase2b/c policy.
5. #210 `1563608c7fa90db7f0172266f7d511edc9ce979f`: Phase2d policy.
6. #211 `de3617e8b6cf68979038c2491ed99be22a2c5763` then `a66e694`:
   smoke harness, inventory, positive independent-oracle requirement.

This is a proposed source selection, not an executed cherry-pick or a claim
that it applies cleanly. Review conflicts specifically in tests/CMakeLists.txt
and tests/test_phase2a/b/c/d.cpp. Preserve every phase decoder registration,
failclosed case, strict coefficient/entropy/metadata/geometry/resource gate,
and existing quality thresholds. Only reference instability or missing
qualified pixel evidence may produce 77. Never replace whole CMake/test files
from the main-based stack or count 77 as a quality PASS.

## Resulting-head checks and remaining dependency

After source integration review, verify zero production/public-header/upstream
delta, build identities and CTest inventory; run full native CPU CTest, the
five-case guard smoke on the resulting executable, bounded reference lane
and explicit release target. A full resulting-head CUDA run belongs to the
approved integration candidate, not another retry at unchanged #187.
Record strict FAIL, SKIP and absent coverage separately. Preserve the M4
8K capacity failure as a strict resource issue. Reuse historical unchanged
CUDA implementation sanitizer/performance evidence only within its scope;
changed lifecycle/kernel code triggers fresh relevant checks.

**Next action:** phase owner reviews this source selection and resulting-head
plan after #211 review. Independent same-packet pixel qualification still
requires a reviewed implementation/version, matrix/range/depth normalization,
repeatability and justified thresholds. Until supplied, do not approve pixel
release or phase landing. No protected phase/main merge, release or corpus
rewrite follows from this report.

## Worktree boundary

Pre-existing uncommitted Phase2a diagnostics and decision-report edits in the
#187 worktree were left untouched and excluded from this documentation-only
commit. They have not been tested or endorsed by this follow-up. No new GPU
run is claimed; documentation does not change the validated decoder source.

Validation for this documentation follow-up: existing native Windows CPU-only
Release CTest **7/7 PASS, zero skips, 4.71 s**. These existing binaries test
the committed decoder implementation, not the unrelated uncommitted diagnostics.
