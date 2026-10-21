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
| Output-boundary readiness | `HOLLOW` — the always-on gate checks only for NULL; the correct gate is compiled out |
| Thread-level access pattern | `SANITIZER_CONFIRMED` — pre-existing TSan logs show the unlocked reads and concurrent double reconstruction |
| Silent drop with a success return | `FALSE_OKAY_DEMONSTRATED` — induced, 120/120 calls in four arms (§8) |
| Production-safe contract | `NOT_DESIGNED_YET` — requirements in §9 |
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

### 3.3 The output path: a hollow gate that is always on, and a real gate that is always off

> **Correction to this report's first revision.** It described the inverse-stage
> gaps above as the only unguarded sites and stopped there. That was incomplete.
> The frame-output path carries a pair of helpers that fail in opposite
> directions, and one of them sits directly on the measured HALF route.

**`AllTransformBandsValid` (`decoder.c:2050`) — always compiled, never checks a
band.** It is wrapped in `#if DEBUG || 1`, which is unconditionally true, and it
has **seven live runtime call sites** (`:13303`, `:13315`, `:13329`, `:13534`,
`:13555`, `:13576`, `:13599`) — including the `DECODED_RESOLUTION_HALF` and
`HALF_HORIZONTAL` branches that `ReconstructSampleFrameToBuffer` and
`ComputeOutputDimensions` take. Its body does exactly this:

```c
//if (!AllBandsValid(wavelet))
if(wavelet == NULL)
{
    return false;
}
```

The band test is **commented out**. Every caller therefore believes it has
validated the output frame and receives a `true` that only means "the pointer is
non-NULL". This is not a rare path: HALF 1080p — the exact condition #160 and
#192 measured — goes through it.

**`AllLowpassBandsValid` (`decoder.c:2078`) — the only correct output check,
compiled out everywhere.** It is the one helper that inspects
`band_valid_flags & BAND_VALID_MASK(0)` per channel. Its single call site
(`:24533`) is an `assert()` inside `#if DEBUG`. `decoder.c` defines
`DEBUG` as `(1 && _DEBUG)`, and `_DEBUG` is an MSVC Debug-configuration macro
that **no build file in this repository defines** — verified by scanning
`CMakeLists.txt`, `cmake/*.cmake` and `tests/CMakeLists.txt`. On gcc and clang
the expression is therefore `0`, and the block does not exist. The tool checks
that the call is the *sole body* of that conditional block, so removing the
guard does not leave a neighbouring `#if DEBUG` behind to satisfy the test.

Consequence, stated carefully: the release build has **no** per-channel band-0
check at the output boundary, and the seven guards it *does* have cannot detect
the condition they appear to guard. That is a source-level statement about what
the code can check, not a claim that the measured corruption came through this
path — that was not measured, on either host.

**`AllBandsValid` (`decoder.c:2044`)** is the third reader. It forwards to the
vacuous macro, and both of its call sites (`:11327`, `:11725`) sit in the `#else`
branch of `#if _THREADED_DECODER`, which `config.h` sets to 1. Dead in the
shipped build, and vacuous even if reached. The verifier resolves real
preprocessor nesting for these two lines rather than searching for the nearest
`#if` in the file.

### 3.4 The queue drain, which is where the race is decided

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

### 3.5 Sanitizer evidence: the unlocked reads and a concurrent double reconstruction

Everything above was read out of the source. Three pre-existing ThreadSanitizer
logs on this host then confirm the access pattern directly. They were produced
on 2026-10-18 from a different worktree, so before using them the tool checks
that every frame they cite resolves to the same line in this tree. It does: the
cited `upstream/Codec` files are byte-identical
(`decoder.c` `acd477cb…`, `entropy_threading.c` `32033d23…`, `spatial.c`
`97555d73…`), so the logs are evidence about these sources.

| Log | `cpu_max` | Race blocks | Same address | Both sides in the inverse | Flag-writer blocks |
| --- | --- | --- | --- | --- | --- |
| control | 1 | **0** | – | – | – |
| run A | default | **36** | 35 | 33 | 2 |
| run B | default | **32** | 32 | 30 | 1 |

The control and run B decode the *same* sample (286 596 B, `10c7e2b5…`), so the
contrast is not a fixture difference: one worker, no races; four workers, 32.

**The flag pair is confirmed as a data race, with the lock on exactly one side.**
Every flag-writer block shows the writer holding `entropy_worker_new.lock`
(`mutexes: write M0` at `decoder.c:24098`) while the other side reads without
any lock, at precisely the two places predicted from source:

* `decoder.c:13039` — `if ((lowpass->band_valid_flags & BAND_VALID_MASK(0)) == 0)`,
  the "has this wavelet already been reconstructed" test; no `Lock()` exists
  anywhere in that function.
* `entropy_threading.c:285` — the post-`Unlock()` re-check of `BANDS_ALL_VALID`
  described in §3.4.

So the pair is read torn, in the shipped build, on real decodes. That is no
longer an inference.

**A second, unanticipated finding: the same reconstruction runs twice at once.**
In 33 and 30 blocks respectively, *both* stacks pass through
`ReconstructWaveletBand` at `decoder.c:13107`, i.e.
`TransformInverseSpatialQuantLowpass`. Two workers are inside the inverse
transform for overlapping memory simultaneously. The two sides reach it through
different `scale` branches — `wavelet.c:5756` (`scale > 1`,
`InvertSpatialQuantDescale16s`) versus `wavelet.c:5778` (`scale == 0`,
`InvertSpatialQuant16s`) — writing and reading the same heap block.

That is consistent with the indexing in §3.2: the lowpass branch writes
`wavelet[index-1]->band[0]` while reading `wavelet[index]->band[0]`, so the
entry at `index` and the entry at `index+1` share one allocation, and their
`prescale` values are indexed by different wavelet numbers. Two levels of the
pyramid therefore touch the same plane. I did not predict this from source and
am not claiming which index pair each report corresponds to — the sanitizer
does not record it. What the logs establish is that it happens.

**What these logs do not show.** All of them decoded cleanly and produced
byte-identical output across repeats; the guard band was intact. They used
185 648-byte and 286 596-byte samples, **not** the pinned 900 396-byte packet,
and no gate patch. With eight decodes in total they also have little power
against a defect seen in roughly one call in five, so this is not evidence
against the causal link — it is simply a different question. The sanitizer
confirms the mechanism; the A/B/A′ ledgers measure the symptom. Neither
substitutes for the other.

The reduction is reproducible:
`tools/analyze_rca195_tsan_log.py --log <log>`, which emits no paths, addresses
or thread ids.

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

## 6. Channel × mode matrix

The guard predicate is **channel-agnostic**; what differs per channel is only when
a given wavelet's bands become available. Verified, not assumed:

- YUV422 sets `codec->num_channels = 3` (`decoder.c:11272`). ch0/ch1/ch2 are the
  three planes; nothing in the readiness path branches on which one it is.
- The guard block at `:13032`, `:13097` and `:13157` contains **no reference to
  `channel`** at all. Same code, same predicate, three instantiations.
- At HALF the probe counts exactly **9 claims per decode** (`free_entry = 9`,
  `claimed = 9`), i.e. 3 queue points × 3 channels. That is the measured
  instantiation count, not a derivation.

Wavelet sets per mode, from the subband tables and the resolution masks:

| Transform | Resolution | Subbands decoded | Wavelets present | Queue points/ch |
| --- | --- | --- | --- | --- |
| field-plus | FULL | 0–16 (`17`) | {0, 1, 3, 4, 5} | 5 |
| field-plus | HALF | 0–10 (`11`) | {3, 4, 5} | 3 |
| spatial | FULL | 0–9 | {0, 1, 2} | 3 |
| spatial | HALF | 0–6 (`0x7F`) | {1, 2} | 2 |

The temporal wavelet (index 2) is created only when an empty-band record
(`subband == 255`) arrives, and the two frame wavelets (0 and 1) get their band 0
from the temporal inversion. At HALF, wavelet 0 does not exist until that
happens — which is why the measured fault surfaced at `index == 1` and `index == 2`
rather than at the frame wavelets.

**What differs per channel is arrival order, not code.** The three channels are
decoded in bitstream order and each queues its own transforms; the fault needs one
channel's producer to lag its consumer within the same window. That is consistent
with the measured evidence (Linux waited on `ch1/index1`, Windows on `ch0/index1`
and `ch2/index1`) without requiring any channel-specific defect.

## 7. The five traces, separately

The issue asks for these stated separately from measured frequency. They are.

**(a) Expected valid/started transitions.** A highpass band raises `started` when
decoding begins (`UpdateWaveletBandStartedFlags`, `:24127`, lock compiled out) and
`valid` when decoding completes (`UpdateWaveletBandValidFlags`, `:24098`, under the
lock). A lowpass band raises both together at `:24098-24099`. The released
predicate compares the two words, so a band that reached only `started` cancels
out on both sides: `0xE` reads as "complete" while band 0 has not begun.

**(b) Claim vs ownership vs execution vs publication.** `done = 1` and
`num_entries--` at `:271` is a **claim**, taken under the lock, before the inverse
runs. Execution happens at `:302`, **after** `Unlock()`, behind an unlocked
re-check at `:285`. Publication is the `valid` write inside the reconstruction.
Nothing records completion: `queue[].done` is written at `:271` and read only in
that same admission condition — it is not a completion acknowledgement. The probe
measures the gap directly: `claimed = 9`, `executed = 7`.

**(c) Source-buffer generation and reuse.** Bands share one allocation per wavelet
(`IMAGE_ALLOC_ONE_MALLOC`, `wavelet.c:204-209`). The lowpass branch writes
`wavelet[index-1]->band[0]` while reading `wavelet[index]->band[0]`
(`:12997-13009`), so two adjacent pyramid levels touch the same plane — the
double-reconstruction the sanitizer reported. Flags are zeroed per group in
`ClearTransformFlags` (`:1393`) and on (re)creation (`wavelet.c:924`); a wait that
is not tied to the generation can therefore be satisfied by a recycled bit.

**(d) Synchronisation and ordering.** `valid` is written under
`entropy_worker_new.lock`; `started` is written with its lock compiled out
(`_DELAYED_THREAD_START == 1`); both are plain `uint32_t`, not atomics. The
consumer reads them unlocked at `:13039` and at `:285`. **Correction to an earlier
verbal claim of mine:** an enqueue-side race is *not* established. `QueueThreadedTransform`
is reached only from the parser path (`DecodeSampleSubband` ← `UpdateCodecState` ←
`DecodeSampleGroup` / `DecodeSampleIntraFrame`), the worker never calls it, and the
only live `THREAD_MESSAGE_START` for that pool is sent from `WaitForTransformThread`
*after* enqueueing. Absence of the lock there is a smell, not a demonstrated race,
and I am labelling it unobserved.

**(e) First terminal error and cleanup.** This is where the design fails worst.
The drain loop contains **zero** assignments to `decoder->error`. `WaitForTransformThread`
(`:24291-24296`) calls `ThreadPoolWaitAllDone` and then zeroes `started`,
`num_entries`, `next_entry` and `free_entry` **without checking whether anything
was left unadmitted or unexecuted**. The output boundary cannot compensate:
`AllTransformBandsValid` only tests for non-NULL pointers. So a dropped transform
produces a frame that is reported as successfully decoded — measured in §8.

## 8. Bounded fail-closed counterexample

The review asked for a deterministic absent/delayed-producer probe on a private
scratch SDK, recording queue state and the public return **on the same call**.
That exists now, and it confirms the silent drop.

### What was built

- `tools/rca195_probe.h` — test-only counters: `CLAIM` logged right after
  `done = 1; num_entries--;`, `EXEC` logged immediately before the reconstruction,
  and a queue record immediately before `WaitForTransformThread` discards it.
- `tools/apply_rca195_probe.py` — applies the probe to a private out-of-repo copy
  by line anchors, failing closed on ambiguity. `upstream/` is never touched.
- `tools/rca195_probe_harness.cpp` and `tools/run_rca195_probe.py` — one decode per
  fresh process, one JSON record per call, classification and verdict rules.

The private copy was byte-identical to the frozen tree before patching
(`decoder.c` `acd477cb…`, `entropy_threading.c` `32033d23…`).

### Protocol honesty

Two registrations exist, and neither is rewritten after the fact:

| Version | SHA-256 | Outcome |
| --- | --- | --- |
| v1 | `3001720e…` | **`INCONCLUSIVE`** |
| v2 | `00b5748e…` | `FALSE_OKAY_DEMONSTRATED` |

v1 required `num_entries > 0` at the reset. The injection hook consumes the entry
exactly as production does, so that value is legitimately 0 — **the rule was
unfulfillable by my own construction**. The v1 run stands as `INCONCLUSIVE` in the
record; v2 replaced the witness with `claimed > executed`. That is the same
bookkeeping discipline I applied to #192, applied here to my own work.

### Result — 30 calls per arm, fixed order

| Arm | Injection | `claimed` | `executed` | `sdk_ok` | `guard_ok` | classified |
| --- | --- | --- | --- | --- | --- | --- |
| A | none | 9 | 9 | 30/30 | 30/30 | 28 `OK`, 2 `NATURAL_VARIANT` |
| B | skip `ch1/index2` | 8–9 | 7 | 30/30 | 30/30 | **30 `DROPPED_FALSE_OKAY`** |
| C | skip `ch0/index2` | 9 | 7 | 30/30 | 30/30 | **30 `DROPPED_FALSE_OKAY`** |
| D | skip `ch2/index2` | 8–9 | 7 | 30/30 | 30/30 | **30 `DROPPED_FALSE_OKAY`** |
| E | skip `ch1/index1` | 9 | 8 | 30/30 | 30/30 | **30 `DROPPED_FALSE_OKAY`** |

**120 of 120 induced calls returned `CFHD_ERROR_OKAY` with an output that differs
from the control mode, while the witness recorded a claimed transform that was
never executed.** The control arm has zero such calls. Control modal digest
`38d1c149…`; natural-variant rate 2/30, reported separately and **not** counted as
evidence — that is the known #160 behaviour.

Ledger: `docs/validation/rca195_probe_ledger_2026-10-20.json`.

### Two instrument defects, found and fixed before the final run

Recorded because they are the kind of thing that turns a measurement into a
comfortable story:

1. The probe state was `static` in a header included by two translation units, so
   `skipped` read 0 where two skips had occurred. Fixed with a single definition
   (`RCA195_PROBE_OWNER`).
2. The skip hook ran unguarded at loop exit, where `channel` and `index` are loop
   leftovers, firing 18 times for 9 claims. Fixed by guarding on `wavelet != NULL`.

Both were caught because the counters disagreed with the log lines — not because
the verdict looked wrong.

### What this does and does not establish

It establishes that **the state is reachable and that the public API reports
success in it**. It does not establish how often the unmodified decoder reaches it
on its own: the injection fabricates the condition. It is also a different claim
from the #160 rate — those runs measured corrupt output under contention, these
measure a dropped transform under a forced drop.

**A digest match is corroboration only.** The registered limitation says so
explicitly, and the data showed why: the control arm's natural variants produced
byte-identical digests to the induced arms (`fad4a643…` appeared 3× in control and
29× in arm B). Identical bytes therefore do **not** prove a drop — the counter does.

## 9. What a contract has to state (design requirements, not a patch)

Derived from §2–§8, in the order the issue asks for. Nothing here is implemented.

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

## 10. What this pass does not establish

* No decode was executed; every statement about timing is a citation of the
  #160 / #192 ledgers, not a new measurement.
* The temporal branch, the `index == 3` highpass site and the output-boundary
  gates are characterised from source. Their real exposure is unmeasured on
  both hosts — in particular, nothing here shows that the measured corruption
  passed through the hollow output gate.
* Only HALF 1080p BGRA on the pinned 900 396-byte YUV422 packet was ever
  measured. FULL was not exercised on either host.
* No claim about other formats, resolutions, GOP structures or channel counts.
* `DecodedBandsValid`'s per-topology logic was re-derived by re-implementation,
  not by executing it; it has no live caller to execute.
* The Windows per-channel attribution gap (#192) is still open on that side and
  is not repaired here.

## 11. Reproduce

```
python3 tools/verify_rca195_band0_contract.py --check
python3 tools/verify_rca195_band0_contract.py --check --json docs/validation/rca195_band0_contract_2026-10-19.json
python3 tools/verify_rca195_band0_contract.py --self-test
```

`--self-test` copies the files the verifier reads into a scratch directory and
applies ten mutations, each of which must be rejected:

1. released macro rewritten to the strict form;
2. band-count table altered;
3. spatial guard deleted;
4. started-writer lock made unconditional;
5. `DecodedBandsValid` given a live caller;
6. delayed thread start disabled;
7. the hollow output gate made to check bands for real;
8. the correct output gate moved out of `#if DEBUG`;
9. a build file starts defining `_DEBUG`;
10. `_THREADED_DECODER` disabled, reviving the dead `AllBandsValid` path.

The run fails if any mutation still verifies. Both checks are registered in
CTest as `rca195_band0_contract` and `rca195_band0_contract_selftest`.

Machine-readable output of this pass:
`docs/validation/rca195_band0_contract_2026-10-19.json`.

## 12. One next action

Nothing on this lane should build a candidate from §9 yet. The cheapest correct
next step is the one #195 already names for another lane: the **#158 owner
closes the two disclosure gaps** on #192 — per-channel output-plane digests for
the five corrupt calls, and the `EXPECTED_PUBLISH_BAND0` correction — so a
cross-host statement can distinguish "same admission window" from "same
corruption". Only after that is a targeted test-only candidate worth building,
and only against the producer relation in §4, not against a bitmask.
