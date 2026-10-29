<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Six profiles on the SDK-free writer (#46)

Status: **SIX_PROFILE_PACKET_PARITY_1080P** / **4K_PACKET_PARITY_1F** /
**8K_GEOMETRY_BLOCKED** / **120F_FS2_FS3_PASS**.

This closes the seam left by #315. That change routed the writer's tables
through the shared SDK-free calculator but admitted only FilmScan3, refusing
the other five with an explicit reason. The reason was honest at the time: no
one had shown what would come out. It has now been measured, and the answer is
that the bytes were already correct except for one header field.

## What was measured, not assumed

Every number below comes from the pinned Windows corpus, hashed against its own
`TRANSFER_SHA256.tsv` (1760 entries, 5,970,674,259 bytes, SHA-256
`2343a5ef…8205`) before any packet was read.

1. **The calculator is right for all six profiles.** For each frame the
   packet's own quant tags (53) and scale tags (54) were extracted and compared
   against `CalculateQualityState`. All six profiles agree on the comparable
   high-pass slots of every frame at 1080p (96 frames), 4K frame 0 and 8K frame 0
   (12 more): **108 packets, zero mismatches**. The manifest tables agree independently, so this is a
   three-way statement (packet, manifest, calculator), not a self-check.
2. **The packet skeleton is profile-invariant.** All six profiles emit an
   identical sequence of tag ids — 463 tags at 1080p — and identical
   `scale`/level-scale values. What differs is the sample size (tag 1), the
   profile id (tag -80) and the tag-53 quant entries.
3. **The writer, once the header field follows the session, is byte-exact.**
   With tag -80 written from the session profile instead of a hard-coded 6,
   the SDK-free writer reproduces the original Windows packets
   **byte-for-byte after variable-metadata normalization only**:
   - 1080p, 16 frames × 6 profiles: 96/96 identical
   - 4K frame 0 × 6 profiles: 6/6 identical
   - `sequence-120f`, 120 frames × filmscan2/filmscan3: 240/240 identical
   Before the fix, exactly **one byte per frame** differed — offset 87, tag
   -80 — in all five previously refused profiles and in none of FilmScan3.

That last point is the whole finding. The writer was never writing the wrong
picture; it was writing the right picture under the wrong label.

## The change

- `src/core/filmscan3_encoder.cpp`: tag -80 carries the session quality enum
  (`1..6`), not a constant. The frozen FilmScan3 rate cross-check now applies to
  FilmScan3 only — comparing another profile against a formula that was never
  its law would fail on correct input.
- `src/core/quality_profile_bridge.cpp`: `IsQualifiedSessionProfile` accepts all
  six regular profiles. `RequireTablesMatchFrozen` stays as the FilmScan3 oracle
  and is invoked for FilmScan3 only; the other five are gated on real packets
  instead.
- `tools/hypercine_encode_filmscan3.cpp`: explicit `--quality PROFILE`.
- `tests/test_profile_enter_contract.cpp`: the five refusals are gone; the suite
  now proves six accepted profiles produce six **distinct** quant tables from
  identical input, and that tag -80 follows the session.
- `tools/p6_writer_parity.py` + `tools/p6_profile_measure.py`: the measurement
  harness. Registered as `p6_six_profile_packets` when
  `HYPERCINE_QUALITY_ORACLE_ROOT` is set; a missing corpus registers nothing
  rather than passing.
- `docs/MACOS_PROFILE_ENTRY_46.md` is superseded on the refusal paragraph by
  this file.

Note that the low-pass band carries no tag 53/54: it declares its quantization
implicitly (tag 34 = 1) and its scale through tag 45. The comparator therefore
checks the 27 high-pass slots positionally and states the low-pass separately,
instead of silently aligning mismatched indices.

## Guard liveness

`profile_enter_contract` was mutation-checked: reverting tag -80 to the constant
`6` makes it fail with *"header quality stamp did not follow the session
profile"*. A guard that cannot fail is not evidence.

## What this does NOT claim

| Claim | State |
| --- | --- |
| Six profiles byte-exact at 1080p vs original SDK | **PASS** (96/96) |
| 4K byte-exact, frame 0, six profiles | **PASS** (6/6) |
| 8K byte-exact | **NOT RUN — geometry budget rejects 8K** |
| 120-frame sequences, filmscan2/3 | **PASS** (240/240) |
| 120-frame sequences, low/medium/high/filmscan1 | **NOT AVAILABLE** in corpus |
| Overbitrate hysteresis window | **NOT EXERCISED** by this corpus |
| Metal per-profile | **ABSENT** (blocked by #45) |
| macOS native execution of this change | **NOT RUN** (Linux x64 only) |
| Decode / PSNR / visual quality | **NOT CLAIMED** |

The 8K blocker is separate and named: `ValidateFilmScan3Geometry` caps the
pixel budget at 8,294,400 px, which is exactly 4K. Raising it is a memory and
allocation decision, not a quality-profile one, and is not part of this change.

The overbitrate note matters for the low/medium/high profiles: their tables can
switch to the mid-point table when the bitrate sits between 80 % and 100 % of
the profile ceiling. On this corpus the feedback chain stayed identical whether
or not `previous_overbitrate` was carried, so this run does not distinguish the
two behaviours. The writer currently passes a constant 0. That is a real,
untested gap — not a proven equivalence.
