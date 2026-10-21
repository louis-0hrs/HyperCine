# ADDENDUM — RCA-195 M2 scratch contract, iteration 3 (precedence, ABA probe, second configuration)

Iteration 3 answers the three points of the 2026-10-21 review. Like iteration 2,
it is a declared follow-up rather than a blind pre-registration: it was designed
after the review named the defects, and the defects are restated here before the
fix so the sequence stays auditable.

Predecessors, unchanged:

| Iteration | Outcome | Artefact |
| --- | --- | --- |
| 1 | `FALSE_OKAY_SURVIVES` (published, negative) | `rca195_contract_ledger_iter1_2026-10-20.json` |
| 2 | `CONTRACT_CANDIDATE_QUALIFIED`, one change (`R6b`) | `rca195_contract_ledger_2026-10-20.json` |

## 1. The review's first point was a real defect, not a documentation gap

Iteration 2's guard was

```c
if (error != CODEC_ERROR_OKAY || decoder->error == CODEC_ERROR_OKAY)
    decoder->error = error;
```

It stopped OKAY from erasing a failure. It was **not** first-error semantics:
with two non-OKAY values the later one replaced the earlier one, so the reported
code depended on arrival order. The reviewer said exactly this and was right.

R6b is now a macro with the intended rule:

| Field state | Incoming | Result |
| --- | --- | --- |
| OKAY | OKAY | OKAY |
| OKAY | failure | failure recorded |
| failure | OKAY | unchanged — OKAY never displaces |
| failure A | same failure A | unchanged, no conflict |
| failure A | different failure B | **A kept**, precedence conflict counted |

The conflicting case is *counted and logged* (`PRECEDENCE kept=… dropped=…`)
rather than silently resolved, so the error hierarchy is measured instead of
asserted.

### The regression the review asked for, and its mutation test

`tests/test_rca195_error_precedence.cpp` exercises the macro against the real
`CODEC_ERROR` enum, with `static_assert` pins on the ordinals (`OKAY 0`,
`RESOLUTION 36`, `BAD_FRAME 58`) so an upstream renumbering fails loudly instead
of quietly turning the assertions into aliases. Six cases, including order
independence in both directions.

A regression that has never failed proves nothing, so it was mutated: restoring
iteration 2's last-wins behaviour makes it fail in **four** of six cases —

```
FAIL earlier failure survives a later one: got 36, want 58
FAIL precedence conflict is counted: got 0, want 1
FAIL first failure wins in order A then B: got 36, want 58
FAIL first failure wins in order B then A: got 58, want 36
```

The header was restored afterwards and re-verified.

## 2. The review's second point: ABA, measured rather than caveated

R3/R5 compares `transform->wavelet[index]` against the pointer captured at
claim time. That cannot distinguish a genuine reallocation from the allocator
returning the same address for a new object. The review required that this
limitation stay explicit and not be promoted to a generation counter. It is now
also *measured*: every wavelet free and every allocation is recorded, and a
returned address matching a recently freed one is counted as `ABA_REUSE`.

### The measurement first had to be made trustworthy

The first ABA run logged **3950** realloc entries and **zero** free/alloc lines.
That is the shape of dead instrumentation — a counter reading zero because
nothing reaches it looks identical to "no reuse was observed". Two causes were
found and both were real:

1. **The hooks sat in the wrong preprocessor branch.** The build defines
   `-D_ALLOCATOR=1`, and the free/alloc sites are `#if _ALLOCATOR / #else /
   #endif` pairs. The hooks were inserted next to the concrete statement, i.e.
   inside one branch, and were compiled away. They now sit after the closing
   `#endif`, verified by preprocessing the file with `-D_ALLOCATOR=1` and with
   `-D_ALLOCATOR=0` (both keep the hooks).
2. **The log was not thread-safe.** Several workers share one `FILE*`. One call
   showed 38 realloc entries against 28 early returns + 9 allocations — exactly
   one event missing. A lost line and a genuine zero are indistinguishable, so
   logging is now done under `flockfile`/`funlockfile`.

Both defects were found by measuring the instrument, not the decoder.

### Reachability, so a zero can be read correctly

Three counters separate the possibilities:

| Hook | Meaning |
| --- | --- |
| `WREALLOC_ENTER` | `ReallocWaveletEx` was called |
| `WREALLOC_EARLY` | it returned the existing wavelet unchanged |
| `WFREE` / `WALLOC` | the delete-and-recreate branch was reached |

### Per-call self-accounting

Every call writes a `RESULT` summary carrying all counters. The runner compares
each counter against the number of log lines that counter should have produced
(rule **R-G**). A disagreement is scored `UNATTRIBUTED` — an instrument failure,
not a measurement. This exists specifically so "WFREE = 0" cannot be published
unless the instrument agrees with itself.

Across the 100-call run the invariants hold with **zero** violations:

```
enter = 3950    nil-allocations = 900    early returns = 3050    frees = 0
entered and not early-returned = 3050    delete branch reached  =    0
```

## 3. Result — iteration 3

| Run | Calls | Verdict | Control | Injected |
| --- | --- | --- | --- | --- |
| Primary, packet `2b9a28de…` | 100 | `CONTRACT_CANDIDATE_QUALIFIED` | 20/20 OKAY, 1 digest per resolution | 80/80 non-OKAY, each with its closing hook |
| Second configuration, packet `34fcbcfac1a3792f…` | 60 | `CONTRACT_CANDIDATE_QUALIFIED` | 12/12 OKAY, 1 digest per resolution | 48/48 non-OKAY, each with its closing hook |

No hangs, guard band untouched in all 160 calls, no unattributed failures, no
instrument disagreements. The second run used a **different packet** (a 4:2:2
canon sample, not the pinned FilmScan packet) with `--control-digest` omitted, so
R-F fell back to requiring internal consistency of the control arm — a new
packet legitimately has its own valid digest, and pinning every run to the first
packet's value would have failed R-F for the wrong reason.

## 4. The finding that outranks the pass

**The delete-and-recreate branch never executed in 160 calls.**

`ReallocWaveletEx` was entered 3950 + 2370 = 6320 times. In every single case it
either received `NULL` (allocating fresh) or matched the existing wavelet on
width/height/level/memory and returned it unchanged. The `DeleteImage` +
`CreateWaveletEx` path — the one that actually invalidates a stale pointer — was
never taken.

Two consequences, and they cut in opposite directions:

1. **The R3/R5 mechanism was not exercised by a real reallocation.** What the
   stale arm proves is that *if* the token changes, the check refuses to execute.
   It does **not** prove that the decoder ever reallocates a wavelet inside the
   claim→execute window on these inputs. The forced-stale arm demonstrates the
   guard, not the hazard.
2. **That makes the ABA caveat narrower *and* sharper.** Address reuse is only
   possible once the delete branch runs; on these 160 calls it never did. So
   `aba = 0` is not "no reuse was possible" — it is "the situation in which reuse
   could occur did not arise". A packet or resolution that forces a size change
   through `ReallocWaveletEx` could reach it, and no run here covers that case.

I am not claiming the hazard is absent. I am claiming it was not reached, and
that the earlier framing ("pointer identity detects a changed pointer") glossed
over the fact that this decoder, on this input, never changes it.

## 5. What iteration 3 does and does not establish

Established, within scope:

- R6b keeps the **first** non-OKAY code, with a mutation-tested regression and a
  measured conflict counter.
- The contract separates valid from dropped on two packets, two worker counts,
  two resolutions — 160 calls — and the separation reaches the public API.
- The instrument accounts for itself call by call; a lost log line can no longer
  masquerade as a clean zero.

Not established:

- **No memory ordering.** The pointer comparison is a plain read under the
  existing lock discipline, not a C++ acquire/release argument. The ABA counter
  measures address reuse only within a 16-entry recent-free window; it is a
  probe, not a proof.
- **No liveness or timing.** Two worker counts and a handful of calls are a
  bound.
- **The reallocation path is unexercised.** No claim about a live reallocation
  hazard, in either direction.
- **No HALF/FULL correctness** beyond digest equality of the control arm.
- **Not a production patch**, not a merge, not a release, and not authorisation
  for a decoder fix. `upstream/` stays byte-identical; `#120` stays
  `REFERENCE_DECODER_NONDETERMINISTIC / UNRESOLVED`.

## 6. Artefacts

| Item | SHA-256 |
| --- | --- |
| `rca195_contract_ledger_iter3_2026-10-21.json` | `4a19c3c2439ae61686eec785f9f08a527bc1469036f35ac38a93347d416917aa` |
| `rca195_contract_ledger_iter3_second_config_2026-10-21.json` | `b468e3ec71f9cfd0c55eaaf388a5dd82fb10342ec5923ae74b56df5e007bbe21` |
| `tools/rca195_contract.h` | `aa276ee9c9d4ef1b6249ec89a464796c83810b18fc0a2f322eb0462ea2983a1d` |
| `tools/apply_rca195_contract.py` | `c24d78266b7aae77b7fabcc19ffe3fac90d7703fc6ef9a914139bcb678c610c0` |
| `tools/run_rca195_contract.py` | `3f68c7640a820f536bea31a9ab2eb03a327600982b1c184167c39b2a70b6654b` |
| `tests/test_rca195_error_precedence.cpp` | `b43accf5dd7897eaa334a86c8f015886722a1dd8b9cc828761d57411452d3795` |

Both ledgers are committed as produced; neither is regenerated at publication
time. The scratch copy was verified byte-identical to the frozen `Codec/` before
patching on every iteration.

## 7. Post-publication correction: the header was rebuilt for MSVC

The first push of iteration 3 (`2e162fd`) failed the hosted Windows MSVC job.
The cause was mine: the counters used `__atomic_fetch_add`, a GCC/Clang builtin
that MSVC does not have, and the new precedence test includes the contract
header, so ten uses of an undeclared identifier reached the Windows build. That
diagnosis belongs in this document rather than only in a CI log.

The fix makes the increment portable (`InterlockedIncrement64` under `_MSC_VER`,
the builtin everywhere else). Because the header is part of the **instrument**,
both ledgers were regenerated with the corrected header rather than left
carrying hashes for a header that no longer exists. The regeneration is a
declared replacement, not a silent swap:

| | Before (`2e162fd`) | After |
| --- | --- | --- |
| `tools/rca195_contract.h` | `715c757c…` | `aa276ee9…` |
| Primary ledger | `c8270054…` | `4a19c3c2…` |
| Second-configuration ledger | `1bfed58e…` | `b468e3ec…` |

Every verdict and every counter is **bit-identical** across the two runs —
100 calls and 60 calls, same verdicts, same `claims`/`executed`/`stale`/
`drain_short`/`walloc`/`wfree`/`aba` totals. So the change repaired portability
and did not alter behaviour. That equality is the check that makes the swap
legitimate; without it, a replacement of published artefacts would be exactly
the defect this project already found once in v2.
