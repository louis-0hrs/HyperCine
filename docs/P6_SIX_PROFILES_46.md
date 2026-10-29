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
  rather than passing. The gate verifies every input and every SDK packet
  against the manifest's pinned length and SHA-256 *and* against the corpus
  transfer index before use, re-copies a cached work input whose hash no longer
  matches instead of trusting its size, discards stale output directories, and
  requires the encoder trace to describe exactly the requested frames in order
  rather than zipping short.
- `tests/test_p6_gate_negative.py`: negative controls for that gate, registered
  as `p6_gate_negative_controls`. It builds a same-length tampered SDK packet, a
  truncated trace, a reordered trace and a poisoned work cache, and requires the
  gate to reject or heal each one. Without these, a gate that always exits 0
  would look identical to a passing one — which is exactly what the first
  revision of `p6_writer_parity.py` did.
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
| Overbitrate hysteresis, forced trajectory | **PASS** (`profile_enter_contract` 5c) |
| Overbitrate hysteresis, real packets | **NOT EXERCISED** by this corpus |
| Metal per-profile | **ABSENT** (blocked by #45) |
| macOS native execution of this change | **NOT RUN** (Linux x64 only) |
| Decode / PSNR / visual quality | **NOT CLAIMED** |

The 8K blocker is separate and named: `ValidateFilmScan3Geometry` caps the
pixel budget at 8,294,400 px, which is exactly 4K. Raising it is a memory and
allocation decision, not a quality-profile one, and is not part of this change.

## The rate-control hysteresis, and a correction

Low/Medium/High each choose between their own table and the midpoint table
between them and the profile below. Which one applies depends on a session
token: when the previous frame used the overrate table and the bitrate stayed
between 80 % and 100 % of the ceiling, the choice is *held*, not re-decided.
Passing a constant `previous_overbitrate = 0` makes that hold unreachable, so a
session that entered the overrate table would drop straight back out as soon as
the bitrate fell under the ceiling.

That token is now session state (`FilmScan3State::fs_overbitrate`), fed through
`ProfileTableRequest`, returned in `ProfileTables`, committed with the rest of
the rate state and cleared by `Reset()`.

It is worth being precise about what is and is not proven. This corpus **never
enters the 80–100 % hold band** — every overrate frame in the pinned packets is
above the ceiling, so the corpus alone cannot distinguish the two behaviours.
An earlier revision of this document described that as a gap while claiming the
six profiles were done; that was too weak a statement, and the measured rows
above did not support a claim either way.

What closes it is a forced trajectory in `profile_enter_contract` (5c):
enter → hold → leave, driven through the same seam the encoder calls, compared
against a control that passes 0. At `hold`, the shared table differs between the
two runs for medium and high; for low the midpoint table coincides with the
profile's own table, so the token is observable in the state but not in the
bytes, and the test asserts exactly that rather than pretending a difference
exists. Reverting the feed to `0` fails the suite with *"hysteresis did not hold
inside the 80..100% band"*.

So: the state is implemented and the branch is proven live. Real-packet parity
inside the hold band still has no oracle, because the corpus does not contain
that trajectory. It is not claimed.
