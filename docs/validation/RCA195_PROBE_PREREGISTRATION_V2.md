# PRE-REGISTRATION v2 — RCA-195 silent-drop probe (corrected witness)

Supersedes v1 (`3001720e…`). v1's verdict rule was **unfulfillable by my own
construction** and is recorded, not relocated: see "Why v1 was inconclusive".

Written and hashed BEFORE the v2 confirmatory run.

## Why v1 was inconclusive

v1 required `num_entries > 0` at the queue reset. The injection hook consumes the
entry exactly as the production code does (`done = 1`, `num_entries--`) and only
then skips execution, so `num_entries` is legitimately 0. The rule could never
fire. Correcting it is a protocol change, not a re-label — the v1 ledger stands as
`INCONCLUSIVE` and remains in the record.

## The corrected witness

`claimed > executed`, counted inside the worker translation unit:

- `CLAIM` is logged immediately after `done = 1; num_entries--;`
- `EXEC` is logged immediately before `ReconstructWaveletBand(...)`
- both counters live in one definition (`RCA195_PROBE_OWNER`), so they cannot
  disagree through a header copy — the defect that produced `skipped=0` in the
  first draft.

A call with `claimed > executed` has taken a transform out of the queue and never
performed it. That is the "claimed but not executed" state the review names.

## Fixed inputs

Unchanged from v1: pinned 900396-byte packet
`2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52`, private SDK
copy byte-identical to the frozen tree before patching, HALF/BGRA 8-bit,
`TAG_CPU_MAX=4`, one decode per fresh process, `taskset -c 0`, `upstream/` untouched.

## Arms

| Arm | `RCA195_SKIP` | Meaning |
| --- | --- | --- |
| A | none | control |
| B | `1:2` | temporal producer of `wavelet[1]` band 0 |
| C | `0:2` | luma channel, same producer |
| D | `2:2` | second chroma channel, same producer |
| E | `1:1` | the consumer site #160 measured |

30 calls per arm, 150 total, fixed order.

## Verdict rules

`FALSE_OKAY_DEMONSTRATED` requires all of:

1. control arm has **zero** calls with `claimed > executed`;
2. control arm is deterministic in the sense that ≥ 25/30 calls share one digest
   (the known #160 nondeterminism is expected and reported, not hidden);
3. at least one induced arm has a call with `claimed > executed`, `sdk_ok = 1`,
   and a digest differing from the modal control digest;
4. that call's probe log shows the matching `SKIP` line.

Otherwise `INCONCLUSIVE` with the reason list.

## Explicit limitation, registered before the run

Natural control calls and induced calls are **not distinguishable by digest
alone**: the natural #160 race reaches the same byte state as dropping the
`ch1/index2` transform. This was observed in the exploratory run (control call
`n30`/`n33` matched the induced `fad4a643…` digest with `claimed = executed = 9`).
Therefore a digest match is reported as corroboration only; the witness is the
counter, not the hash. Any write-up that says "identical bytes prove the drop"
would be overclaiming, and this registration forbids it.

## What this cannot show

One packet, HALF only, Linux gcc Release, one machine, 30 calls per arm. It shows
the state is reachable and that the public API reports success in it. It does not
show how often the production decoder reaches that state on its own, and it
proposes no fix.
