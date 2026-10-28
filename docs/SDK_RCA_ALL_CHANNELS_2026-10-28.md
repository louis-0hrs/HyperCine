# Issue #160: all-channel publication RCA

Copyright (c) 2026 HyperCine Contributors. Dual-licensed under MIT OR Apache-2.0.

The recorded JSON evidence is now supplied for public audit. See
[the publication inventory and SDK-free audit commands](evidence/README_RCA160.md).
Private SDK instrumentation, raw snapshots and media are excluded.

**Verdict: ROOT_CAUSE_CONFIRMED for the measured 1080p spatial Phase2A packet,
FULL and HALF, channels 0/Y, 1/V and 2/U.** This is a bounded causal result,
not a production SDK fix, universal decoder qualification or historical failure-rate estimate.

## Previous evidence and new scope

The earlier [96-call report](SDK_RCA_2026-10-28.md) was recorded reproducibly in
[#160](https://github.com/louis-0hrs/HyperCine/issues/160#issuecomment-6472740284)
before this extension. It demonstrated a channel1 dependency, leaving channel0
and channel2 corruption in instrumented FULL calls. The new experiment targets
each channel separately, then all three, and orders consumers after **actual
publication**, rather than merely after coefficient computation.

Input remains the 900396-byte retained guard-smoke packet with SHA-256
`fc046231223e537bb221524b2d6f9c0d9f24a71ea23f79deb140f44f7bd063c8`.
Identity with historical #160/#206 media is not established.

## Isolation and replay

All experiment sources, generated SDK translation unit, binaries and raw planes
are in ignored private `build-rca-all/`. `setup.py` reads the immutable original
`upstream/Codec/decoder.c` and creates `decoder_private.c` with three hooks:
before the spatial inverse, after its computation, and after the original
`UpdateWaveletBandValidFlags(decoder, lowpass, 0)` has returned. The executable
links this private TU and the existing CPU-only SDK static archive. No production
target, versioned upstream file or original arithmetic was altered.

```powershell
python build-rca-all/setup.py
cmake -S build-rca-all -B build-rca-all/bin
cmake --build build-rca-all/bin --config Release
python build-rca-all/run.py
python build-rca-all/verify.py
```

These historical experiment commands require the private harness, a fresh
directory and the verified retained packet. The private setup/worker/probe
scripts and raw snapshots are not distributed in this evidence publication;
their source hashes remain in the plan. Repository-only review uses the offline
audit linked above. Decoder replay still requires separately authorized access
to those exact private artifacts; no substitute packet is implied.

## Preregistered small matrix and synchronization

Seven arms Ã— two fresh processes Ã— FULL/HALF = **28 decodes**. Arms are serial
CPU_MAX=1, induce Y, induce V, induce U, induce all, publish all, and reinduce all.
Multithreaded arms use CPU_MAX=8; caller seed is `0x1337BEEF`. Output is aligned,
pitch padded, prefilled with 0/165, with a 4096-byte guard. Process timeout is
30 seconds; each event wait is bounded at four seconds. No retry-to-green.

The selected channel mask is 1, 2, 4 or 7. For each selected channel:

```
INDUCE:
  producer index2: wait(consumed[channel]); perform original inverse; publish
  consumer index1: snapshot input; perform original inverse;
                   snapshot output; signal(consumed[channel])

PUBLISH ALL:
  producer index2: wait(attempted[channel]); perform original inverse;
                   original UpdateWaveletBandValidFlags; signal(published[channel])
  consumer index1: signal(attempted[channel]); wait(published[channel]);
                   snapshot input; perform original inverse
```

The publication event is signalled only after the SDK's original locked flag
update and a snapshot of that published plane. This establishes a deliberate
producerâ†’publicationâ†’consumer ordering. It is a diagnostic scheduling
intervention, not a replacement for the SDK queue/admission logic. Successful
bounded waits do not prove universal liveness.

Each snapshot saves the visible signed-16 band0 plane without pitch padding.
The consumer's input allocation is matched internally with the same channel's
index2 published allocation; raw addresses are not exported. Event IDs capture
hook reservation order. All selected induced cases require the stronger witness
`consumer input < consumer output < producer input < producer publication`.
Thus the producer remains held throughout the inverse, not merely at the first
observation. HALF sometimes executes an index2 transform more than once; all
captured producer results are checked, not a presumed fixed publication count.

## First differing coefficients

These are the first **row-major signed-16 coefficients**, not claims of an
instruction-by-instruction first load. Both resolutions and both repetitions
give the following values in the separately targeted channel and all-channel
induction arms:

| Channel | Consumer input wavelet1 band0 size | First input difference | First computed wavelet0 band0 difference |
|---|---|---|---|
| 0 / Y | 480Ã—270 | (0,0), offset 0: expected 2700, actual 0 | (0,0): expected 2696, actual âˆ’10 |
| 1 / V | 240Ã—270 | (0,0), offset 0: expected 1063, actual 0 | (0,0): expected 1066, actual 6 |
| 2 / U | 240Ã—270 | (0,0), offset 0: expected 709, actual 0 | (0,0): expected 708, actual 0 |

Channel order is corroborated in `Codec/convert.c:4793,5221` (2=U, 1=V).
No poison or zero-fill was injected into these input planes. The observed zero
contents are ordinary allocation contents before the required producer write;
they are not a valid reconstruction. The exact provenance of every stale heap
coefficient remains unmeasured.

## Results and causal interpretation

For each resolution separately:

| Arm | Pixel deviations | Targeted channel's computed plane |
|---|---:|---|
| Serial controls | 0/2 | all captured planes equal |
| Induce Y | 2/2 | Y wrong 2/2 |
| Induce V | 2/2 | V wrong 2/2 |
| Induce U | 2/2 | U wrong 2/2 |
| Induce all | 2/2 | Y/V/U each wrong 2/2 |
| Publish all | 0/2 | every captured coefficient plane equal |
| Reinduce all | 2/2 | Y/V/U each wrong again 2/2 |

All **28/28** calls returned SDK OK, retained guards, and completed every
instrumented wait without timeout. Across all calls, every captured index2
input band0 and computed output band0 matched the serial reference. The first
localized divergence is the dependent index1 input; the producer subsequently
computes the correct coefficients but cannot undo the already wrong inverse.
In the all-publication arm every saved coefficient snapshot and final BGRA
byte matches the serial control (four calls across FULL/HALF).

Untargeted channels can enter with absent band0, then catch up before actual
arithmetic reads. Some snapshots therefore differ although the computed plane
is correct. Such snapshots alone are not causal proof. Only the targeted
held-producer witnesses support the read-before-write conclusion. This also
explains why a readiness-window hit need not always cause visible corruption.

The deterministic verifier checks complete arm inventory, API/guard/wait status,
parent plane identity, all captured index2 results, missing-band masks, ordering
witnesses, targeted output errors, and coefficient/pixel recovery. Four mutated
ledgers (missing call, timeout, wrong first coefficient and absent publication)
are rejected. Its result is recorded as `ROOT_CAUSE_CONFIRMED`.

## Exact original SDK defect

The dependency is the same for each channel:

`ReconstructWaveletBand(ch,index2)` â†’ write `wavelet[1].band[0]` â†’
`UpdateWaveletBandValidFlags(...,0)` â†’ `ReconstructWaveletBand(ch,index1)`.

`Codec/image.h:230` defines `BANDS_ALL_VALID` as equality of valid and started
masks. Both can be `0xE`: highpasses complete, lowpass neither started nor valid.
The worker admits work under that predicate (`entropy_threading.c:269â€“285`);
the spatial reconstruction repeats it (`decoder.c:13032â€“13044`) and consumes
band0 without requiring its producer. Measured scale 2 dispatches through
`wavelet.c:5756` to `InvertSpatialQuantDescale16s`. Its top-row read at
`spatial.c:22528` evaluates `11*lowlow[column + 0*lowlow_pitch]`, followed by
the next two rows; SIMD interior reads consume the same allocation. These
values can be old or partially written. Flag equality does not establish
producer completion or a happens-before relationship for coefficient reads.

The original publication follows the parent write (`decoder.c:13050`);
`UpdateWaveletBandValidFlags` updates both masks under the entropy lock
(`decoder.c:24073â€“24104`). Waiting for this actual publication prevents the
measured failures in Y, V and U. Merely clearing output, reseeding RNGs or waiting
after an invalid inverse cannot repair that dependency. Incorrect computed
planes precede color packing/dither. `AllTransformBandsValid` checks pointers
rather than band completion; a final worker wait cannot repair prior arithmetic,
and public API success was observed even for deliberately wrong output.

## Remaining uncertainties and boundaries

The exact 28 calls confirm the cause on the measured spatial packet/routes.
They do not prove the exact interleaving of every earlier spontaneous channel0/2
failure, cover all formats/GOP2/temporal paths, estimate spontaneous incidence,
qualify the SDK pixel oracle, or prove a safe general synchronization repair.
Duplicate HALF transforms and the separate claimed-work/silent-drop exposure
remain relevant to a broader SDK redesign. Disk snapshots perturb scheduling;
no performance claim is made. Raw snapshots remain private and are not uploaded.
No broad CTest/GPU sweep was needed for this private diagnostic-only extension.
No commit, merge or production fix was made; `git status --porcelain upstream/`
is empty.
