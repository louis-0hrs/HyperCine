# ADDENDUM — RCA-195 M2 scratch contract, iteration 2 (R6b no-clobber)

**This is not a blind pre-registration.** Iteration 1 was registered, run and
scored before this document existed. Its result is negative and is published
unchanged. Iteration 2 was designed **after** reading that result and the
diagnostic that explains it. It is declared here so the sequence is auditable:
hypothesis → run → negative result → diagnosis → narrower hypothesis.

Iteration-1 artefacts:

| Item | SHA-256 |
| --- | --- |
| `tools/apply_rca195_contract.py` (iter 1) | `1112dbe84750b53abb2bfce64af2996c60f682187bcc1990342afe179dc10726` |
| `tools/rca195_contract.h` (iter 1) | `6efb60bfa071ac1a288ae7485d05d328695bc44c7cf30ba68d8270e2a11343d1` |
| `tools/run_rca195_contract.py` (iter 1) | `4302931d1c7117d6edb2afc2ba379380dee5be63b63931c1df11135a64b1bf76` |
| Iteration-1 ledger | `ef462647b7160ea82ec70ab97b21d65df32e98789d1b19958aadb999d9d28bc7` |

## Iteration 1 result — `FALSE_OKAY_SURVIVES`, and it is real

100 calls: 5 arms × {FULL, HALF} × {2, 4} workers × 5 calls.

| Arm | Calls | `sdk_ok` | `DRAIN_SHORT` | `STALE` lines | claims / execs |
| --- | --- | --- | --- | --- | --- |
| A control | 20 | 20 | 0 | 0 | 150 / 150 |
| B `SKIP 1:2` | 20 | **20** | 20 | 0 | 150 / 120 |
| C `SKIP 0:1` | 20 | **20** | 20 | 0 | 150 / 130 |
| D `STALE 1:2` | 20 | **20** | 20 | 29 | 150 / 120 |
| E `SKIP 1:1` | 20 | **20** | 20 | 0 | 150 / 130 |

The control is clean in all 20 calls: `num_entries = 0`, `claims == executed`,
no `DRAIN_SHORT`, unchanged digests. Every injection is detected internally —
`DRAIN_SHORT` fires in all 80 injected calls, and the forced-stale arm logs 29
`STALE` lines — and **the public API still returns `CFHD_ERROR_OKAY` in all
80**. So R6 as written does not reach the caller.

## Diagnosis — measured, not inferred

A diagnostic build (not part of the candidate) logged every
`decoder->error = CODEC_ERROR_OKAY;`, every `decoder->error = error;`, the drain,
and the return of `DecodeSample`:

```
DRAIN ptr=0x… entries=0 claimed=6 executed=5 err=0
DRAIN_SHORT remaining=0 claimed=6 executed=5
ASSIGN_ERR at=13816
CHECK ptr=0x… err=0
RETURN result=1 error=0
```

`ReconstructSampleFrameToBuffer` (`decoder.c:13402`) declares its **own**
`CODEC_ERROR error = CODEC_ERROR_OKAY;` and ends with

```c
// Save the codec error code in the decoder state and return
decoder->error = error;
return;
```

The drain runs at `decoder.c:13425`, *before* that assignment. So the value the
drain recorded is overwritten one statement later by a local that was never
told about it. `DecodeSample` then reads `decoder->error == OKAY` and keeps
`result = true`.

This is the same defect class the M2 document already names — a failure that is
computed and then discarded — one layer below the drain. It is **not** a
measurement artefact: the diagnostic was rebuilt from a clean tree after an
earlier attempt silently reused a stale CMake cache, and the stale-cache run was
discarded rather than reported.

## Iteration 2 — one change, declared in advance

| Item | Iteration 2 |
| --- | --- |
| Change | R6b: the final `decoder->error = error;` becomes `if (error != CODEC_ERROR_OKAY \|\| decoder->error == CODEC_ERROR_OKAY) decoder->error = error;` |
| Everything else | identical — same packet, same arms, same worker counts, same resolutions, same 5 calls per combination |
| Registered expectation | every injected call returns `sdk_ok == 0`, with `DRAIN_SHORT` (B/C/E) or `STALE` (D) present; arm A unchanged (`sdk_ok == 1`, `claims == executed`, digest equal) |

Current hashes for iteration 2:

| Item | SHA-256 |
| --- | --- |
| `tools/apply_rca195_contract.py` | `c97136cd8f14c1a41f8c3ebc59245083440c9cbabbf6d420e49cef9316293300` |
| `tools/rca195_contract.h` | `6efb60bfa071ac1a288ae7485d05d328695bc44c7cf30ba68d8270e2a11343d1` |
| `tools/run_rca195_contract.py` | `4302931d1c7117d6edb2afc2ba379380dee5be63b63931c1df11135a64b1bf76` |

Verdict rules are unchanged from the iteration-1 registration. R-A/R-E/R-F can
still void the experiment; R-B still means "the candidate did not stop it";
R-C still means "it failed for a reason the candidate does not name".

## What iteration 2 still cannot establish

- No memory ordering, no timing, no liveness. Two worker counts and five calls
  are a bound.
- The no-clobber guard is a **candidate**, not a fix. It changes what the public
  API reports; it does not make the decode correct, and it does not establish
  that `CODEC_ERROR_BAD_FRAME` is the right code for a lost transform.
- If iteration 2 qualifies, the honest claim is: *this contract, in this narrow
  shape, separates valid from dropped on one packet at 2 and 4 workers.* It is
  not a production patch, not a merge, and not authorisation for one.
- `upstream/` stays byte-identical. `#120` stays
  `REFERENCE_DECODER_NONDETERMINISTIC / UNRESOLVED`.
