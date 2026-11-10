# Independent review — RCA-160 Windows cross-host band-0 evidence (#158 / PR #192)

Status: **VERDICT_REPRODUCED / EVIDENCE_BOUNDED / TWO_DISCLOSURE_GAPS / GATE_NOT_PORTABLE**.
Read-only review of the Linux A2 lane. No decoder change, no `upstream/` edit, no
new hardware run, no merge or release. The #120 gate stays
`REFERENCE_DECODER_NONDETERMINISTIC / UNRESOLVED`.

Reviewed artefact: `tests/reference_decode/band0_1080.py` (protocol
`rca160-windows-cross-host-band0-20261019-v1`), the sanitized per-call ledger,
`docs/validation/RCA160_WINDOWS_CROSS_HOST.md`, `apply_rca160_gate.py`,
`rca160_probe.h`, all at the exact head `a6a22ad1f1e46eab128f47d28a5ee25061f16f06`.

## What was done

The submission's summary block was not trusted. A stdlib-only verifier
(`tools/verify_rca160_cross_host.py`) recomputes every arm from the 360 raw
per-call records: modal digest per arm, corrupt set, premature-entry set,
gate-entry counts, wait-to-publication matching, timeouts, condition uniformity,
execution order, and the registered verdict rules in the registered order. The
verifier carries its own four-case self-test, because a checker that cannot say
"INCONCLUSIVE" is not a check; the self-test is registered in CTest as
`rca160_cross_host_verifier_selftest`.

## Result 1 — the submitted verdict is reproducible

| quantity | submitted | recomputed from ledger |
| --- | --- | --- |
| O corrupt | 4/120 | **4/120** (calls 94, 103, 108, 112) |
| B corrupt | 0/120 | **0/120**, one output digest |
| A′ corrupt | 1/120 | **1/120** (call 37) |
| modal digest, all arms | identical | **identical**, `38d1c149…c686` |
| gate entries per call | 9/9 | **9 in every call of every arm** |
| B waits matched to a same-`IMAGE` band-0 publication | 60/60 | **60/60** |
| B gate timeouts | 0 | **0** |
| condition uniform (cpu_max 4, HALF, BGRA, 960×540, pitch 3840, 2,073,600 B, 0 padding) | stated | **confirmed across all 360 records** |
| execution order | O → B → A′ | **O → B → A′**, sequential |

Verdict re-derived by the registered rules: `CROSS_HOST_CONFIRMATION_PASS`, with
no reason list. The submission and this review agree on the mechanics.

## Result 2 — the contrast is real but the rate is not

Zero corrupt outputs in B is compatible with the observed control rate.

| comparison | Fisher exact (two-sided) |
| --- | --- |
| O 4/120 vs B 0/120 | p = 0.122 |
| pooled gate-off 5/240 vs B 0/120 | p = 0.174 |
| A′ 1/120 vs B 0/120 | p = 1.000 |

At the pooled gate-off rate of 2.1 %, `P(0 corrupt in 120) = 0.080`. So "the gate
eliminated the corruption" is **not** supported as a rate effect, and the
submission does not claim one. What the evidence does support is a conjunction:
in both gate-off arms, *every* corrupt call had a premature entry, and in arm B
*every* one of the 60 waits completed on a `BAND_PUBLISH band=0` for the same
`IMAGE` pointer with band 0 transitioning invalid→valid inside the window, while
no call waited where band 0 was already valid. That conjunction is the real
finding; the 4-versus-0 count is its weakest part.

A′ is a **single** event. Had A′ returned 0/120, the same binary and the same
mechanism would have been declared `INCONCLUSIVE` by the registered rule. The
registered rule was therefore satisfied on a one-event margin, and that margin
should be carried in any downstream statement.

## Result 3 — two gaps that the acceptance text does not report

**(a) A registered coverage rule was never enforced.** The pre-registration for
this run required "9 gate entries **and 6 band-0 publications**" per call. The
protocol defines `EXPECTED_PUBLISH_BAND0 = 6` and its unit test uses it, but the
verdict function only checks the entry count. Observed band-0 publications per
call are **9–12**, in all three arms.

The constant is a derivation error, not a measurement surprise: band 0 is
published once per channel by the entropy lowpass decode (3), once per channel
at each of the two spatial reconstruction sites (6), and once for each of the two
frame wavelets after the temporal transform (2) — median 11, which matches the
observation exactly. Had the registered rule been executed as written, the run
would have returned `INCONCLUSIVE`. It is a correct measurement against a
mis-derived constant that was then dropped silently; the pre-registration text
should be corrected in the open, not left as an unenforced line.

**(b) The corrupt frames are not attributed to a channel.** The Linux fourth
pass identified the corrupt plane as a chroma lowpass (ch1 in that sample,
ch2 in others). The Windows ledger records one frame digest per call and no
per-channel plane digest, so this review cannot say *which* channel was wrong in
the four O calls or the one A′ call.

This matters because the two hosts waited on different channels: Linux waited
only on **ch1/index1** (22 of 120 calls), Windows waited on **ch0/index1 (55)**
and **ch2/index1 (5)**. It is entirely possible that the Windows gate blocked a
different corrupt state than the one Linux localised. "The same admission window
exists on Windows" is supported. "The same corruption was removed on Windows" is
**not** supported by this ledger, and PR #192's wording should not imply it.

## Result 4 — source review of the gate as a candidate mechanism

The gate is correctly placed and correctly scoped for a diagnostic:

* it replaces the two spatial lowpass predicates only, at `decoder.c` inside the
  `transform_type == TRANSFORM_TYPE_SPATIAL && index > 0` and `index > 3`
  branches — verified by applying the published applier to a scratch tree and
  reading the enclosing branches, not by trusting the comment;
* it is reached for all three channels, which is consistent with the observed
  9 entries per call;
* gate-off executes exactly the original `BANDS_ALL_VALID` predicate;
* transform math, the writer and scheduling are untouched.

Three properties block any production reading, and all three are visible in the
published header, not in a measurement:

1. **The wait is not synchronised.** `RCA160Band0Gate` reads
   `band_valid_flags`/`band_started_flags` in a polling loop with no lock,
   acquire or atomic. The writer, `UpdateWaveletBandValidFlags`, publishes the
   same two words **under `decoder->entropy_worker_new.lock`**. This is the same
   unsynchronised pair TSan already reported in #159; the gate adds a reader
   that waits on it, not an ordering guarantee. A pass under it does not
   demonstrate a portable happens-before relation.
2. **Progress is not guaranteed.** The publish the gate waits for is produced by
   another queue entry's reconstruction, and those entries are drained only by a
   worker that has just finished decoding a band (`entropy_threading.c`, the
   `do { … } while (wavelet && BANDS_ALL_VALID(wavelet))` drain). A worker
   spinning in the gate cannot drain. Liveness therefore depends on *some other
   worker still having a band to decode*. If none does, the gate cannot see its
   publication and must burn the full bound. On this run that never happened
   (max wait 3 ms, 42 of 60 waits under 1 ms), which is why the pass is credible
   — but it is a structural property of the intervention, not a property of the
   host, and it is exactly the shape that would turn a rare corruption into a
   rare multi-second stall followed by a discarded frame.
3. **Two sites are ungated.** The temporal branch (`index == 2`) still guards
   readiness with `assert(BANDS_ALL_VALID(temporal))`, compiled out under
   `NDEBUG`, and the `index == 3` highpass branch keeps the vacuous predicate.
   The frame wavelets the HALF and FULL output paths read get their band 0 from
   the temporal transform. A readiness race there is neither covered by the gate
   nor excluded by this run.

Bounded-failure semantics are fail-closed in the right direction: on timeout the
gate returns 0, the caller sets `CODEC_ERROR_BAD_FRAME` and returns, and both
frame consumers test `decoder->error` after the reconstruction and clear the
frame, so a timeout becomes an explicit failure rather than a corrupt OKAY frame.
Worst case per decode is 9 × 5,000 ms = 45 s, against the harness's 60 s child
bound; that headroom exists only because the harness bound was raised for this
run, and it would not exist in an application that expects ~20 ms decodes.

`FULL` resolution was **not** exercised on either host. The same two sites run
on the FULL path, so the gate would be reached, but nothing here measures it, and
the Windows route probe only confirms the HALF `CopyLowpass16sToBuffer` path.

## Result 5 — reproducibility of the published patch hash

The published patched `decoder.c` hash is reproducible exactly, and it is
reproducible *only* on a CRLF tree:

| variant | SHA-256 | matches published |
| --- | --- | --- |
| applied to the frozen CRLF tree | `ebf03108…363f09` | **yes** |
| same patch on an LF tree | `cdef2e9a…3cf205` | no |

Cause: `apply_rca160_gate.py` rewrites with `Path.write_text()`, which follows
`os.linesep`. Content is identical either way. This is not a defect in the
evidence — it is a positive provenance signal that the applier ran on the frozen
tree as shipped — but a downstream lane that re-applies the patch on Linux and
compares the hash will conclude "wrong patch". Compare content, not this hash.

All three published helper hashes (protocol, applier, probe header) match the
files at the reviewed head.

## What this review does not establish

* No new decode was run. Every statement above is either a recomputation from
  the published ledger or a source reading of the frozen tree.
* No channel attribution, no FULL resolution, no macOS or Linux re-measurement.
* No statement about the Windows `4/45` result in #116, which uses a different
  condition.
* No claim that the gate is a fix, or that the underlying defect is repaired.

## One next action

The Windows owner should append, to PR #192, (a) the per-channel output-plane
digests for the five corrupt calls, so the removed state can be compared with
the Linux ch1/ch2 states, and (b) an explicit correction of the
`EXPECTED_PUBLISH_BAND0` constant with the rule restated and re-evaluated
against the existing ledger. Until (a) exists, cross-host confirmation should be
stated as "the same admission window", never "the same corruption".

For any future production decision, the gate as published is not a candidate:
it needs a real completion signal under the existing lock (not a poll), it must
cover the temporal and highpass sites, and it must be evaluated with more than
one gate-off event per arm.
