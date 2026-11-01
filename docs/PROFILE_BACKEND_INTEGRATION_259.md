# Shared profile/backend integration (#259)

## Selected path

Canonical policy remains #320: typed ProfileNotQualifiedForBackend, reference/memory/geometry priority, no silent fallback around a profile refusal, and one EncoderSession::SetSessionQuality path. CPU scalar has six regular profiles; CUDA and Metal sessions retain FilmScan3-only admission. SIMD and Vulkan remain outside this session matrix.

Forward-integrate main 04c348f848de1eb3a47d3b90fe191722c1ab559e (including #325/#329/#326/#328); quality_state.cpp, test_quality_state.cpp and the original hysteresis regression must be identical to main. No duplicated ladder implementation, rebase, force push or protected-main merge.

## Measurement does not grant admission

Only native CUDA/Metal matrix tests and guarded CMake registrations from #324/#330 are carried forward. Their competing capability/session policy is not imported. A qualification-only test uses the existing internal frontend seam with the existing writer, while asserting unchanged typed production refusal for every unadmitted profile. CPU uses the canonical session. There is no new writer, public API or second quality calculator.

Each native matrix must execute all six profiles for twenty consecutive frames with deterministic gradient and seeded-noise passes. Compare complete packets, 36 quant entries, 36 scale entries, frame index, previous packet size, limiter/overbitrate, tag -80 and all 30 declared-full coefficient hashes. Require real dispatches, no fallback, frames 8/17 and overbitrate above one for Low/Medium/High. Six distinct frame-zero quant tables prevent trivially collapsed profile equality.

GPU-required tests are registered only for their enabled native backend; no non-Darwin Metal skip stub enters Linux totals. Missing registration fails inventory guardrails. Existing hosted standalone jobs do not enable Metal: their success is not native Metal evidence.

## Independent original SDK gates

#322 owns normalized whole-packet comparisons and directly observed original SDK group C state witnesses. #331 owns independently indexed Group A packet quant/scale replay. These are complementary axes, not competing codec implementations. Packet tables cannot prove internal SDK counters; CPU/GPU equality cannot prove original SDK equality.

Preserve historical Medium/High 78/120 failures and the negative forty-frame ramp. Each corpus retains its own SHA-pinned index. Raw assets remain external/read-only; private bindings use HYPERCINE_GOLDEN_DIR.

## Completed evidence and provenance

Historical canonical source head: `a1861102eed283bff01df435727dc2b5c9a17784`.
Its source/test blobs remain pinned in the adjacent JSON. This documentation
follow-up changes no implementation, test, build, or upstream file and does not
rerun unchanged GPU workloads.

- Native owner A2 CUDA Release: **130/130**, zero failures/skips, 268.04 s.
- Native owner M2 Pro Metal Release: **114/114**, zero failures/skips, 116.93 s;
  240 frames, 6,480 wavelet dispatches and 240 color/command-buffer/wait operations.
  [Exact-head native Metal handoff](https://github.com/louis-0hrs/HyperCine/pull/320#issuecomment-6526417925).
  Five non-FilmScan3 GPU session profiles remain typed-refused. FFmpeg/ffprobe
  were unavailable on that Mac: external decode was not registered, not passed.
  Original SDK not wired into GPU matrix; no native GPU sanitizer/performance claim.
- Required regular suite at that source: **58/59**, zero failures, one
  `ffmpeg_oracle_qualification` skip, not decoder proof.
- Independent #322 `31b9c8f0502049f9ff23a75a1f05b9b4eaf99fe3`: 129/129 private,
  122/122 public; Linux 480/480 and Windows A 480/480, B 145/145, C 21/21 packets
  plus direct state. Linux replay is not native Windows.
- Independent #331 `d34eefd025cb6f423c672b540e64a8066aa5103e`: 42/42, 606/606
  artifacts, 480/480 observable tables. Neither corpus is freshly replayed here.
- Historical hosted checkpoint: owner and 2026-10-30 independent integration
  review report 28/28 successful checks on `a186110`. That is not a hosted
  result for a later documentation commit.

Original #330 `9e60f0c` failed native Metal overbitrate coverage. The corrected
canonical stimulus preserves that failure rather than relabeling it.

## Remaining acceptance

- The 2026-10-30 integration review inspected policy/fallback and requested this
  documentation correction; it did not independently replay private fixtures.
  Final maintainer acceptance remains open, including the native Windows limit.
- Native Windows execution remains deferred; Linux fixture replay does not
  replace it. The nightly documentation worker has not rechecked node health.
- Check exact-head hosted CI after this update; historical 28/28 is not new-head CI.
- Separate source-bound native GPU/original SDK qualification and explicit review
  before later profile admission expansion. Measurements do not authorize it.
- Do not independently merge competing admission policies from #324/#330.

No 4K/8K profile qualification, alternate-timebase, decoded-pixel quality, product
release or SMPTE/VC-5 conformance claim. Keep #259/#46 open and #320 Draft.
Next: review the docs-only correction and decide remaining qualification limits;
no autonomous merge or draft promotion.
