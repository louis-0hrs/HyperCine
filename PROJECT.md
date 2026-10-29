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
