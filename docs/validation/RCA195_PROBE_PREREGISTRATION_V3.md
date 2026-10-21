# PRE-REGISTRATION v3 — RCA-195 silent-drop probe (reproducible witness)

Supersedes v2 (`00b5748e…`). v2's **outcome is not withdrawn**: its control arm
and its induced arms were re-scored here from the retained probe logs and the
result stands. What v2 could not do is reproduce **from its own committed
artefacts**, and that is a defect in the publication, not in the decoder.

Written and hashed BEFORE the v3 run. The v3 run is a fresh set of arms on a
fresh private SDK copy with the corrected instrument; it is not a re-label of
v2's records.

## Why v3 exists

Four defects, each found by re-deriving v2's ledger rather than reading it:

1. **The committed ledger does not reproduce from the committed runner.** Every
   record in `rca195_probe_ledger_2026-10-20.json` is *flat*
   (`claimed`/`executed`/`skipped` at top level). `run_rca195_probe.py` at the
   same head writes a *nested* `queue` object and no flat mirror. Feeding the
   committed records to the committed `classify()` returns `NATURAL_VARIANT` for
   all 120 induced calls — i.e. `INCONCLUSIVE`. A verdict that cannot be
   re-derived from its published inputs is not evidence.

2. **Two published "measurements" were literals.** `apply_rca195_probe.py`
   emitted `RCA195LogQueueReset(…, 0, 0)`, so `admitted` and `unadmitted` were
   constants in every call, and `unadmitted=0` was reported as a finding about
   the decoder. `admitted` is now read from `next_entry`, `unadmitted` from
   `num_entries`, and the applier self-test rejects a return of the literal.

3. **The registered `SKIP`-match criterion was never enforced.** v2 §"Verdict
   rules" §4 required that the dropped call's log show the matching `SKIP`
   line. `run_rca195_probe.py` never checked it. It is now rule R4, and the
   ledger records `skip_target_matched` per call.

4. **`guard_ok` was vacuously true.** The harness allocated exactly `pitch*height`
   bytes and then scanned `[pitch*height, size)` for untouched bytes — an empty
   range. The column was 1/1 in all 150 calls for arithmetic reasons, not
   because a guard band survived. The buffer now carries 64 KiB of real slack
   and the record states `guard_bytes` and `guard_first_dirty`.

Additionally: `RCA195_WITHHOLD` had **no call site in any translation unit**. It
was dead instrumentation presented as an absent-producer capability. It is
removed. The absent-producer case is covered by the skip arms, which is what v2
actually ran.

## Fixed inputs (unchanged)

Pinned 900396-byte packet
`2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52`, private SDK
copy byte-identical to the frozen tree before patching
(`decoder.c acd477cb…`, `entropy_threading.c 32033d23…`), HALF/BGRA 8-bit,
`TAG_CPU_MAX=4`, one decode per fresh process, `taskset -c 0`, `upstream/`
untouched.

## Arms (unchanged)

| Arm | `RCA195_SKIP` | Meaning |
| --- | --- | --- |
| A | none | control |
| B | `1:2` | temporal producer of `wavelet[1]` band 0 |
| C | `0:2` | luma channel, same producer |
| D | `2:2` | second chroma channel, same producer |
| E | `1:1` | the consumer site #160 measured |

30 calls per arm, 150 total, fixed order.

## Verdict rules, in this order

`FALSE_OKAY_DEMONSTRATED` requires **all** of:

- **R1 coverage** — every record carries a queue-reset line. A record without
  one is `NO_QUEUE_RECORD` and cannot be scored.
- **R2 attribution** — the control arm has zero calls that drop a claimed
  transform *and* zero calls whose queue is discarded with `num_entries > 0`.
- **R3 mode** — ≥ 25/30 control calls share one digest.
- **R4 witness + record** — at least one induced call has SDK OKAY, a digest
  differing from the control mode, `claimed > executed`, and a `SKIP` line for
  **its own** arm target. A dropped call without its own `SKIP` line does not
  count, and an arm with hits but no matched `SKIP` line is a failure reason.
- **R5 instrument integrity** — no induced arm logs a `SKIP` for an entry other
  than its own target.

Otherwise `INCONCLUSIVE` with the reason list.

A call whose queue is discarded with an entry that was **never admitted**
(`num_entries > 0`) is classified `UNMITTED_DISCARD_FALSE_OKAY` when the digest
differs. That is the production state the maintainer review describes, and it is
scored separately and more severely than an induced claim-skip.

The harness and the runner both carry a `--self-test`/`--rescore`. The runner's
self-test must return `INCONCLUSIVE` for seven synthetic cases (control drop,
scattered control, no `SKIP` line, stray `SKIP`, missing queue line, flat-witness
record, SDK error). `--rescore` must reproduce a ledger's verdict from its own
records; it is run against the committed v2 ledger in CI and is expected to
**pass on the v3 ledger and fail on the v2 one**.

## Explicit limitation, unchanged from v2

Natural control calls and induced calls reach identical byte states
(`fad4a643…` appears in both). A digest match is corroboration only; the witness
is `claimed > executed` plus the matched `SKIP` line.

## What this cannot show

One packet, HALF only, Linux gcc Release, one machine, 30 calls per arm. It shows
the state is reachable and that the public API reports success in it. It does not
show how often the unmodified decoder reaches it unaided, and it proposes no fix.
