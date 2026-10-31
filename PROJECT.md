# Current issue #331 review continuation (2026-10-31)

Status: **REVIEW / Draft retained; no merge or self-approval**. Implementation
and independently qualified source head: `d34eefd025cb6f423c672b540e64a8066aa5103e`.
This current-status documentation commit is its direct docs-only successor;
`git rev-parse HEAD` identifies the checkout documentation head without a
self-referential hash. The PR body/publication receipt records the exact head.

Base is `main` (`04c348f848de1eb3a47d3b90fe191722c1ab559e`), already forward
merged at d34eefd. #325/#329 are integrated ancestry, not a pending stack.
Implementation-head hosted snapshot: **28/28 SUCCESS**, including Linux/macOS
sanitizer jobs. These are not new documentation-head results; fresh hosted
checks remain a separate publication gate, reported on the PR.

- [x] Audited equal replay lengths, 27 unique high-pass pairs, coordinates and
  integer types, contiguous frame order, previous-byte chain, pinned index SHA,
  1920x1080 geometry and exact four-profile session count. No demonstrated
  remaining defect in requested scope; no implementation/test edits.
- [x] Independent fresh-build qualification at d34eefd:
  [source-bound report](https://github.com/louis-0hrs/HyperCine/pull/331#issuecomment-6543131547),
  42/42 quality-only CTests, 606/606 indexed sizes/SHA-256 and 480/480 packet
  tables; 18/18 adversarial methods also under Python -O. Pre-fix calculator
  rejected: 84/480 divergences (Medium 42, High 42). Historical negative
  evidence remains negative, not a passing historical implementation.
- [x] Documentation continuation reruns the full quality-only suite with real
  pinned Group A and Python -O before commit; counts and publication head are
  recorded in the PR handoff. Implementation/test blobs remain equal to the
  independent d34eefd measurement.

Index SHA-256:
`62c6f4c9297d760f1f14bde73ad15f62ca254371fa8b05f55c7ddf68e1d2e0bc`.
Manifest SHA-256:
`3df4950c649f56457298f7d5ad33dd648ccfeec2101534ed2450f6a4cb5f29aa`.
Frame-zero previous bytes are null (absence), normalized to probe zero; optional
lowpass tags may be absent. Neither is a measured SDK numeric zero.

Scope: observable packet tables only, complementary to #322 byte/direct state
and parent-owned #320. SDK counters remain **NOT_OBSERVABLE**. Linux replay of
original Windows artifacts is **not native Windows execution**. Native Windows
and maintainer evidence acceptance remain outstanding. No original-state,
pixel, GPU, FS2/FS3, conformance, performance or release claim. Raw evidence
stays outside Git; upstream and private corpus remain unchanged.

Next action: maintainer accepts independent evidence and completed exact new-head
hosted checks, coordinates native Windows if required, then decides integration
with #322/#320. Keep #46 open and PR Draft pending that decision.
The sections below preserve historical snapshots; their then-pending publication
and CI statements are not current status.

# Historical issue #321: independent Windows corpus verification

Status: REVIEW deliverable prepared; branch `expert/issue321-windows-corpus-20261029`, base `9909bfc`. Publication receipt is recorded on issue #321.

Scope: read-only verification; no SDK exports, upstream changes or corpus writes.

- [x] Read repository guidance and current issue coordination; claim task.
- [x] Implement independent fail-closed transfer, framing/order and provenance verifier.
- [x] Compare all four 120-frame Windows/Linux sessions; report per-field differences.
- [x] Negative controls (16 pass), full CTest (55 pass / 0 fail / 1 skip), privacy review.
- [x] Prepared focused commit/Draft PR publication and issue handoff; publication receipt on #321 is authoritative. No merge.

Result: Windows 818/818 indexed hashes/sizes; Linux 776/776; all four original-SDK profiles 120/120 normalized packet equality. See `docs/ISSUE321_WINDOWS_CORPUS_VERIFICATION.md`.

Unresolved evidence: actual playback framerate, absent historic binaries/archive, source-to-build/run provenance, original seed and trace authenticity. No packet-serialized internal state claim.

Next action: maintainer reviews reproducible script and evidence, then #322/#325 consumers qualify exact candidate heads. Keep #321 open; do not generate bulk exports.

## Historical issue #331 preparation snapshot (2026-10-30)
Status: REVIEW preparation; source PR head 18547b762aa0642948466b7bde24ca14db51b787.
Forward integration target: main 04c348f848de1eb3a47d3b90fe191722c1ab559e.
Original first-frame previous bytes are null (no sample); probe initial bytes are zero.
Optional lowpass tags accepted; required highpass pairs and coordinate types fail closed.
Checklist: corrected verifier complete; 18/18 contract tests (also Python -O); full quality Release CTest 42 PASS / 0 FAIL / 0 SKIP; 606 indexed files verified, 480/480 packet tables equal; four historical control tests red as expected. Privacy-reviewed publication pending.
Scope: #331 packet tables only, complementary to #322 byte/direct-state gate; #320 parent-owned.
Blocker: native Windows deferred while node unavailable. No original SDK state claim.
Next action: normal push existing PR branch after outgoing-commit privacy scans, verify exact head and publish sanitized handoff. Hosted CI pending; no protected-main merge.
Raw logs retained outside Git; no corpus writes or tracked upstream edits.
