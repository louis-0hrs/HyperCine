<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# S2 output completion

**Status: synthetic graph only.** This note qualifies buffer lifetime inside
`src/core/decode_task_graph.*`. It does **not** integrate a GoPro or CFHD decoder,
and it does not attach a CUDA or Metal consumer. A productive decoder must not be
wired to this graph until the rules below have been qualified on the scheduler
itself. `upstream/` is untouched.

## What completion is

A task becomes `Completed` only when `publish()` has installed a `BufferVersion`
for **this** graph generation:

- `version != 0` (a returning callback that never published is not a result),
- `published_generation` equals the graph generation,
- the span is non-null and non-zero,
- the address is the graph-owned copy, not the caller's pointer.

The boolean `TaskSpec::published` predicate is an extra fail-closed check. It
cannot by itself complete a task. `GraphError::OutputNotPublished` is terminal:
the node is `Failed`, and `fail_locked()` does not move any dependent to `Ready`.
The same is true of cancellation and of `PublicationTimeout`. The first recorded
`GraphError` is write-once.

## Ownership: per-node copy, not a refcount

Reviewed against the code that is actually here, not against a generic scheduler:

- `run()` is synchronous. It joins every worker before it returns. There is no
  asynchronous completion API. The owner must keep the graph alive until `run()`
  returns. Every consumer callback therefore happens before the graph is destroyed.
- `publish()` used to store the caller's `const void*` and never copy it. A
  producer that freed that buffer after `publish()` returned left every dependent
  with a dangling read. That is the lifetime hole.
- `TaskContext` has no retain and no release. Consumers do not check a buffer
  out. A refcount would be an invented protocol. Forgetting one successor —
  a removed dependency, a cancel, a timeout — either leaks or frees the bytes
  while another dependent still reads them.
- Each task publishes at most one version. The node lives in a `std::deque`, so
  it is not relocated. The copy is a `std::vector<unsigned char>` on that node,
  filled once and not resized. Its address stays valid until the graph destructor
  runs, which is after every dependent callback.

One copy therefore covers **all** dependents of that task. Dropping an edge from
a retain-set cannot free it, because there is no retain-set. The dependency
counter is separate: a successor becomes `Ready` only when every declared
predecessor has completed with a valid buffer (`remaining` hits zero). Completing
one predecessor does not remove the others.

## What this still is not

- Not a decoder, and not a pixel oracle. `GraphProfile` stays `Synthetic`.
- Not a claim that an arbitrary production consumer can hold a buffer past
  `run()`. Nothing in this API can.
- Not a change to CUDA, Metal, or `upstream/`.
