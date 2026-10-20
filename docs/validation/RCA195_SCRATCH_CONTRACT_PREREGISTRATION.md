# PRE-REGISTRATION — RCA-195 M2 scratch-SDK contract candidate (R1/R3/R4/R5/R6)

Written and hashed **before** the run. This is the one narrow private
scratch-copy experiment the fourth review gate allows after the rev-6 verdict
fix. It is **not** a production patch, **not** a fix for #120/#195, and it does
not touch `upstream/`.

## Fixed inputs

| Item | SHA-256 |
| --- | --- |
| Pinned packet `pinned.cfhd` (900 396 bytes, 1920×1080) | `2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52` |
| Frozen `upstream/Codec/decoder.c` (pre-apply) | `acd477cb72bd314cda2d088e5a37fb956810e2f27ec893eb6272abc33fc511a6` |
| Frozen `upstream/Codec/entropy_threading.c` (pre-apply) | `32033d23498f33ee11d4e032ad46b90a426c3458fb3c4e9a01efab49d5da9874` |
| `tools/apply_rca195_contract.py` | `1112dbe84750b53abb2bfce64af2996c60f682187bcc1990342afe179dc10726` |
| `tools/rca195_contract.h` | `6efb60bfa071ac1a288ae7485d05d328695bc44c7cf30ba68d8270e2a11343d1` |
| `tools/rca195_probe_harness.cpp` (unchanged from M1 v3) | `a773df435eff9fe4dde06d90332fa430d17f2fc77f128f46b6fcf5e4bfacda88` |

Scratch copy: a private out-of-repo SDK tree whose `Codec/` was replaced by a
**byte-identical copy of the frozen tree** before patching (verified with
`diff -r`), then instrumented by the applier. Post-apply hashes:

| Instrumented file | SHA-256 |
| --- | --- |
| `Codec/decoder.c` | `dd86ed57c302241e69c8f7d20cebc8ed3e35b0036f2b9548613c1b29a60fce56` |
| `Codec/entropy_threading.c` | `6099f77c1e8c20e95029b07057c82656189aae0a0116c959dedfd02f787e07a7` |
| `Codec/rca195_contract.h` | `6efb60bfa071ac1a288ae7485d05d328695bc44c7cf30ba68d8270e2a11343d1` |

## Premise measured before the candidate was written

The R6 check asserts that a valid decode drains completely. That was
**measured**, not assumed, on the unpatched M1 probe copy (same packet,
`pinned.cfhd`):

| Decode | Workers | `num_entries` at drain | claimed / executed / admitted |
| --- | --- | --- | --- |
| FULL | 2 / 4 | 0 / 0 | 6/6/6 |
| HALF | 2 / 4 | 0 / 0 | 9/9/9 |
| QUARTER | 2 / 4 | 0 / 0 | 3/3/3 |

A valid decode therefore reaches the drain with an empty queue and
`claims == executed`. Without that measurement, "drain with open work is an
error" would have been a guess that a legitimate HALF/QUARTER decode could
falsify.

## What "generation" maps onto

The M2 document speaks of a generation re-check (R3/R5). The frozen decoder has
**no generation counter**: `grep -rni generation Codec/*.c` returns only two
unrelated comments about multi-generation *encodes*. The modelled generation
therefore maps onto **wavelet pointer identity**:

- `GetWaveletThreadSafe` → `ReallocWaveletEx` (`wavelet.c:870`) returns the
  existing `IMAGE` only when width, height, level and `memory` match; otherwise
  it calls `DeleteImage` and `CreateWaveletEx`, which clears
  `band_valid_flags` and `band_started_flags`.
- A queue entry claimed before that reallocation still holds the old pointer.
  Executing onto it publishes into an object the decoder no longer owns.

So R3/R5 becomes: *the wavelet pointer captured at claim time must still be
`transform->wavelet[index]` at execution time.* That is implementable against
the frozen source and is what the candidate checks.

This is a **mapping the design has to declare**, not a decoder feature that was
discovered. A future contract that wants a true generation counter would have to
add one, which is a production change and outside this experiment.

## Contract candidate — exactly four hooks

| Rule | Site | Behaviour |
| --- | --- | --- |
| R1 | claim admission (unchanged control) | `BANDS_ALL_VALID(wavelet)` still gates the claim; no new admission rule is added, so R1 is measured only as "the candidate does not weaken it" |
| R4 | `decoder.c:13233` | `assert(BANDS_ALL_VALID(temporal))` → real check setting `CODEC_ERROR_BAD_FRAME` and returning. Release compiled the assert out; a temporal wavelet missing a band then reached `TransformInverseTemporalQuant`, whose band guards return silently, and the caller still reported OKAY |
| R3/R5 | `entropy_threading.c` claim→execute window | pointer captured at claim, re-checked before execution; mismatch is terminal, sets `CODEC_ERROR_BAD_FRAME`, does not execute |
| R6 | `decoder.c` `WaitForTransformThread` | before zeroing: `num_entries != 0` or `claims != executed` → `DRAIN_SHORT`, sets `CODEC_ERROR_BAD_FRAME` |

The skip switch (`RCA195_CONTRACT_SKIP`) and the stale switch
(`RCA195_CONTRACT_STALE`) are **failure injection**, present so each fail-closed
path is measured rather than asserted unreachable. Neither is a fix.

## Arms (fixed order, fixed targets, no reshuffling)

Five calls per arm, `cpu_max` ∈ {2, 4}, resolution FULL and HALF.

| Arm | Injection | What it must show |
| --- | --- | --- |
| A | none | valid decode: SDK OKAY, `claims == executed`, `DRAIN_SHORT == 0`, digest equals the M1 control digest |
| B | `SKIP 1:2` (temporal producer of frame wavelet 1 band 0) | claimed-but-unexecuted entry ⇒ **non-OKAY** (`CODEC_ERROR_BAD_FRAME`) via R6, no hang |
| C | `SKIP 0:1` (spatial producer, luma) | same, on a different producer site |
| D | `STALE 1:2` | R3/R5 fires ⇒ **non-OKAY**, `STALE` counter 1, entry not executed |
| E | `SKIP 1:1` (the consumer site #160 measured) | same closure on a non-producing index |

## Registered verdict rules (fixed now, evaluated after)

- **R-A** Arm A must be `sdk_ok == 1`, `drain_short == 0`,
  `claims == executed`, in **every** call at 2 and 4 workers. One failure ⇒
  `CONTROL_BROKEN`, and the experiment is void — not "the candidate works
  anyway".
- **R-B** Every injected call must return `sdk_ok == 0`. A single OKAY in B–E ⇒
  `FALSE_OKAY_SURVIVES`.
- **R-C** Every injected call must log the hook that closed it (`DRAIN_SHORT` in
  B/C/E, `STALE` in D). An injected non-OKAY without its hook is scored
  `UNATTRIBUTED_FAILURE`, not a pass.
- **R-D** No call may hang. Per-call timeout 120 s; a timeout is `HANG` and is
  a failure of the candidate, because R7 forbids waiting inside the drain.
- **R-E** The guard band (64 KiB slack) must be untouched in every call. A dirty
  guard is `MEMORY_CORRUPTION` and voids the experiment.
- **R-F** The candidate may not change valid-decode output: arm A digests must
  equal the M1 control digest `38d1c1498d3a990a141b622b8758a49fd1b634aa8042ef402d219fc9ee68c686`
  (HALF) at 2 and 4 workers.

Verdicts: `CONTRACT_CANDIDATE_QUALIFIED` (all rules hold),
`CONTRACT_CANDIDATE_REJECTED` (R-A/R-E/R-F broken), `FALSE_OKAY_SURVIVES`
(R-B broken), `UNATTRIBUTED` (R-C broken).

## What this cannot establish

- No memory ordering. The pointer re-check is not a C11/C++ acquire/release
  argument; it is a value comparison under the same lock discipline the decoder
  already uses.
- No timing or liveness proof. Five calls per arm at two worker counts is a
  bound, not an exhaustive schedule.
- No FULL/HALF *correctness* claim beyond digest equality on one packet.
- **Not** a production patch, **not** a fix, **not** authorisation to merge.
  `upstream/` stays byte-identical; `#120` stays
  `REFERENCE_DECODER_NONDETERMINISTIC / UNRESOLVED`.

## Reproduction

```
tools/apply_rca195_contract.py --self-test
tools/apply_rca195_contract.py <private-copy-root>
cmake -S <copy> -B <copy>/build -DCMAKE_BUILD_TYPE=Release && cmake --build <copy>/build -j
tools/run_rca195_contract.py --copy <copy> --packet <pinned.cfhd> --out <dir>
```

The runner refuses to write into the repository, and the applier refuses to
patch a tree that looks like the frozen `upstream/` or the repository root.
