# Combined #320 + #322 + #331 integration — merged

Status: **MERGED** to `main` through [PR #339](https://github.com/louis-0hrs/HyperCine/pull/339) on 2026-11-01.
Integration source: `baccccffe4ec2cab5fe3807e66a49318657f014a`.
Main merge commit: `37908fc6ead55e5db254ffbac4c9af7961b816b2`.
Base: `04c348f848de1eb3a47d3b90fe191722c1ab559e`.

Merged sources: #320 `6fbee4a072681247feef55b9cf1317eceecdced9`,
#322 `c9172d497e3de5d827e0d4b77439ca910fe89af7`,
#331 `59febad252d0ca9f65790542622a11b034b1a039`.
All three source Draft PRs were closed after the combined merge; do not merge them separately.

The exact-source acceptance is recorded in #339: Linux private strict **130/130**,
quality-only Group A **42/42**, Windows public strict **123/123**, native A2 and
Metal GPU-required profile matrices **1/1 each**, and hosted head checks
**28/28**. See [Linux/CI](https://github.com/louis-0hrs/HyperCine/pull/339#issuecomment-6558259804),
[GPU](https://github.com/louis-0hrs/HyperCine/pull/339#issuecomment-6558291175), and
[Windows](https://github.com/louis-0hrs/HyperCine/pull/339#issuecomment-6558331173).
The main merge SHA has not been separately native-retested; these receipts bind
to the unchanged integration source and must not be represented as new post-merge runs.

Full source/provenance inventory, overlap resolution and portable test commands:
`docs/PROFILE_INTEGRATION_320_322_331.md` and `.json`.

**Scope:** CPU scalar internal six-profile sessions; CUDA/Metal production sessions
remain FilmScan3-only. Original SDK observable packet-table replay is distinct
from directly observed SDK state. Historical SDK source/binary/compiler/ISA,
seed/FRMR, independent pixel oracle, direct GPU-original and SMPTE VC-5
conformance remain unqualified. Preserve #187 HOLD and keep #46/#259/#321 open.
No product release or broad GPU admission is implied.

---
## Historical canonical project status (preserved)

# Canonical profile/backend integration (#320 / #259)

Status: REVIEW / documentation correction for 2026-10-30 integration review.

Existing Draft: https://github.com/louis-0hrs/HyperCine/pull/320
Publication branch: expert/issue259-profile-sessions.
Historical native-tested source: a1861102eed283bff01df435727dc2b5c9a17784.
Integrated main: 04c348f848de1eb3a47d3b90fe191722c1ab559e.
Nightly continuation: expert/nightly-20261031-259; docs only, no new GPU runs.
No protected-main merge, force push, upstream edit or draft promotion.

- [x] Current-main integration; no duplicate quality-state/test implementation.
- [x] Preserve typed refusal, no silent fallback, one session quality path.
- [x] Extract only #330 measurement tests, no competing admission policy.
- [x] Historical owner A2 Release CUDA 130/130, zero failures/skips, 268.04 s.
- [x] Historical regular SDK 58/59, zero fail, ffmpeg_oracle_qualification skip.
- [x] Historical owner canonical M2 Pro Metal 114/114, zero failures/skips,
      116.93 s; 240 frames/6,480 wavelet dispatches; exact-head handoff linked.
- [x] Historical exact-head hosted checkpoint a186110: 28/28 SUCCESS reported
      by owner and independent integration review, not new documentation-head CI.
- [x] Independent #322 31b9c8f and #331 d34eefd corpus evidence recorded
      separately; neither is a direct GPU/original SDK comparison.
- [x] 2026-10-30 independent policy/diff review received; documentation drift
      identified. This correction pins evidence/limits and removes stale status.
- [ ] Final maintainer acceptance, including native Windows qualification limit.

GPU measurements do not admit the five typed-refused GPU profiles. Metal
FFmpeg/ffprobe were unavailable: external decode not registered, not passed.
No original SDK in GPU matrix; no native GPU sanitizer/performance, 4K/8K,
timebase, product, decoded-pixel or SMPTE/VC-5 claim. Windows-native remains
deferred; this docs worker has not rechecked node health. Keep #259/#46 open.

Nightly 2026-10-31 CPU-only verification (Debian 13 x86_64, GCC 14.2.0,
CMake 3.31.6, Release, bounded parallelism 2, benchmarks/GPU OFF):
- [x] Full official standalone gate: 120 registered, 120 passed, zero fail/skip;
      required external decode ON. Public fixtures only; private replay NOT_RUN.
- [x] Full regular SDK CTest: 57 passed / 58 total, zero fail, one
      ffmpeg_oracle_qualification skip (FFmpeg 7.1.5 not on measured list).
- [x] JSON/source consistency: 47 assertions; all 12 recorded source/test blobs
      unchanged from historical GPU source. Exactly four documentation files changed.
Publication/CI receipt belongs to the linked PR/issue handoff for the resulting
commit. Do not transfer historical 28/28 hosted success to this documentation head.
Raw logs and per-run checklist remain outside Git. Historical source/test blobs
are pinned in docs/PROFILE_BACKEND_INTEGRATION_259.json.

Next action: maintainer reviews this docs-only correction and decides the
remaining native Windows/qualification limits; keep #320 Draft, do not merge
competing #324/#330 policies independently.
