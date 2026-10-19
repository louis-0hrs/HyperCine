# RCA-195 — band-0 publication contract, Milestone 1 (`DESIGN_ONLY`)

Source-anchored dependency map, per-topology required-bit matrix and liveness
statement for the original GoPro SDK reference decoder. **No decoder change, no
decode run, no `upstream/` edit.** This pass answers the "why" left open by the
bounded A/B/A′ evidence in #160 / Draft #180 and by the Windows cross-host
recheck in Draft #192; it does not extend their measurement claims.

Everything below is re-derived from the frozen tree by
`tools/verify_rca195_band0_contract.py`, which fails closed if any anchor moves.
Run it before citing this document.

## 1. Verdict

| Item | Status |
| --- | --- |
| Causal admission window on HALF 1080p YUV422 | `DEMONSTRATED` (Linux 25/120 → 0/120; Windows 4/120 → 0/120) |
| Root mechanism (vacuous readiness predicate) | `SOURCE_CONFIRMED` — this pass |
| Per-topology required bits | `SOURCE_DERIVED`, and **insufficient** as specified — see §4 |
| Production-safe contract | `NOT_DESIGNED_YET` — requirements in §6 |
| Portable patch | `NONE`. The published spin gate is not a candidate |

`DESIGN_ONLY` is the honest status: the mechanism is named and bounded, the fix
is not.

## 2. The predicate is vacuous, and the strict form was deliberately dropped

`upstream/Codec/image.h:229-230`, both lines still present:

```c
//#define BANDS_ALL_VALID(wavelet)  ((wavelet)->band_valid_flags == (uint32_t)((1 << (wavelet)->num_bands) - 1))
#define BANDS_ALL_VALID(wavelet)    ((wavelet)->band_valid_flags == (wavelet)->band_started_flags)
```

The released form compares two flag words to each other. A band that is
**neither started nor valid** contributes the same bit to both sides, so it
cancels out. `UpdateWaveletBandStartedFlags` raises a started bit *before* the
coefficients exist, which is what makes the state reachable:

| Wavelet | `num_bands` | Vacuous state that passes the released predicate |
| --- | --- | --- |
| spatial / frame | 4 | `valid = started = 0xE` — band 0 absent |
| temporal | 2 | `valid = started = 0x2` — band 0 absent |

`0xE` is exactly the entry state recorded in every corrupt call on both hosts.
The strict form (`valid == (1<<num_bands)-1`) would have rejected it; it survives
only as a comment, so the release build has no equality-to-all-bands check left.

This also explains why the earlier #163 "strict validity" attempt behaved
differently: it was a different predicate in a different place, not a weaker or
stronger version of this one.

## 3. Producers and consumers, per channel and band

### 3.1 Writers

| Symbol | Line | Sets | Lock discipline in the shipped config |
| --- | --- | --- | --- |
| `UpdateWaveletBandValidFlags` | `decoder.c:24073` | `valid\|bit` **and** `started\|bit` | takes `entropy_worker_new.lock`, guarded by `_THREADED_DECODER` and a live pool — **compiled in** |
| `UpdateWaveletBandStartedFlags` | `decoder.c:24114` | `started\|bit` only | lock block guarded by `#if _DELAYED_THREAD_START==0`; `config.h` sets `_DELAYED_THREAD_START` to 1 — **compiled out** |

The two words are therefore written by two paths with **different lock
discipline**. `band_started_flags` is updated with no synchronisation at all in
the default build, while `band_valid_flags` is updated under the entropy worker
lock. A reader that compares the pair without the lock cannot obtain a
consistent snapshot of it: it can observe a started word that is newer than the
valid word it is compared against, which is precisely the `0xE` state.

`UpdateWaveletBandValidFlags` is called from at least five sites (entropy
decoders, both lowpass inversions, the highpass inversion and the temporal
inversion), so the valid bit for a given band can be published from a worker
thread or from the caller thread.

### 3.2 Consumers inside `ReconstructWaveletBand`

Three live runtime guards survive, all of them the released vacuous predicate:

| Line | Branch | Publishes |
| --- | --- | --- |
| `decoder.c:13030` | spatial lowpass, `index > 0` | band 0 of `wavelet[index-1]` |
| `decoder.c:13095` | field-plus lowpass, `index > 3` | band 0 of the lower wavelet |
| `decoder.c:13155` | spatial highpass, `index == 3` | highpass band 1 |

Two further sites have **no runtime check**:

* `decoder.c:13233` — temporal branch, `assert(BANDS_ALL_VALID(temporal));`
  only. `NDEBUG` removes it, so a release build inverts the temporal transform
  whenever the queue admits it, with no readiness test at all.
* The published #160 gate patched only the two **lowpass** sites. The highpass
  site at `:13155` is still gated by the vacuous predicate, and the temporal
  branch by nothing.

Both frame wavelets that the HALF/FULL output reads get their band 0 from the
temporal inversion (`decoder.c:13253-13254`), so the unguarded temporal path is
upstream of the pixels that were actually measured. That exposure is
**source-level only** — it was not measured, on either host, in this campaign.

### 3.3 The queue drain, which is where the race is decided

`upstream/Codec/entropy_threading.c`, inside `EntropyWorkerThreadProc`:

```c
Lock(...);
for (curr_entry = 0; curr_entry < data->free_entry; curr_entry++) {
    ...
    if (data->queue[curr_entry].done == 0 && BANDS_ALL_VALID(wavelet)) {  /* :269 */
        data->queue[curr_entry].done = 1;                                 /* admitted, not finished */
        ...
    }
}
Unlock(...);                                                              /* :283 */
if (wavelet && BANDS_ALL_VALID(wavelet))                                  /* :285 unlocked re-read */
    ReconstructWaveletBand(..., &local, 0);                               /* :302 */
} while (wavelet && BANDS_ALL_VALID(wavelet));                            /* :305 unlocked re-read */
```

So: the **admission decision is taken under the lock**, the **transform runs
outside it**, and admission is re-tested twice outside the lock with the same
vacuous predicate against a word that another path may be writing lock-free.

`done = 1` marks *claimed*, not *published*. A consumer can be admitted while
its producer is still executing, and the producer's band-0 write lands either
before or after the consumer's first read of that plane. That window — not a
deterministic switch — is what the 25/120 vs 30/120 (Linux) and 4/120 vs 1/120
(Windows) counts measure, and it is why some calls with a premature entry still
produced correct output.

## 4. Required-bit matrix — the matrix exists, and it is not enough

`DecodedBandsValid` (`decoder.c:24136`) already encodes the per-topology answer,
using `num_bands` (spatial 4, temporal 2, frame 4) and a per-type mask. It is
**dead code**: zero live callers, two call sites commented out (`:11322`,
`:11721`).

Re-derived from that function (bit 0 = lowpass):

| Transform | `index` | `num_bands` | Required mask | Bands that must be published |
| --- | --- | --- | --- | --- |
| spatial | 1 | 4 | `0xE` | 1, 2, 3 |
| spatial | 2 | 4 | `0xF` | 0, 1, 2, 3 |
| field-plus | 2 | 2 | *true* (no wait) | earlier entries compute both |
| field-plus | 3 | 4 | `0xF` | 0, 1, 2, 3 |
| field-plus | 4 | 4 | `0xE` | 1, 2, 3 |
| field-plus | 5 | 4 | `0xF` | 0, 1, 2, 3 |

**This matrix does not fix the observed defect.** Read row 1: for a spatial
wavelet at `index == 1`, band 0 is *deliberately exempted*
(`threaded_band_mask = BAND_VALID_MASK(0)`), because band 0 is not expected to
come from entropy decoding — it is expected to have been computed by the
adjacent transform. That exemption is an assumption about **ordering**, expressed
as if it were a statement about **bits**.

Consequences, both of which argue against the mechanical `0xF` rewrite the issue
forbids:

1. Replacing `valid == started` with `valid == 0xF` on the `index == 1`
   consumer would deadlock: band 0 is produced by a different queue entry, and
   waiting for entropy to deliver it never terminates.
2. Reaching for `DecodedBandsValid` is equally insufficient: it would admit
   `index == 1` with band 0 still absent, because that is exactly what it
   exempts.

The correct precondition is not a set of bits but a **producer relation**:
"band 0 of `wavelet[i]` has been published by the queue entry that owns
`index == i+1`, in this generation, under the same lock". Which bits are
required is then a *consequence* of that relation, not a substitute for it.

The same reasoning gives the per-channel answer for free: the relation is
per-`(channel, index, generation)` tuple. The measured evidence waited on
`ch1/index1` on Linux and `ch0/index1` + `ch2/index1` on Windows. That is the
same relation instantiated on different channels, which is consistent — but the
Windows ledger records one frame digest per call and no per-channel plane
digest, so "the same admission window" is supported and "the same corruption was
removed" is not.

## 5. Liveness and forward progress

The drain loop and the transform invocation live in **the same worker
function**. A consumer that blocks inside `ReconstructWaveletBand` is a worker
that cannot drain the queue entry it is waiting for. That is not a theoretical
concern:

* the pool is created only when `cpus > 1` and is capped at **4 threads**;
* a blocked worker therefore removes up to a quarter of the drain capacity;
* admission is `done = 1` before the work runs, so "admitted" entries are
  invisible to other workers as pending work while still being incomplete.

The published spin gate has a bound (50 M spins / 5 000 ms) and correctly turns
a timeout into `CODEC_ERROR_BAD_FRAME`, so it fails closed rather than hanging.
But the bound is reachable *by construction*: nothing in the design guarantees
that a worker able to publish is still idle. On the measured runs a publisher
was always available — max wait 3 ms, 42 of 60 waits under 1 ms — which is a
property of those runs, not of the protocol. Worst case, with four workers all
blocked on entries whose producers sit behind them in the queue, the failure is
nine timeouts per decode against the harness's 60 s child bound.

Therefore, for any contract that is allowed near production:

* blocking must not hold the only publication path — either the waiter must be
  able to make progress itself, or the queue must not let it block while a
  producer is runnable;
* impossibility must be detected from queue state, not from repeated timeouts;
* a refused admission must surface as an explicit bounded error, never as
  `CFHD_ERROR_OKAY` with wrong pixels.

## 6. What a contract has to state (design requirements, not a patch)

Derived from §2–§5, in the order the issue asks for. Nothing here is implemented.

1. **A single publication point per band.** One writer raises *both* words while
   holding one lock, so no reader can observe the pair torn. The current split
   between `UpdateWaveletBandValidFlags` and `UpdateWaveletBandStartedFlags`,
   with the second one's lock compiled out, has to collapse.
2. **A readiness predicate that is not self-referential.** It must be derived
   from `num_bands` and from *which bands are expected from entropy* versus
   *which are expected from the transform chain* — i.e. the matrix in §4 used as
   an input, with the exempted band's producer named explicitly instead of being
   silently dropped.
3. **A generation/epoch per wavelet.** Flags are cleared per group
   (`ClearTransformFlags`, `decoder.c:1370`); a wait must be tied to the
   generation it was entered for, or a recycled wavelet can satisfy it with a
   stale bit.
4. **Memory ordering.** Either a mutex/condition variable shared with the
   producer, or an explicitly proven release/acquire pair. Plain `volatile`
   polling, bounded spin loops, and unlocked reads of words written under a lock
   are all excluded by the issue and by §3.1.
5. **Forward progress.** See §5: blocking is only admissible if the blocking
   thread (or another guaranteed runnable one) still publishes the required
   band.
6. **Error semantics.** First error wins, the frame is cleared, and the caller
   sees a bounded error — never a partially reconstructed frame reported as
   success.

## 7. What this pass does not establish

* No decode was executed; every statement about timing is a citation of the
  #160 / #192 ledgers, not a new measurement.
* The temporal branch and the `index == 3` highpass site are characterised from
  source. Their real exposure is unmeasured on both hosts.
* Only HALF 1080p BGRA on the pinned 900 396-byte YUV422 packet was ever
  measured. FULL was not exercised on either host.
* No claim about other formats, resolutions, GOP structures or channel counts.
* `DecodedBandsValid`'s per-topology logic was re-derived by re-implementation,
  not by executing it; it has no live caller to execute.
* The Windows per-channel attribution gap (#192) is still open on that side and
  is not repaired here.

## 8. Reproduce

```
python3 tools/verify_rca195_band0_contract.py --check
python3 tools/verify_rca195_band0_contract.py --check --json docs/validation/rca195_band0_contract_2026-10-19.json
python3 tools/verify_rca195_band0_contract.py --self-test
```

`--self-test` copies `upstream/Codec` into a scratch directory and applies six
mutations (released macro rewritten, band-count table altered, spatial guard
deleted, started-writer lock made unconditional, `DecodedBandsValid` given a
live caller, delayed thread start disabled). Each must be rejected; the run
fails if any mutation still verifies. Both checks are registered in CTest as
`rca195_band0_contract` and `rca195_band0_contract_selftest`.

Machine-readable output of this pass:
`docs/validation/rca195_band0_contract_2026-10-19.json`.

## 9. One next action

Nothing on this lane should build a candidate from §6 yet. The cheapest correct
next step is the one #195 already names for another lane: the **#158 owner
closes the two disclosure gaps** on #192 — per-channel output-plane digests for
the five corrupt calls, and the `EXPECTED_PUBLISH_BAND0` correction — so a
cross-host statement can distinguish "same admission window" from "same
corruption". Only after that is a targeted test-only candidate worth building,
and only against the producer relation in §4, not against a bitmask.
