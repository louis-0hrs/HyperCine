<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Where the shared profile tables enter `FilmScan3Encoder` (#46)

Status: **PROFILE_SEAM_PASS** (superseded on profile admission by
`docs/P6_SIX_PROFILES_46.md`).

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
        |                                   |
        |                                   | state.quant[36] / state.scale[36]
        |                                   v
        |                      checked int -> unsigned copy  --->  writer tables
        |
        +-------> ComputeFilmScan3Tables(limiter)  (oracle only; never copied)
                        throws std::logic_error on any slot drift
        |
        v
FilmScan3Frame::{tables, state.fs_rate_limiter}
```

Two properties matter and both are enforced in code rather than documented:

1. **One producer.** The frame's 36-entry `quant`/`scale` pair and its rate
   limiter are copied out of `CalculateQualityState`'s own `quality_state`
   arrays after a checked conversion, and that pair is what `Encode` writes.
   `ComputeFilmScan3Tables` is never assigned into `ProfileTables`; it is only
   read for comparison, so the byte stream cannot come from the legacy helper.
   No second quantizer table was added; `hypercine_standalone` links
   `hypercine_quality_state`.
2. **A drift is a defect, not a preference.** `RequireTablesMatchFrozen`
   compares all 36 slots of the shared result against `ComputeFilmScan3Tables`
   and throws `std::logic_error` naming the slot that disagreed; a negative
   entry or one that does not fit the writer's `unsigned` table type is also a
   hard error rather than a silent narrowing. `Encode` likewise compares the
   shared limiter against the frozen FilmScan3 rate formula, so a behavioural
   change in either side fails loudly instead of shifting bytes.

The frozen FilmScan3 table helper therefore stays as the *oracle* the shared
calculator is verified against — its output is already pinned by golden bytes,
which makes it a usable independent statement of the pre-#46 tables, and
`profile_enter_contract` proves the guard fires on a deliberately drifted slot
in both directions. It is no longer a producer.

`quality_profile_bridge.cpp` includes the standard headers it uses directly
(`<algorithm>`, `<limits>`, `<stdexcept>`, `<string>` for `std::to_string`)
instead of relying on transitive inclusion; MSVC rejects the transitive form.

## Superseded: the five other profiles are now enabled

This section previously explained why Low/Medium/High/FilmScan1/FilmScan2 were
refused. That refusal has been retired by measurement, not by preference:
`docs/P6_SIX_PROFILES_46.md` records that the SDK-free writer reproduces the
original Windows packets byte-for-byte for all six profiles once tag -80
carries the session quality enum. Only FilmScan3 had been tried, so only
FilmScan3 was labelled; the writer itself was never FilmScan3-specific.

The refusal boundary is retained in the code as a *value* rather than deleted
silently: `p6_six_profile_packets` fails when any profile's emitted bytes
diverge from the pinned originals, and `profile_enter_contract` fails when the
header stamp stops following the session.

## Measured facts behind the design

`ComputeFilmScan3Tables` and the shared calculator's FilmScan3 branch were
compared for every legal limiter `0..20`: they agree on all 36 slots. The
observed pinned corpus frame-0 tables match the same arithmetic
(`filmscan3` limiter 4, `filmscan2` limiter 8). That equality is why the
cross-check is cheap and why no second table set needs to exist, and it is also
why demoting the helper to an oracle changes no emitted byte: the pinned
185,648-byte frame-0 sample is unchanged.

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
| Writer tables are the shared `state.quant/scale` entries (not the oracle's) | PASS (`profile_enter_contract`, per-slot identity plus live drift guard) |
| Shared-calculator rejections govern the seam (odd width, zero height) | PASS (`profile_enter_contract`) |
| Shared rate feedback == frozen FilmScan3 formula, whole window | PASS (`profile_enter_contract`) |
| Default/explicit FilmScan3 byte identity | PASS (185,648 B sample unchanged) |
| Five unqualified profiles fail closed | PASS |
| Per-profile original SDK packet parity | PENDING (needs #36 per-profile goldens) |
| Metal per-profile gates | PENDING (blocked by #45) |
| 4K / 8K quality status per profile | PENDING |

Next action: after #45 lands, add the Metal per-profile session adapter on top
of this seam — same `ComputeProfileTables` call, no duplicated writer — and
gate each profile on its own original SDK packet bytes.
