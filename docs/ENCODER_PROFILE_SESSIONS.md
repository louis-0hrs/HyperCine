# Internal session quality profiles (#259, step 3)

This step makes the internal capability/session contract profile-aware. It is the
step the merged six-profile writer (#318, `FilmScan3Encoder::SetSessionQuality`)
left open: the writer accepted six profiles while `detail::CheckEncoderCapability`
still refused five of them with `ValidButNotImplemented`. Nothing here is a
product qualification or a direct original-SDK/GPU reference. The combined
#320 integration also carries native measurement-only GPU tests; their evidence
is separate from production session admission (see below).

## Scope

- Changed: `src/core/encoder_session_config.h`, `src/core/encoder_capability.cpp`,
  `src/core/encoder_backend_selection.cpp`, one new CTest, and the two existing
  session/capability contract tests whose expectation was the old
  FilmScan3-only rule. The combined integration adds measurement-only CUDA/Metal
  profile matrices and guarded registrations; it does not expand GPU admission.
- Unchanged: public headers, `FilmScan3Encoder` constructors, the legacy fixed-Q2
  path, RGB444, `upstream/`, and the single sample writer. There is no second
  quality table calculator and no second writer.

## Admission matrix

| Profile | CPU scalar | Metal | CUDA |
| --- | --- | --- | --- |
| Low | admitted | refused (`ProfileNotQualifiedForBackend`) | refused |
| Medium | admitted | refused | refused |
| High | admitted | refused | refused |
| FilmScan1 | admitted | refused | refused |
| FilmScan2 | admitted | refused | refused |
| FilmScan3 | admitted | admitted | admitted |

Admission is a reviewed session policy, not an automatic consequence of a
measurement-only parity result. It does not mean original SDK qualification.

- **CPU scalar, six profiles.** They share one writer, one rate-state calculator
  and one quality stamp, and #318 pinned 16-frame 1080p packets for all six,
  one 4K frame zero for all six, and 120-frame FilmScan2/FilmScan3 sequences.
  A profile is therefore a table/state selection inside a contract that is
  already admitted, not a new geometry or backend claim.
- **Metal and CUDA, FilmScan3 only.** The merged GPU session work (#312, #319)
  carries native packet evidence for FilmScan3. Admitting another profile there
  is still refused by policy. The combined #320 matrix now measures six profiles
  through a test-only frontend/writer seam and asserts those typed refusals.
  This is not production session admission or direct original-SDK/GPU parity.
  Expanding admission requires a separate source-bound qualification/review.

## New refusal axis

`CapabilityReason::ProfileNotQualifiedForBackend` plus
`CapabilityFlags::profile_not_qualified_for_backend` and the result fields
`profile_admitted` / `profile_note`.

It is deliberately separate from the neighbouring reasons:

| Condition | Reason |
| --- | --- |
| Enum outside the six regular profiles | `FormatMathematicallyInvalid` |
| Legal profile, no packet evidence on the effective backend | `ProfileNotQualifiedForBackend` |
| Profile implemented but the writer cannot express it (e.g. RGB444 family) | `ValidButNotImplemented` |
| Backend/geometry pair without a record | `BackendNotQualifiedForGeometry` |

The invalid-enum case stays a format error so a mistyped cast cannot be reported
as an evidence gap, and it stays a format error with fallback enabled.

## Semantics that are easy to get wrong

- **Evaluated on the effective backend.** An explicitly permitted scalar fallback
  is a different backend with a different matrix, so the check runs after the
  effective backend is settled. A caller therefore cannot obtain a profile on a
  backend the contract never admitted.
- **Fallback does not repair a profile refusal.** A GPU request for `Medium`
  with `allow_cpu_scalar_fallback` stays a `ProfileNotQualifiedForBackend`
  refusal instead of silently becoming a CPU session. Only a *backend-presence*
  or *geometry* refusal is fallback-eligible; that is the existing rule and it is
  unchanged.
- **The profile travels through the existing entry point.** `EncoderSession`
  calls `SetSessionQuality` after capability permission and after the selected
  frontend is bound, and before the diagnostic policy. A refused session never
  holds a profile the contract did not authorize, and a profile that the shared
  writer itself rejects throws as `std::logic_error` from the writer rather than
  being swallowed.
- **Profile state stays per session.** Two sessions on different profiles keep
  independent limiter/hysteresis/previous-sample state, tables and header stamps
  across `Reset()`, because the profile is a session field, not a process-global
  switch.

## Tests

`encoder_profile_session_matrix` (`tests/test_encoder_profile_sessions.cpp`),
CPU-only, no GPU required:

- all six profiles on CPU scalar: admitted, encoded, and the emitted packet
  carries its own tag `-80` quality enum (1..6);
- writer quant tables equal frozen literal per-profile vectors for all 36
  entries, not the same calculator used by the writer; starting limiter is pinned;
- Low and FilmScan3 produce different table sets, so the six profiles cannot
  collapse into one;
- Metal and CUDA refuse the five non-FilmScan3 profiles with
  `ProfileNotQualifiedForBackend`, typed status, no factory call, and no fallback
  repair; FilmScan3 stays admitted at capability level (the native GPU packet
  gates remain `encoder_metal_session_packets` / CUDA session tests);
- the profile refusal is not contaminated by the geometry, memory, presence,
  implementation or format axis, and keeps its independent geometry evidence;
- two concurrent sessions on different profiles keep separate state, bytes and
  stamps across `Reset()`.

Existing expectations updated in `encoder_backend_selection_contract` and
`encoder_capability_contract`: a legal profile on CPU scalar is now admitted, and
its absence on a GPU backend is the new typed profile refusal.

## Completed combined measurement evidence (historical source head)

At source head `a1861102eed283bff01df435727dc2b5c9a17784` the native owners
reported A2 CUDA Release **130/130** and M2 Pro Metal Release **114/114**, zero
failures/skips. Each native matrix executes 240 frames (six profiles, two
patterns, twenty frames), including frames 8/17 and Low/Medium/High overbitrate
above one. The Metal run reports 6,480 wavelet dispatches. These are historical
owner results, not new GPU executions by the documentation follow-up.

The [native Metal handoff](https://github.com/louis-0hrs/HyperCine/pull/320#issuecomment-6526417925)
pins the exact head and limits. The [integration evidence](PROFILE_BACKEND_INTEGRATION_259.json)
records unchanged source/test blobs. Original #330 head `9e60f0c` failed native
coverage; corrected stimulus in canonical #320 does not make that old head pass.

Independent CPU original-SDK gates in #322/#331 remain complementary evidence,
not a direct GPU-vs-SDK test. Metal FFmpeg/ffprobe were unavailable, so external
decode was not registered, not passed. Native Windows and final maintainer
acceptance remain open. GPU session admission is unchanged.

## Explicit non-claims

- No new GPU profile session admission or direct GPU-vs-original-SDK proof.
- No new native GPU run in this documentation follow-up; cited native parity
  is historical, source-bound, measurement-only evidence.
- No 8K, no alternate timebase, no RGB444, no CPU SIMD, no Vulkan.
- No original-SDK per-profile 4K/8K qualification, no SMPTE VC-5 conformance,
  no product release, and no decoded-pixel/PSNR claim.
- #46 retains remaining per-profile qualification work. Long CPU original
  sequences and direct hold witnesses now exist in #322; native measurements
  do not close GPU/original, native Windows, or larger-geometry gaps.
