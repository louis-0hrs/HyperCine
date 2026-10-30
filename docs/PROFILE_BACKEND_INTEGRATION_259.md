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

## Remaining acceptance

- Complete current-main #322 private suite and #331 original table replay.
- Native A2/Apple Metal results bound to final implementation/test blobs.
- Exact-head hosted CI and independent policy/integration review.
- Separate source-bound native GPU/original SDK qualification before any later profile admission expansion. Current measurements do not authorize it.
- Windows-native execution deferred while its node is unavailable, not silently replaced by Linux fixture replay.

No 4K/8K profile qualification, alternate-timebase, decoded-pixel quality, product release or SMPTE/VC-5 conformance claim. Keep #259/#46 open and affected PRs Draft pending review.
