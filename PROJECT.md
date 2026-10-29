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
