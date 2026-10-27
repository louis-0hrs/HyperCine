<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# T4 plane dependency

**Status: synthetic graph only.** This note qualifies one decoder-shaped
dependency inside `src/core/decode_task_graph.*`. It does **not** integrate a
GoPro or CFHD decoder, it does not add a wavelet kernel, and it does not touch
`upstream/`. The original SDK races tracked by **#195 are not fixed** and are
not claimed by a green run of this gate.

## What is modeled

One spatial step, not a codec:

```text
wavelet5 band0 --\
wavelet5 band1 --+--> wavelet4 lowpass (band 0, same channel)
wavelet5 band2 --/
wavelet5 band3 -/
```

Channels are Y, V, U as 0, 1, 2. V does not satisfy U. Band 1 does not satisfy
band 0. A later generation does not satisfy this one. The consumer is a
witness that the four edges arrived intact. It is not `InverseWavelet5To4`
and not a second inverse.

## Rules the scheduler enforces

- `publish_plane()` copies into graph-owned storage before the version
  release-store. The caller may free its buffer when the call returns.
  `cancel()` does not free that copy. The destructor does, after `run()`
  has joined every worker.
- **T4-EXTENT.** A span that fails the signed-plane check (alignment, geometry,
  stride, or `pitch * height` bytes, capped at 2 MiB) is not a publication.
  Version stays 0. A later legal publish of the same task is still admitted.
- **T4-BUDGET.** One generation may retain at most 26 MiB of plane copies:
  3 channels × 4 wavelet5 bands × 2 MiB, plus one 2 MiB lowpass. A tighter
  `GraphLimits::generation_budget_bytes` is honored. A larger value is clamped
  to the ceiling. Over budget is rejected before the copy, with no waiter wake
  and no charge. Untyped `publish()` does not spend the budget.
- **T4-KEY.** Only Y/V/U, wavelet5 bands 0..3, or the wavelet4 lowpass
  (level 4, band 0) may publish a plane. The channel and band are the node
  key, not a tag the caller passes in.
- **T4-STALE.** `input_plane()` named with another generation returns an empty
  view and does not expose this generation's bytes. `cancel()` of another
  generation does not stop this one.
- **T4-IDENTITY.** The producer's channel, wavelet, band, frame and transform
  must match the identity the consumer names. A crossed edge is empty.
- **T4-GEOMETRY.** The stored width, height and pitch must match too. A wider
  claim over the same bytes is not a view the caller may index.

`input()` stays the untyped S2 span. It does not learn channel or band. A
plane consumer that skips `input_plane()` is outside this contract.

Missing plane: the producer fails `OutputNotPublished`, the dependent is not
readied, and `run()` returns. No spin and no fabricated samples. A lax graph
whose consumer actually waits hits `PublicationTimeout` and still sees an
empty plane. An exception fails the generation with `Callback` and does not
ready the lowpass. The first error is unchanged.

## What remains open

Real coefficient order, a qualified inverse, entropy, temporal bands, CUDA and
Metal consumers, and any repair of the reference decoder. A passing T4 run
does not speak to #195.
