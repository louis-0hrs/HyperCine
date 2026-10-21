# PRE-REGISTRATION — RCA-195 Milestone-1 silent-drop probe

Written and hashed BEFORE the confirmatory run. Any later change is a new protocol.

## Question

Does a decode that leaves a transform entry never admitted — `num_entries > 0` at
the queue reset — return public `CFHD_ERROR_OKAY` while producing output that
differs from the control mode?

This is deliberately narrower than "the decoder is wrong". The decoder already has
a pre-existing nondeterminism (#160: about one call in five), so a differing digest
alone proves nothing. Only the conjunction counts.

## Fixed inputs

- Packet: the pinned 900396-byte YUV422 GOP1 sample, SHA-256
  `2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52`.
- Private SDK copy at a scratch path outside the repository, byte-identical to the
  frozen tree before patching (`decoder.c` `acd477cb…`, `entropy_threading.c`
  `32033d23…`), patched only by `tools/apply_rca195_probe.py`.
- Probe header `tools/rca195_probe.h`, harness `tools/rca195_probe_harness.cpp`,
  runner `tools/run_rca195_probe.py` — exact SHAs recorded in the ledger.
- Resolution HALF, BGRA 8-bit, `TAG_CPU_MAX=4`, one decode per fresh process,
  pinned to one CPU with `taskset -c 0`. `upstream/` untouched.

## Arms (fixed order, fixed targets)

| Arm | `RCA195_SKIP` | Meaning |
| --- | --- | --- |
| A | none | control: no injection |
| B | `1:2` | temporal producer of `wavelet[1]` band 0 |
| C | `0:2` | same producer on the luma channel |
| D | `2:2` | same producer on the second chroma channel |
| E | `1:1` | the consumer site #160 measured |

30 calls per arm, 150 total. The skip hook consumes the entry exactly as the real
code does (`done = 1`, `num_entries--`) and then does not execute the
reconstruction. It is failure injection, never a candidate fix.

## Classification, in this order

1. `HANG` — no record within the 60 s per-call bound.
2. `NO_OUTPUT` — SDK returned a non-OKAY error.
3. `DROPPED_FALSE_OKAY` — SDK OKAY, `num_entries > 0` at the reset, and the digest
   differs from the modal control digest.
4. `DROPPED_IDENTICAL` — SDK OKAY and `num_entries > 0`, digest equal.
5. `INDUCED_SKIP` — SDK OKAY, a skip fired, queue drained.
6. `NATURAL_VARIANT` — SDK OKAY, digest differs, nothing dropped.
7. `OK` — SDK OKAY, digest equals the modal control digest.

## Verdict rules

`FALSE_OKAY_DEMONSTRATED` requires all of:

- the control arm shows **zero** calls with `num_entries > 0` at the reset, so the
  effect is attributable to the induction;
- at least one call in arms B–E is classified `DROPPED_FALSE_OKAY`;
- that call's probe log records the skip and the reset on the same invocation.

Otherwise the verdict is `INCONCLUSIVE` with the reason list printed. The
`NATURAL_VARIANT` rate of the control arm is reported separately and is **not**
counted as evidence — it is the known #160 behaviour.

## What this cannot show

One packet, HALF only, one machine, Linux gcc Release. No FULL arm, no Windows
replay, no claim about other formats. A pass here means the silent-drop exposure is
real for this input; it does not size the rate in the wild and does not propose a
fix.
