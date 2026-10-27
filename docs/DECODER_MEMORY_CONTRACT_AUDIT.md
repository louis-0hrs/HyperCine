<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Decoder memory contract audit (synthetic scheduler)

**Status: source audit only.** Head inspected: `44827e84f1658d67a506ecdd41d8d1c7ba7379c8`
(`origin/main`). No build and no test was run for this note. Nothing here is a
PASS. `upstream/` was not modified. Issues #144 and #195 are the context; this
note does not claim either is fixed, and it does not touch the FilmScan3
encoder/frontend/backend files.

Scope is the synthetic decoder scheduler and the one spatial consumer already
attached in front of it:

- `src/core/decode_task_graph.h`
- `src/core/decode_task_graph.cpp`
- `src/core/decode_s3_plane.h`
- `src/core/decode_s3_consumer.h`
- `src/core/decode_s3_consumer.cpp`
- `src/cpu/decode_spatial_scalar.h`
- `src/cpu/decode_spatial_scalar.cpp`

There is one byte ceiling in this module: `kPlaneGenerationBudget`
(`decode_task_graph.h:262-263`), installed as `generation_budget_`
(`decode_task_graph.cpp:70-72`, member `decode_task_graph.h:400`). The only
counter is `owned_budget_` (`decode_task_graph.h:401`), exposed read-only as
`owned_publication_bytes()` (`decode_task_graph.cpp:117-119`). The header states
what that counter means: bytes retained by successful `publish_plane()` calls
(`decode_task_graph.h:327-329`).

## Claim

> Die Budgetgrenze greift hauptsächlich für typisierte Plane-Kopien. Ältere
> untypisierte Buffer werden nicht über denselben Mechanismus abgerechnet.

**Verified, not refuted.** Typed plane copies are the only publications that
consult and increment the generation budget. Untyped S2 copies use the same
owned-vector storage and skip that counter entirely. The split is explicit in
the API comment, not an accident of a missed call:

- `GraphLimits::generation_budget_bytes` is documented as charged by
  `publish_plane()` only. "Untyped `publish()` does not spend this budget."
  (`decode_task_graph.h:277-282`)
- `publish()` (`decode_task_graph.cpp:164-177`) refuses a stopped generation, a
  second version, a null pointer and a zero length, then calls
  `commit_copy_locked`. It never reads `generation_budget_` or `owned_budget_`.
- `publish_plane()` (`decode_task_graph.cpp:236-248`) rejects the copy when
  `bytes > generation_budget_` or `owned_budget_ > generation_budget_ - bytes`,
  and only then adds `bytes` to `owned_budget_`.

Both paths install the bytes through `commit_copy_locked`
(`decode_task_graph.cpp:182-207`) into `Node::owned_bytes`. The storage is
shared. The ledger is not.

## Speicherpfad × budgetiert

"Budgetiert" means: the successful allocation is checked against
`generation_budget_` and added to `owned_budget_` before the caller can
observe a published version. "Nein" means that ceiling is not applied. There
is no second scheduler ceiling hiding under another name.

| Speicherpfad | Stelle | budgetiert? |
| --- | --- | --- |
| Untypisierte S2-Graph-Buffer | `TaskContext::publish` → `DecodeTaskGraph::publish` (`decode_task_graph.cpp:122-123`, `164-177`). Copy in `commit_copy_locked` (`182-207`): `std::vector<unsigned char> copy; copy.assign(...)` then `owned_bytes.swap`. Node field: `decode_task_graph.h:356`. No read of `generation_budget_` / `owned_budget_` on this path. A single untyped publish may be larger than `kPlaneGenerationBudget`; the only failure is `assign` throwing, which returns false and leaves version 0 (`188-191`). | nein |
| Typisierte S3/T4-Planes | `publish_plane` (`decode_task_graph.cpp:212-249`). Extent gate `check_signed_plane` at `235` (rules in `decode_s3_plane.h:51-71`, per-plane cap `kS3MaxPlaneBytes` = 2 MiB at `decode_s3_plane.h:24`). Budget gate at `decode_task_graph.cpp:236-238`. Charge `owned_budget_ += bytes` at `248`, after the copy succeeds. Ceiling expression: 3 channels × 4 wavelet5 bands × 2 MiB plus one 2 MiB lowpass (`decode_task_graph.h:259-263`). A larger `generation_budget_bytes` is clamped down (`decode_task_graph.cpp:70-72`). Zero admits no plane (`decode_task_graph.h:279`). | ja |
| Wavelet-Scratch | `InverseWavelet5To4` allocates `OwnedSignedPlane scratch(w, 4)` (`decode_spatial_scalar.cpp:156`). Storage is `::operator new(bytes_, std::align_val_t{32})` (`decode_spatial_scalar.cpp:109-116`), four rows (`157-158`). Freed by the local destructor when the call returns, including on cancel inside the row loop (`160-161`). Not a `DecodeTaskGraph` member. Not added to `owned_budget_`. | nein |
| Temporäre Inverse-Daten | Vertical/horizontal samples are written into the scratch rows and into the caller-owned lowpass (`decode_spatial_scalar.cpp:162-166`). `consume_wavelet5_lowpass` passes the caller's `OutputEdge` through (`decode_s3_consumer.cpp:62-65`) and does not allocate a graph buffer. The scalar header states that cancel or an exception may leave a partial output and that the caller must not publish it (`decode_spatial_scalar.h:55-56`). `consume_wavelet5_lowpass` itself returns `Canceled` / `Rejected` and does not publish (`decode_s3_consumer.cpp:54-69`). These bytes are outside `owned_budget_`. | nein |
| Generation-Wechsel | A graph is one immutable `Generation` (`decode_task_graph.h:285-286`, stored `389`). There is no API that replaces `generation_` or that moves `owned_bytes` to a successor graph. `cancel(generation)` returns false when the id does not match (`decode_task_graph.cpp:400-404`) and does not free the other graph. A new generation is a new object; the previous object's copies live until its destructor. The budget is not a process-wide pool and is not released by switching ids. | nein (kein Wechsel-Pfad; Budget stirbt nur mit dem Objekt) |
| Cancellation | `cancel` → `fail_locked(GraphError::Canceled, no_node)` (`decode_task_graph.cpp:400-404`, `377-398`). `fail_locked` sets `stop_`, marks the cause `Failed` when there is one, marks unstarted transitive dependents `Failed`, marks other pending/ready nodes `Canceled`, and notifies. It does not clear `owned_bytes`, does not decrement `owned_budget_`, and does not allocate. `owned_publication_bytes()` documents the same: `cancel()` does not release retained plane bytes (`decode_task_graph.h:327-329`). Already-running callbacks still finish; `complete` then marks that node `Canceled` if `stop_` is set (`decode_task_graph.cpp:413-416`). | nein (kein Rollback) |
| Error-Rollback | Three different outcomes. (1) Rejected before the copy: `publish` / `publish_plane` return false with version still 0; `publish_plane` has not incremented `owned_budget_` yet. A failed `assign` inside `commit_copy_locked` returns false before `swap` (`decode_task_graph.cpp:188-191`); `publish_plane` then zeroes the geometry fields it had staged (`242-246`) and does not charge. (2) First graph error or cancel after a successful copy: `fail_locked` (`377-398`) does not roll the copy or the charge back. Later `publish` / `publish_plane` calls refuse because `stop_` or `error_` is set (`168`, `218`), so the counter stops growing, but bytes already counted stay until the destructor. (3) Admission after stop: `admitted_input` returns an empty view when `stop_` or `error_` is set, even if a version is already non-zero (`decode_task_graph.cpp:316-318`). The bytes remain allocated; they are just not handed out. | nein für bereits committete Kopien; ja im engen Sinn, dass eine abgelehnte Kopie nicht zählt |

Not in the table, also unbudgeted: validation scratch (`decode_task_graph.cpp:41-60`), the node deque and successor vectors built in the constructor (`84-104`), the ready queue, and the `std::thread` vector in `run()` (`494-507`). None of them touch `owned_budget_`.

## What the typed path actually checks

`publish_plane` charges `bytes` (the caller span), which `check_signed_plane`
has already required to be at least `pitch * height` and at most 2 MiB
(`decode_s3_plane.h:66-70`). It does not charge a separate aligned capacity,
and it does not charge the consumer's scratch. `input_plane`
(`decode_task_graph.cpp:251-290`) re-checks generation, task key and extent. It
does not re-check 32-byte data alignment of the graph-owned pointer, and it
does not consult the budget.

`admit_owned_plane` (`decode_s3_plane.h:119-133`) is the S3 joint admission of
an already published `BufferVersion`. It forces `require_data_align=false`
(`129`). That flag exists only because the graph copy is a
`std::vector<unsigned char>` (`decode_s3_plane.h:47-50`). Pitch alignment and
the 2 MiB extent still apply. The budget is not consulted again on read.

## Alignment: `std::vector<unsigned char>` is not a 32-byte SIMD buffer

The scheduler does not contain `std::vector<uint8_t>`. The owned publication
buffer is `std::vector<unsigned char>` (`decode_task_graph.h:356` and the
temporary at `decode_task_graph.cpp:186`). `unsigned char` has alignment 1.
`std::allocator` is not asked for `kS3PlaneAlign` (32, `decode_s3_plane.h:23`).
Nothing in `commit_copy_locked` over-aligns, offsets, or swaps in an
`align_val_t{32}` allocation. The pointer stored for consumers is
`owned_bytes.data()` (`decode_task_graph.cpp:195`).

This audit did not sample the heap. No alignment number was measured. The
language rule used by this code is the gap: the vector is not required to be
32-byte aligned, and the SIMD consumer rejects a pointer that is not.

Concrete sites:

| Stelle | Was nicht garantiert ist |
| --- | --- |
| `decode_task_graph.h:356` | `Node::owned_bytes` is `std::vector<unsigned char>` with the default allocator. This is the address `input()` / `input_plane()` later return (`decode_task_graph.cpp:333-339`, `287-288`). |
| `decode_task_graph.cpp:186-195` | The copy that becomes that address is another `std::vector<unsigned char>`, filled with `assign` and swapped in. No aligned allocator, no `alignas(32)`, no `std::align`. |
| `decode_s3_plane.h:47-50` and `:128-129` | The S3 helper documents the hole and turns the 32-byte data check off for a graph-owned `BufferVersion` (`require_data_align=false`). Pitch alignment is still required. |
| `decode_s3_plane.h:51-56` | `check_signed_plane` defaults to requiring `kS3PlaneAlign` on `plane.data`. `publish_plane` runs that check on the **caller** pointer (`decode_task_graph.cpp:227-235`), then copies into the vector. The stored pointer is a different address and is not checked. |
| `decode_spatial_scalar.cpp:37-46` | `InverseWavelet5To4`'s `validate` throws unless `pitch` and the data pointer are both multiples of 32. A `PlaneView` / `BufferVersion` that still points at `owned_bytes.data()` is not a legal input to that primitive. |
| `tests/decode_task_graph/test_decode_s3_plane.cpp:269-271` | The S3 scheduler test says so in source and `memcpy`s the owned bytes into an `alignas(32)` host plane before `consume_wavelet5_lowpass`. The graph pointer is not passed to the inverse. |

Contrast, same consumer, different allocator: `OwnedSignedPlane`
(`decode_spatial_scalar.cpp:115-120`) uses `::operator new` / `::operator delete`
with `std::align_val_t{32}`. The wavelet scratch is aligned. The scheduler copy
the scratch would have to read, if a callback skipped the memcpy, is not.

`consume_wavelet5_lowpass` (`decode_s3_consumer.cpp:32-50`) calls
`check_signed_plane` with the default (`require_data_align=true`) on the
`PlaneEdge` the caller supplies. That rejects a misaligned caller buffer before
`InverseWavelet5To4`. It does not realign a graph-owned vector, and it does not
allocate a substitute plane.

## Phase 2

Not started in this commit.
