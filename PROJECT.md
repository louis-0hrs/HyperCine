# Draft #322 — current review continuation (2026-10-31)

Status: REVIEW; Draft retained. Scope: gates/docs only.
Measurement/source baseline: `31b9c8f0502049f9ff23a75a1f05b9b4eaf99fe3`.
Integrated main: `04c348f848de1eb3a47d3b90fe191722c1ab559e`.
This continuation changes current documentation only, not codec, gates or tests.
The final documentation head/publication receipt belongs in the live PR handoff;
this is not a new private-corpus or native measurement.

Checklist:
- [x] Forward-integrated main; duplicate ladder hunks removed; implementation and quality-state tests match main.
- [x] Published baseline source-bound evidence, staged/outgoing privacy checks and Draft handoff.
- [x] Baseline hosted checks: 28/28 SUCCESS in the 2026-10-31 review snapshot (not native/private evidence).
- [x] Independent Linux CPU replay: private strict standalone 129/129 PASS, zero failed/skipped, 762.38s.
- [x] Independent original inventories: 1594 artifacts; Linux/A 480/480 each, B 145/145, C 21/21 packet/direct-state comparisons.
- [x] Preserved historical Medium/High FAIL 78/120 and negative 40f NON_QUALIFYING_NO_HOLD artifacts unchanged.
- [x] Kept #331 packet-table oracle distinct from #322 full-packet/direct-state gates.
- [ ] Maintainer acceptance of final diff/evidence and integration order; not inferred from independent replay.
- [x] Fresh documentation-only full public standalone: 122/122 PASS, zero failed/skipped, 388.98s; external decode required, GPUs OFF.
- [ ] New documentation-head hosted CI receipt: see live PR handoff; baseline success is not transferred.

Independent replay: https://github.com/louis-0hrs/HyperCine/pull/322#issuecomment-6543208160
Combined candidate: https://github.com/louis-0hrs/HyperCine/pull/320#issuecomment-6543352580
The latter records unpublished gate-union tree `40444f5eb9bd051ef2073f9f995e89afc1bcea28`
(quality-only 42/42 and private standalone 130/130), not a canonical PR head or merged-main/native result.
#331 independently owns observable packet-table replay; its SDK counters are NOT_OBSERVABLE.
#320 native evidence is attributed only to exact #320 head, not to this documentation continuation.

Remaining gates: native Windows execution; original Windows compiler/ISA provenance,
seed and explicit SDK FRMR qualification; maintainer acceptance. No GPU/original-SDK
pixel oracle, GPU admission expansion, 4K/8K, alternate-timebase, sanitizer, performance,
product or conformance qualification is supplied by this lane. Keep #46/#321 open.
Next action: maintainer reviews the final gates/docs diff and source-bound evidence,
checks the new documentation-head hosted CI and decides integration order with #331/#320.
No autonomous merge, release, upstream edit or protected-branch push.

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
