# Canonical profile/backend integration (#320 / #259)

Status: IN_PROGRESS / final native Metal qualification pending.

Branch: expert/issue259-profile-main-20261030, continuing existing Draft #320.
Base: dbe715a5682f3b934290088a3ca5a887fc4d7957. Forward main: 04c348f848de1eb3a47d3b90fe191722c1ab559e. No protected-main merge or force push.

- [x] Clean forward-main integration, no duplicate quality-state implementation/test diff.
- [x] Preserve canonical typed profile refusal and single session quality path.
- [x] Extract #330 measurement-only CUDA/Metal matrices without competing admission.
- [x] Native A2 full Release standalone CUDA suite: 130/130, zero failures/skips, 268.04s. Earlier interrupted attempts do not count.
- [x] Required regular SDK suite: 58 passed / 59 total, zero failures, ffmpeg_oracle_qualification skipped, 321.74s.
- [x] Inventory guardrails:31 tests pass; Metal source C++17 syntax check.
- [x] Independent current-main #322:31b9c8f,129/129 private,122/122 public, original Linux480/480,A480/480,B145/145,C21/21 packets and directly captured state. History preserved.
- [x] Independent #331:d34eefd,42/42 suite,606 artifacts verified,480/480 original packet tables.
- [ ] Commit and normal update existing #320 branch after privacy gates.
- [ ] Native Metal canonical-head result and exact-head hosted CI.
- [ ] Independent policy/diff review; no automatic merge.

Measurement-only GPU writer seams do not admit five previously refused GPU profiles. CPU/GPU equality is not original SDK equality. Native Windows deferred (node down); read-only fixture replay on Linux is separately labeled. #259/#46 remain open.

Next action: privacy-scan and commit this tested integration, normally update existing #320 branch, then native Metal on exact resulting source. Evidence in docs/PROFILE_BACKEND_INTEGRATION_259.md; raw logs stay external.
