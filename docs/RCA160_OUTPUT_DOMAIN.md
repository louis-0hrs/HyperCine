# RCA #160 — the corrupt output is a per-row DC offset, not a missing band

Diagnostic-only engineering note. No decoder change, no `upstream/` edit, no
release or safety-gate implication. Companion to the synthetic A/B in
`docs/DECODE_TASK_GRAPH_147.md` and the rate measurements in
`docs/RCA159_THREAD_OVERSUBSCRIPTION.md`.

## Question

Earlier passes established *when* the original GoPro CPU decoder returns
successful-but-different pixels under oversubscription, and excluded several
completion-path candidates by measurement. What was never measured is what the
corrupt frame actually *is*. Two competing readings were on record:

* the lowpass (band 0) contribution is **missing**, so rows are reconstructed
  from a partially filled band;
* the frame is written from a **different coefficient set**, i.e. the transform
  itself ran on wrong input.

They predict different output shapes, so the output alone can separate them.

## Method

* Pinned immutable packet, 900396 bytes, SHA-256 `2b9a28de…ec52`; encoder source
  BGRA `45c166c3…afff1`. Digest re-verified before every batch.
* Condition: `TAG_CPU_MAX=4` → 8 decode worker threads against `taskset -c 0`
  → 1 permitted CPU. Oversubscribed by 8×, the condition #159 mapped.
* **One decode per fresh process**, `CFHD_DecodeSample` once, output buffer
  pre-filled with `0xCD` so untouched memory is detectable.
* 90 decodes in two independent batches (50 + 40), same host, CPU-only.
* Analysis tool: `tools/analyze_decoder_output_domain.py` (stdlib only).

## Result 1 — a small, recurring, deterministic state set

| batch | calls | distinct frames | modal share |
| --- | --- | --- | --- |
| M1 | 50 | 9 | 0.84 |
| M2 | 40 | 6 | 0.775 |

All 90 calls returned `CFHD_ERROR_OKAY` (0), 1920×1080, pitch 7680. Ten distinct
corrupt states exist in total, and **three of them recur across both batches**
(byte-identical RGB planes). A corrupt frame is therefore not entropy: it is one
of a handful of reproducible wrong states.

The untouched-buffer sentinel `0xCD` appears nowhere in any output, and no output
contains unwritten regions. The decoder wrote a complete, fully initialised
frame every time.

## Result 2 — the difference is a separable per-row field

For each corrupt frame and colour plane, the difference to the modal frame
`D = C − modal` was tested for separability (`D[y,x] = a[y]·b[x]/mean`):

| statistic | observed | null: two different **correct** planes | null: correct decode vs encoder source |
| --- | --- | --- | --- |
| rank-1 energy fraction | **0.992 – 0.9999** | 0.53 – 0.97 | 0.06 – 0.51 |
| separable-fit residual MAE | 0.15 – 3.5 (amplitude 15–115) | — | — |

So the deviation is one dominant rank-1 component: a **row-varying offset**. It
is not structured residual noise, and it does not resemble the difference
between two different correct images.

A per-row **gain** model (`C[y] ≈ (1+k[y])·R[y]`) fits clearly worse and is
negative for most planes; the offset model wins on every plane.

## Result 3 — "zeroed plane" is the saturation extreme of the same offset

Two corrupt states showed an entire colour plane at exactly 0, which reads like a
missing plane. It is not. With clipping applied after the offset, both are
reproduced **exactly**:

| plane | modal max | model | pixel-exact |
| --- | --- | --- | --- |
| B | 239 | `clip(R − 239)` | **100.00 %** |
| R | 135 | `clip(R − 135)` | **100.00 %** |

A large negative row offset drives the whole plane below zero and the clamp turns
it into a constant plane. That is one fault with a saturation extreme, not a
second fault. It also explains why these planes looked like "missing content".

## Result 4 — AC is intact

Under the offset model, high-frequency content is preserved. The planes are not
smoothed, not partially reconstructed and not stale: the same detail, displaced
in level. The 0.99+ rank-1 fraction is the direct statement of that — a missing
band would leave low-frequency error with full-band residual, which is what the
`correct decode vs encoder source` null looks like (0.06–0.51).

## Correction to the earlier record

Two statements from previous passes do not survive this measurement:

* **"contiguous row prefix from row 0"** describes a *minority* shape here. Four
  of ten states are partial-row (a contiguous block, not necessarily from row 0),
  the majority affect all 1080 rows. The partial cases are the ones previously
  reported as prefix.
* **"lowpass content in the affected rows is near-zeroed"** is an artefact of
  clipping. Where clipping is accounted for, the affected rows carry the same
  lowpass content as the modal frame, shifted. Only the fully saturated planes
  are actually zero.

Both corrections narrow the search rather than widen it: nothing in these frames
looks like an incompletely reconstructed band.

## What this does not establish

* **No mechanism.** A per-row DC offset is consistent with a wrong per-row
  prescale/rounding or a row-level conversion stage reading a stale row term; it
  is not proof of either. Naming a source line needs per-row instrumentation at
  the planar→interleaved output path, which this pass deliberately did not do.
* **No Windows parity.** One Linux host, one packet, one condition. The Windows
  4/45 observation is untouched by this.
* **Not a per-pixel identity claim.** Clipping means the exact residual cannot be
  fully separated from a small additional error; the reported `fraction_within_1`
  is stated per case in the evidence JSON rather than summarised as "exact".
* **No rate claim.** Rates here are not comparable to #159's sweep cells; this
  run used a single oversubscribed condition to obtain states, not to measure
  frequency.

## Second pass — the fault crosses output formats and survives a bypassed transform

The offset measurement above does not say which stage produces the offset. Two
cheap contrasts narrow it: change the **output pixel format** (different packing
and conversion code) and change the **decoded resolution** (different subset of
the transform chain).

### Output format does not select the fault

24 fresh processes per format, same packet, same oversubscription, one decode
each. Frames are compared to the modal frame of their own format; `rand()`-based
rounding noise is separated from structured error by magnitude (±1 count is
dither, anything larger is structured).

| output format | calls | distinct | structured | dither / identical |
| --- | --- | --- | --- | --- |
| BGRA 8-bit | 24 | 3 | 3 | 20 |
| RG24 RGB444 8-bit | 24 | 24 | 2 | 21 |
| RG48 RGB444 16-bit | 24 | 4 | 3 | 20 |
| YUY2 8-bit 4:2:2 | 24 | 18 | 6 | 17 |

Every format produces the same structured error, including the 16-bit RGB path
that has no 8-bit clamp. Two consequences:

* the fault is **not** created by the BGRA packing stage, and
* the earlier `ZEROED` planes were genuinely a **clamp** artefact, not the fault
  itself — with 16 bits of headroom the same corruption appears as large signed
excursions instead of a constant plane.

The distinct-frame counts for RG24 (24/24) and YUY2 (18/24) look alarming but are
not evidence of extra corruption: their per-call variation is ±1 count on 32 %
and 4 % of samples respectively, which is the decoder's `rand()` dither. The
structured minority in each format is what matches the BGRA finding.

Across bit depths the offset stays roughly the same **fraction of full scale**
(8-bit median 38 %, 16-bit median 41 %), which is what an error applied after the
value has been scaled into the output range looks like — not an error in the
integer coefficient domain.

### HALF resolution still corrupts, QUARTER did not

| resolution | work per call | calls | distinct | structured |
| --- | --- | --- | --- | --- |
| FULL 1920×1080 | 28.6 ms median | 24 | 3 | 3 |
| HALF 960×540 | 11.2 ms median | 48 | 4 | **4** |
| QUARTER 480×270 | 5.2 ms median | 88 | 1 | **0** |

HALF resolution **skips the inverse frame transform entirely** and copies the
lowpass image straight to the output (`decoder.c`, `DECODED_RESOLUTION_HALF`
branch). It still corrupts, with the same separable signature (offset explained
75–98 % of the per-plane error). That excludes the inverse frame transform as the
sole origin — the fault is reachable from the lowpass copy path too.

QUARTER was stable in 88 calls, including 24 at `TAG_CPU_MAX=16` (20 workers on
one CPU, i.e. harsher oversubscription than the failing cells). Tempting as that
looks, **it is not proof of exclusion**: QUARTER does about five times less work
per call, so a per-call race window simply has fewer opportunities. Recorded as a
lead, not a result.

### What the second pass adds

* The fault survives a change of output packing and a change of the transform
  subset, so it belongs to something **shared** by those paths — the lowpass
  coefficient data itself, or the per-row scaling/offset applied when that data
  is written out.
* The clamp story is now closed: `ZEROED` planes were saturation, and 16-bit
  output shows the same fault unclipped.
* Still **no mechanism and no source line**. The remaining discriminator is
  whether the lowpass band's *content* differs between a corrupt and a correct
  call, or only the term applied while writing it.

## One next action

Instrument the **lowpass write path** in a private copy, not the inverse wavelet
(which HALF bypasses): per output row, log the scale/prescale term actually
applied and a hash of the source lowpass row being read. Compare a corrupt call
against a correct call at the same settings.

* If the lowpass row hashes match but the applied term differs, the fault is in
  the row scaling/offset computation.
* If the lowpass row hashes differ, the corruption is already in the coefficient
  data before any output conversion, and the search moves to band production.

`TAG_CPU_MAX=1` is the negative control (single worker, no oversubscription); the
`ZEROED`/saturated states no longer serve as a forced positive here, because this
pass showed they are a clamp artefact rather than a distinct fault.

## Artefacts

* `tools/analyze_decoder_output_domain.py` — classifier, stdlib only.
* `docs/validation/rca160_output_domain_m1_2026-10-18.json` — 50 calls.
* `docs/validation/rca160_output_domain_m2_2026-10-18.json` — 40 calls.
* `docs/validation/rca160_output_domain_combined_2026-10-18.json` — recurring
  states across batches, sanitised relative IDs and hashes only.
* `docs/validation/rca160_format_path_matrix_2026-10-18.json` — four output
  formats and three decoded resolutions, same packet and condition.

Raw decoded frames stay outside the repository; only relative IDs and hashes are
published.
