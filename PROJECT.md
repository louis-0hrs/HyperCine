# Draft #322 — current-main continuation

Status: REVIEW. Owned gates/docs only, isolated existing branch.
Current-main integration target: 04c348f848de1eb3a47d3b90fe191722c1ab559e.
Forward merge validated by full CTest before commit; root CMake merged automatically.
Original historical FAIL 78/120 and negative 40f ramp preserved.
Windows-native deferred (node unavailable); original Windows corpus replay on Linux allowed.
#320 canonical integration and #331 independent verifier are separate owners.

Checklist:
- [x] Read current coordination, verify pseudonymous identity and worktree hooks.
- [x] Claim existing Draft #322 continuation.
- [x] Forward-integrate current main without deleting tests or changing upstream.
- [x] Private 129/129 PASS; public 122/122 PASS; regular 58 PASS /59 total, 1 ffmpeg_oracle_qualification SKIP.
- [ ] Source-bound evidence, staged/every-outgoing-commit privacy and publication.

Next action: privacy-reviewed publication to existing Draft #322; independent review and exact-head CI remain pending.
Blockers: no Windows-native execution; exact-head hosted CI remains separate.

---
Historical measurement snapshots follow; their old dependency statuses are historical.

# Issue 46 — SDK golden refresh (Draft #322)

Status: REVIEW / local implementation and validation complete.
Publication receipt and exact final head are recorded in the Draft #322 / #46 handoff.
Main `9909bfc` forward-merged as `68e4dd5`; no conflicts, no tests removed.
The exact #325 dependency `0be5bef` is locally integrated into this branch only;
no protected/main or GitHub PR merge. Corpus remains read-only.

Checklist:
- [x] Read current coordination, activate local privacy hooks, verify identities.
- [x] Independently verify all four corpus inventories (1,594 indexed artifacts).
- [x] Preserve historical 78/120 FAIL records and explicitly test their preservation.
- [x] Separate rejection of the original negative 40-frame ramp from positive gates.
- [x] Add independent Windows A/B/C packet/state gates and adversarial controls.
- [x] Required regular Release build/full suite: 55 PASS, 1 SKIP, 0 FAIL / 56.
- [x] Main-only SDK baseline retains real failures; corrected private full suite 125/125 PASS, 0 skipped, 432.69 s.
- [x] Corrected public standalone full suite 118/118 PASS, 0 skipped, 199.18 s.
- [x] Source-bound sanitized result report and manually reviewed staged privacy scan.
- [ ] Maintainer acceptance of dependency/test scope and exact-head hosted CI.

Open acceptance gates: #325 dependency review/landing; independent review of the
new tests; exact-head hosted CI; original Windows build/ISA, original seed and
explicit SDK FRMR qualification. GPU/4K/8K not tested in this lane.
Next action: maintainer reviews the existing Draft #322 and #325 dependency,
checks exact-head hosted CI and decides integration; no autonomous merge. See docs/SDK_GOLDENS_REFRESH_46.md
and its source-bound JSON; original historical records remain unchanged.

---

## Historical initial #322 handoff (unchanged below)

# Issue 46 — missing original SDK packet oracles

Status: REVIEW / BLOCKED_QUALIFICATION; hold-band experiment negative.
Original unmodified SDK encoder built successfully on Linux
x64 with GCC 13.3, libuuid and OpenMP. SDK-only capture and standalone comparison
are separate executables; private artifacts stay outside Git.

Checklist:
- [x] Measure frozen SDK build without editing upstream.
- [x] Add public-API-only aligned packet capture, portable Python SHA-256 layer.
- [x] Finish four 120-frame captures and fixed 40-frame hold-ramp experiment.
- [x] Measure packet parity and publish actual per-frame bitrate/tag evidence.
- [x] Full opted-in CTest: 116/118 PASS, 2 substantive oracle FAIL, 0 skipped.
- [ ] Privacy review, commit, push, Draft PR and sanitized issue handoff.

Blockers: fixed ramp reached 0 hold-band frames in all three profiles; no
hold-state claim. Medium and High 120-frame packet parity exposed real
long-session divergences (78/120 byte-identical each); do not repair codec in this capture task.
New private corpus inventory: 776 files, 2,024,958,339 bytes; transfer SHA256
`dfb7cace17a73b7bfd1871be545d07acf14692adaafda25907370edec5b6dffb`.
Baseline full CPU standalone CTest 115/115 PASS; complete opted-in suite
116/118 PASS, 2 FAIL (strict hold and sequence parity), 0 skipped.
Four-profile parity: Low 120/120 PASS, Medium 78/120 FAIL, High 78/120 FAIL,
FilmScan1 120/120 PASS. All three ramp sessions 40/40 packet parity, no hold.
Next action: review the long-session divergence and negative hold trajectory
before authorizing a bounded follow-up capture/fix. No support promotion.
