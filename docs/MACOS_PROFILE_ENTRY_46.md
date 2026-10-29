<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Where the shared profile tables enter `FilmScan3Encoder` (#46)

Status: **PROFILE_SEAM_PASS / PROFILES_UNQUALIFIED_BEYOND_FILMSCAN3**.

Issue [#46](https://github.com/louis-0hrs/HyperCine/issues/46) asks for six
CineForm qualities on the macOS CPU/Metal FilmScan3 path with the *same*
quality state as the shared core. The previous audit on that issue named the
remaining gap as architectural, not numerical: `EncodingQuality` was consumed
by `phase2d`/`phase3a` but never reached the FilmScan3 encoder, which derived
its tables from `fs_rate_limiter` through its own `ComputeFilmScan3Tables`.

This change closes that seam. It does **not** start P6 feature work: #45
(Metal 4K) is still open, the writer is still the single progressive YUV422 p10
GOP1 family, and no per-profile Metal gate exists.

## The seam

`src/core/quality_profile_bridge.{h,cpp}` is the single entry point:

```text
FilmScan3Encoder::Encode
        |
        v
ProfileTableRequest{quality, frame_index, limiter, prior sample bytes, geometry}
        |
        v
ComputeProfileTables  ------ >  CalculateQualityState()   (#29, SDK-free)
        |
        v
ComputeFilmScan3Tables(limiter)  --- cross-checked against the shared result
        |
        v
FilmScan3Frame::{tables, state.fs_rate_limiter}
```

Two properties matter and both are enforced in code rather than documented:

1. **One producer.** The frame's 36-entry `quant`/`scale` pair and its rate
   limiter come from `CalculateQualityState`. No second quantizer table was
   added; `hypercine_standalone` now links `hypercine_quality_state`.
2. **A drift is a defect, not a preference.** `ComputeProfileTables` compares
   all 36 slots of the shared result against `ComputeFilmScan3Tables` and
   throws `std::logic_error` naming the slot that disagreed. `Encode` likewise
   compares the shared limiter against the frozen FilmScan3 rate formula, so a
   behavioural change in either side fails loudly instead of shifting bytes.

The frozen FilmScan3 table helper is kept because it is the writer's only
quant/scale producer today and its output is already pinned by golden bytes. It
is now the *reference* the shared calculator is verified against, not a rival
implementation.

## Why the five other profiles are refused, not enabled

`FilmScan3Encoder::SetSessionQuality` and `ComputeProfileTables` throw
`std::invalid_argument` for Low/Medium/High/FilmScan1/FilmScan2, with an explicit
reason string. This is deliberate:

- The writer emits one CFHD family: progressive YUV422 p10, GOP1, 30 fps
  timebase, three channels, ten subbands. Low/Medium/High additionally use the
  upstream fixed-quality ceiling table (`overbitrate`) at 1080p, which this
  writer has never exercised.
- The pinned Windows corpus carries per-profile `quant`, `scale` and
  `fs_rate_limiter` for all six profiles at 1080p, 4K and 8K, but that qualifies
  the *calculator* (already proven on #46 by the corpus oracle), not this
  writer's packet bytes for those profiles.
- Emitting FilmScan3 bytes under a `medium` label would be a silent quality
  misrepresentation. Refusing is the honest state until each profile has an
  original SDK packet gate.

What the refusal buys is testability: the boundary is a value, not a comment.
`profile_enter_contract` proves all five are rejected and that FilmScan3 is
accepted.

## Measured facts behind the design

`ComputeFilmScan3Tables` and the shared calculator's FilmScan3 branch were
compared for every legal limiter `0..20`: they agree on all 36 slots. The
observed pinned corpus frame-0 tables match the same arithmetic
(`filmscan3` limiter 4, `filmscan2` limiter 8). That equality is why the
cross-check is cheap and why no second table set needs to exist.

## Scope and untouched behaviour

- `upstream/` unchanged; no SDK, CUDA, Metal or Vulkan header enters the
  SDK-free path.
- The default session remains FilmScan3, and an explicitly selected FilmScan3
  session is byte-identical to the default: the pinned 1920×1080 FlatBlack
  frame-0 sample stays 185,648 bytes with its existing SHA-256.
- `SetSessionQuality` is a session property: `Reset()` clears rate state but
  keeps the profile, because switching profiles mid-GOP is not allowed.
- No 4K/8K quality claim, no per-profile Metal adapter, no benchmark.

## Remaining gates

| Gate | State |
| --- | --- |
| Shared calculator == frozen FilmScan3 tables, limiter 0..20 | PASS (`profile_enter_contract`) |
| Shared rate feedback == frozen FilmScan3 formula, whole window | PASS (`profile_enter_contract`) |
| Default/explicit FilmScan3 byte identity | PASS (185,648 B sample unchanged) |
| Five unqualified profiles fail closed | PASS |
| Per-profile original SDK packet parity | PENDING (needs #36 per-profile goldens) |
| Metal per-profile gates | PENDING (blocked by #45) |
| 4K / 8K quality status per profile | PENDING |

Next action: after #45 lands, add the Metal per-profile session adapter on top
of this seam — same `ComputeProfileTables` call, no duplicated writer — and
gate each profile on its own original SDK packet bytes.
