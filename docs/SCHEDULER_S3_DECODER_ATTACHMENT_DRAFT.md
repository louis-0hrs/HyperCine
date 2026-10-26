<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# S3 decoder attachment — draft

**Status: draft only. Nothing in this note is implemented, and nothing here
attaches a decoder.** No GoPro/CFHD bitstream parser, no CUDA or Metal consumer,
no change to `upstream/`, and no change to the arithmetic of
`InverseWavelet5To4`. The synthetic graph in `src/core/decode_task_graph.*`
stays synthetic. A productive callback must not be wired in until the gates
below are implemented and qualified on their own.

The first attachment is one spatial consumer of an already dequantized
wavelet5 plane, the same shape the research unit already describes. It is not
a codec rewrite.

## What the scheduler already guarantees

These stay the substrate. The attachment adds checks in front of them; it does
not replace them.

- `publish()` copies into node-owned storage before the release-store of the
  version. The caller may free its own buffer when `publish()` returns.
- `run()` joins every worker before it returns. There is no asynchronous
  completion API. `DecodeTaskGraph` is not reused across generations.
- Completion requires a `BufferVersion` of **this** generation. The first
  `GraphError` is write-once. `fail_locked()` does not move a dependent to
  `Ready`.
- `wait_for_input()`, `input_published()` and `input()` share one precedence:
  stop or the first error outranks a version that was published earlier. The
  non-blocking probes then return false and an empty view. Generation, version,
  address and length are admitted together, and the address must be the
  graph-owned copy.

## 1. Checked sizes and a per-generation budget

`BufferVersion` is still an untyped span. The first attachment must refuse a
publish whose declared size does not match the plane it claims, before the
version store.

Per plane, reuse the research-unit ceiling already stated on
`SignedPlaneView` / `InverseWavelet5To4`: storage covers every row, pitch is
32-byte aligned, and one plane is at most 2 MiB. A publish larger than that,
or smaller than `width * height * sizeof(int16_t)` once pitch is accounted
for, is not a publication (version stays 0).

Per generation, cap the sum of graph-owned copies. The first ceiling is the
known spatial footprint, not a frame buffer cache: three SDK channels (Y, V, U)
times four wavelet5 bands times 2 MiB, which is 24 MiB of owned input, plus
the single lowpass output of the consumer. A publish that would exceed the
generation budget is rejected the same way a null or zero-length publish is
rejected: no version store, no waiter wake, no ready transition. There is no
partial plane.

The budget is accounted when the copy is installed and released only when the
graph is destroyed after `run()` has drained. It is not a refcount.

## 2. Typed plane and extent contract

A consumer-visible input is not a `const void*` plus a length. The view that
`input()` would hand to a real callback carries, together:

| Field | Rule |
| --- | --- |
| generation | equals the graph generation, or the view is empty |
| version | non-zero, or the view is empty |
| sample | signed 16-bit, little-endian, one sample per element |
| width, height | both non-zero; width matches the declared extent |
| pitch | bytes, 32-byte aligned, `pitch >= width * 2`, `pitch * height <= bytes` |
| address, length | the graph-owned copy; length is the allocated byte count |
| channel, wavelet, band | match the declared `TaskKey` of that edge |

Any one field missing or inconsistent empties the whole view. A matching
length with a foreign generation, or a matching generation with a zero height,
is not half-valid. This is the same joint admission the byte-span API now
applies to generation, version, address and length, extended with the extent
the decoder would actually index.

Extents for the first consumer are the ones already pinned for the research
unit, not new ones: width 16..480 and a multiple of 8, height 4..270, output
dimensions doubled. 1920x1080 and 3840x2160 frames show up only as the
channel-scaled planes in `docs/NEXT_INVERSE_STAGE_144.md` (Y at full chroma
grid, V and U horizontally half). The attachment does not invent a fourth
channel or a packed YUV pixel.

## 3. Channel and band mapping

`TaskKey` already names `channel`, `transform`, `level`, `frame`, `band`.
The first consumer reads exactly four inputs and writes one output:

- inputs: wavelet5 bands 0, 1, 2, 3 (LL, LH, HL, HH), one channel, scale 0,
  already dequantized;
- output: the next lowpass for that same channel.

SDK channel order is Y, V, U as channels 0, 1, 2. The attachment must not
swap V and U, and must not treat a control record as a band. A plane published
under a different channel, wavelet level or band is a different edge: the
consumer's `input(i)` for the declared source stays empty. Publishing band 1
does not satisfy an input declared as band 0.

## 4. Controlled object lifetime

- The producer allocates, fills, and `publish()`es. The graph copy is the
  object the consumer reads. The producer allocation dies when the producer
  says so, after `publish()` returns.
- The copy is not resized and the node is not relocated (`std::deque`), so the
  address stays stable until the graph destructor.
- A callback may hold the view only until it returns. Storing the address in
  a structure that outlives `run()` is a use-after-free, not a supported cache.
- There is still no retain/release. Dropping one dependent must not free bytes
  another dependent of the same node reads.
- `TaskSpec::published` remains an extra fail-closed predicate. It is not the
  buffer, and it must not capture a callback local.

## 5. Atomic error propagation

- The first `GraphError` sticks. A later allocation, callback, cancel or
  `OutputNotPublished` does not overwrite it.
- Setting that error sets `stop_`. Both are visible to every subsequent
  admission under the graph mutex.
- After that point `input_published()` is false and `input()` is empty, even
  if producer A already stored a valid version. The consumer reads zero
  payload bytes. `wait_for_input()` throws `PublicationTimeout` for the same
  decision.
- A stage that sees cancel or an exception must not publish a partial plane.
  The scalar unit already states that; the attachment keeps it as a hard rule.
- Dependents of a failed or canceled producer are not moved to `Ready`.

## 6. Release only after a full worker drain

Owned planes are freed by the graph destructor, and the destructor is only
safe once `run()` has returned: `drained_ == true` and every worker joined.
`cancel()` does not free. It only stops scheduling and lets callbacks that
already started leave. The owner destroys the graph after `run()`, not from
inside a callback and not from a second thread racing the drain.

A publish that fails the size or budget check frees nothing it did not
allocate: the rejected call leaves the node's version at 0 and does not swap
in a new copy.

## 7. Cap on parallel frame generations

One `DecodeTaskGraph` per `(session, slot, generation)`. A generation value is
never reused, and a graph is never reset for the next frame.

The first attachment runs **one frame generation at a time**. A second frame
does not construct its graph until the previous `run()` has returned and that
graph has been destroyed. Workers inside one graph stay on the existing cap
(`run()` rejects `workers == 0` or `workers > 16`). That cap is not a license
to overlap frames: sixteen workers on one generation is still one generation.

A later generation that observes a `BufferVersion` whose generation fields do
not match its own graph sees an empty input. Cross-generation reads are not
repaired by a matching pointer.

## Explicitly out of scope

- Parsing a CFHD or GoPro bitstream, entropy decode, and temporal inverse.
- CUDA streams, Metal, Vulkan, and any change under `upstream/`.
- Replacing `InverseWavelet5To4` or widening its numeric contract.
- An asynchronous completion API, a buffer pool shared across generations, or
  a refcount.

When this draft is implemented, the qualification is still a synthetic graph
plus the existing scalar research checks. A green scheduler run does not
qualify a decoder.
