# Issue #321: independent Windows corpus verification

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

## Issue #331 isolated continuation (2026-10-30)
Status: REVIEW preparation; source PR head 18547b762aa0642948466b7bde24ca14db51b787.
Forward integration target: main 04c348f848de1eb3a47d3b90fe191722c1ab559e.
Original first-frame previous bytes are null (no sample); probe initial bytes are zero.
Optional lowpass tags accepted; required highpass pairs and coordinate types fail closed.
Checklist: corrected verifier complete; 18/18 contract tests (also Python -O); full quality Release CTest 42 PASS / 0 FAIL / 0 SKIP; 606 indexed files verified, 480/480 packet tables equal; four historical control tests red as expected. Privacy-reviewed publication pending.
Scope: #331 packet tables only, complementary to #322 byte/direct-state gate; #320 parent-owned.
Blocker: native Windows deferred while node unavailable. No original SDK state claim.
Next action: normal push existing PR branch after outgoing-commit privacy scans, verify exact head and publish sanitized handoff. Hosted CI pending; no protected-main merge.
Raw logs retained outside Git; no corpus writes or tracked upstream edits.
