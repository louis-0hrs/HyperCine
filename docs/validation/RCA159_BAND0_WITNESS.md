# RCA-159 band-0 publish-before-inverse witness

Status: **recorded and machine-checked, mechanism measured, experiment unresolved.**
No decoder patch, no guard proof, no release-gate change.

This file is the committed, sanitized form of the witness measured in
[#159](https://github.com/louis-0hrs/HyperCine/issues/159) /
[Draft #169](https://github.com/louis-0hrs/HyperCine/pull/169), which
[#234](https://github.com/louis-0hrs/HyperCine/issues/234) requires before any
guard is coded. The numbers below are transcribed from that record; nothing here
was re-executed. The private instrumented copy, the packet bytes and the host
identity stay outside Git.

Machine-readable companions:

- `docs/validation/rca159_band0_witness_2026-10-25.json` — the witness, withdrawn
  hypothesis, rate arms and hash domains.
- `docs/validation/rca159_band0_guard_preregistration_2026-10-25.json` — the
  pre-registered A/B for the unpublished guard, with `executed: false`.
- `tools/rca159_witness_contract.py` — validator. Recomputes the published
  Fisher-exact statistics from the contingency tables, requires the withdrawn
  claim to remain contradicted by its own observations, and refuses any host
  path in either record. Exits 1 on failure.
- CTest `test_rca159_witness_contract` runs the same contract, plus negative
  controls that tamper with a copy and must fail.

## Mechanism

At the reader, the original decoder reached

```
band_valid_flags   = 0x0E
band_started_flags = 0x0E
gate_all_valid     = true
inverse_executed   = true
```

i.e. all three high bands were valid and started, band 0 was neither, and the
inverse ran anyway. The gate compares `valid == started`, and the companion
all-started predicate excludes band 0, so a band that never started and never
validated satisfies both. Direct source loci, unchanged and frozen:
`upstream/Codec/image.h:227-231`, `upstream/Codec/decoder.c:12106-12148`,
`upstream/Codec/entropy_threading.c:269-305`.

Six reader calls occur per decode. That number is per invocation inside the
decode loop; it is **not** a channel count, which is what the withdrawn
hypothesis below was built on.

## Witness correspondence

60 fresh processes, eight-worker cap, single-CPU affinity, buffered in-memory
recorder dumped once at exit, six reader calls per process:

| population | `band0_valid == 0` observed | never observed |
| --- | ---: | ---: |
| divergent (4 frames) | **4** | 0 |
| modal (56 frames) | 0 | **56** |

Both arms cover the identical file set, so this is an exact correspondence, not
a sampled one. Fisher exact, two-sided, recomputed from the table:
**p = 2.1 × 10⁻⁶** (2.049 × 10⁻⁶ before rounding).

## Rate arms, and why only one may be quoted

| arm | divergent / processes | quotable as a rate |
| --- | ---: | --- |
| uninstrumented (3 batches pooled, ratio 12:1) | 27 / 120 = 22.5% | **yes** |
| `fprintf` recorder | 0 / 30 | no |
| buffered recorder | 4 / 60 = 6.7% | no |

Buffered versus uninstrumented, Fisher two-sided from `[[4, 56], [27, 93]]`:
**p = 0.007**. The instrumentation suppresses incidence by roughly a factor of
three, so a traced rate is not the defect's frequency. The direction of the
perturbation is what makes the correspondence usable: suppression cannot
manufacture a `band0_valid == 0` observation in a frame that comes out modal, and
no divergent frame was observed with `band0_valid == 1` throughout.

Ratio control: 0/60 divergent at a worker-to-CPU ratio of one or below, against
roughly 19–23% at 12:1 on this host.

## Withdrawn hypothesis

**Withdrawn:** "the number of early-reader events in a decode predicts how many
chroma channels lost their own race." One event maps to two different plane
sets, and two different counts map to the same set:

| frame | early-accept events | B | G | R | planes moved |
| --- | ---: | ---: | ---: | ---: | --- |
| `p09` | 2 | 100.0% | 99.9% | 100.0% | B, G, R |
| `p21` | 1 | 0.0% | 99.4% | 100.0% | G, R |
| `p25` | 1 | 100.0% | 100.0% | 100.0% | B, G, R |
| `p27` | 1 | 0.0% | 99.4% | 100.0% | G, R |

`p21` and `p27` are byte-identical to each other and differ from `p25` at the
same event count. Percentages are full-frame channel comparisons against the
modal reference, no sampling.

What survives is the set correspondence above: 4/4 divergent frames emitted the
`0x0E` state with the gate open, 0/56 modal frames did at any of their six
reader calls.

## Buffer provenance

Sentinel-filling the probe buffer before decode (`0x7F7F`, 32639) flips the
unread prefix from near `0x00` to near `0xFF` while the modal digest is
unchanged. The consumed prefix follows the allocation contents, so the producer
never wrote it.

## Hash domains

The `#159` field `modal_frame_sha256` was the **sentinel-extended** domain, not
the image-only region. Both digests are recorded so the two are not conflated:

| domain | bytes | sha256 |
| --- | ---: | --- |
| image only, 1920×1080, pitch 7680 | 8,294,400 | `c5e0e052be84a807a899d0501fa73f93bbe9d67e0f1c9e5cfdef97a3ffee54a0` |
| image + 64 sentinel bytes `0xCD` | 8,294,464 | `12ed7b2b8d1b06d87fa693db6ac72474ece13632c2d8ffe6981a52f52e1480fd` |

These are transcribed, not recomputed: this record does not own the private
output and does not claim a rehash.

## What this record does not claim

- No production decoder was repaired, and no guard exists or is proven.
- The scheduling interleaving that produces `0x0E` at that instant is **not**
  identified. A lock-scope trace would perturb harder than the recorder did.
- Traced rates are not defect frequencies.
- No pixel oracle is qualified, no `#120` or P7 gate changes, and no cross-host
  reproduction is asserted.
- `upstream/` is unmodified.

## Remaining work, blocked on this host

Steps 2, 4 and 5 of `#234` — the minimum causal patch in a disposable private
source copy, the fail-closed controls and negative mutation, and the bounded A/B
execution with its verdict — are `BLOCKED_HARDWARE_AND_PRIVATE_CORPUS` here and
belong to the Linux original-SDK RCA owner. The pre-registration file fixes the
arms, controls, invariants, deadline and verdict vocabulary before that run, so
the outcome cannot be redefined afterwards.
