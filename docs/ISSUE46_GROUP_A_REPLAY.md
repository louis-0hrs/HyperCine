<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Issue #46: four 120-frame sequences, 480 packets, replayed

Status: **480/480 packets match on every observable band table**.
Not a conformance claim, and not a claim about the SDK's internal rate state —
that axis is not observable in this corpus.

Verifier: `tools/verify_issue46_group_a.py`, opt-in CTest
`issue46_group_a_packet_tables` (`-DHYPERCINE_ISSUE46_GROUP_A_CORPUS=<dir>`).
Fail-closed: a missing file, a wrong byte length, a wrong SHA-256, a wrong frame
count, a noncontiguous/reordered frame index, a broken previous-packet byte
chain, a missing or duplicate high-pass subband, an absent quant/scale tag,
a truncated probe replay, a probe failure or an unparsable probe line all abort
with a non-zero exit. There is no skip path. The corpus-free adversarial
contract `tests/test_issue46_group_a_verifier.py` is registered as
`issue46_group_a_verifier_contract` in quality-state-only CTest, including on
hosts without the private corpus. This contract tests rejection logic; the
private 480-packet comparison remains a separate opt-in acceptance gate.

## What was compared

Corpus: the four Windows original-SDK sequences for Low, Medium, High and
FilmScan1, 120 frames each, 1920x1080, GOP 1, YUV422 p10. All **606** files
(480 packets + 120 inputs + manifest + 5 provenance files) were size- and
SHA-256-checked against the transfer index before any value was read; every
profile's `previous_sdk_sample_bytes` also matches the previous packet's
`byte_length`, so the chain through the sequence is intact.

The comparison is against `packet_band_quant_scale`, i.e. the quant (tag 53) and
scale (tag 54) pairs the **SDK itself wrote into each packet**. That is an
observable artifact, not a re-derivation of our own state — 3 channels × 9
high-pass subbands × 2 fields per frame.

| Profile | Frames | Divergent frames | Divergent quant entries | Divergent scale entries |
| :--- | ---: | ---: | ---: | ---: |
| low | 120 | **0** | 0 | 0 |
| medium | 120 | **0** | 0 | 0 |
| high | 120 | **0** | 0 | 0 |
| filmscan1 | 120 | **0** | 0 | 0 |
| **total** | **480** | **0** | **0** | **0** |

## Why the ladder is justified, measured rather than quoted

The same replay was run against a probe built from the pre-#46
`src/core/quality_state.cpp` (`a42f700`, the flat gate):

| Profile | Current | Pre-#46 flat gate |
| :--- | ---: | ---: |
| low | 0 | 0 |
| medium | 0 | **42** |
| high | 0 | **42** |
| filmscan1 | 0 | 0 |
| **total** | **0** | **84** |

So "42 of 120 frames of Medium and High" is reproducible from the corpus, and
the ladder removes exactly those 84 divergent frames. Low and FilmScan1 are
unaffected by the change in both directions, which is the expected outcome: the
overbitrate counter is not part of their path.

A mutation check confirms the verifier is not merely decorative: pointing it at
the pre-#46 probe produces
`FAIL: 84 of 480 frames diverge from the packet tables` and exit code 1.
Pointing it at an incomplete corpus produces
`FAIL: transfer index not found: …/TRANSFER_SHA256.tsv`.

## The limit, stated plainly

`sdk_rate_limiter` and `sdk_overbitrate` are recorded as `NOT_MEASURED` and
`SDK_INTERNAL_STATE_NOT_OBSERVABLE` for all 480 frames. This corpus therefore
**cannot** confirm the SDK's internal counters, and the replay carries the rate
state forward from our own model: it is open loop in the state and closed loop
only on the tables the packet exposes.

Two consequences worth stating rather than glossing over:

1. Matching 480/480 is strong evidence that the tables the ladder selects are
   the tables the SDK wrote. It is not evidence that our counter sequence equals
   the SDK's; a wrong state that happens to select the right table for these four
   sequences would pass here. The directly observed `0,2,3,2,1,1,0` window comes
   from the separate seven-frame state capture and is tested in
   `tests/test_overbitrate_hysteresis.cpp`, not here.
2. Filmscan2/FilmScan3 are not in this corpus, so nothing is claimed for them.

## Reproduction

```
cmake -S . -B build-q -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DHYPERCINE_BUILD_QUALITY_STATE_ONLY=ON -DHYPERCINE_BUILD_TESTS=ON \
  -DHYPERCINE_ISSUE46_GROUP_A_CORPUS=<group-a-sequences>
cmake --build build-q
ctest --test-dir build-q -R issue46_group_a --output-on-failure
```

The corpus is not vendored: it is 6 GB of original-SDK output and stays outside
the repository. Without the cache variable the test is simply not registered,
and CI stays green for contributors who do not have the corpus.
