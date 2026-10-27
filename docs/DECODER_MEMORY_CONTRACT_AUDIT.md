<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Decoder memory contract audit (synthetic scheduler)

**Status: source audit plus one alignment fix.** The budget finding below was
read on `44827e84f1658d67a506ecdd41d8d1c7ba7379c8` (`origin/main`) and is
unchanged by the later commit. Line numbers are the tree after that fix.
Phase 1 of this note did not build or test. Phase 2 ran only the synthetic
scheduler binaries named at the bottom. That is not a PASS for #144, #195, or
the rest of the tree. `upstream/` was not modified. The FilmScan3
encoder/frontend/backend files were not touched.

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
(`decode_task_graph.cpp:72-74`, member `decode_task_graph.h:440`). The only
counter is `owned_budget_` (`decode_task_graph.h:441`), exposed read-only as
`owned_publication_bytes()` (`decode_task_graph.cpp:119-121`). The header states
what that counter means: bytes retained by successful `publish_plane()` calls
(`decode_task_graph.h:366-368`).

## Claim

> Die Budgetgrenze greift hauptsächlich für typisierte Plane-Kopien. Ältere
> untypisierte Buffer werden nicht über denselben Mechanismus abgerechnet.

**Verified, not refuted.** Typed plane copies are the only publications that
consult and increment the generation budget. Untyped S2 copies use the same
owned-vector storage and skip that counter entirely. The split is explicit in
the API comment, not an accident of a missed call:

- `GraphLimits::generation_budget_bytes` is documented as charged by
  `publish_plane()` only. "Untyped `publish()` does not spend this budget."
  (`decode_task_graph.h:316-321`)
- `publish()` (`decode_task_graph.cpp:166-180`) refuses a stopped generation, a
  second version, a null pointer and a zero length, then calls
  `commit_copy_locked`. It never reads `generation_budget_` or `owned_budget_`.
- `publish_plane()` (`decode_task_graph.cpp:240-250`) rejects the copy when
  `bytes > generation_budget_` or `owned_budget_ > generation_budget_ - bytes`,
  and only then adds `bytes` to `owned_budget_`.

Both paths install the bytes through `commit_copy_locked`
(`decode_task_graph.cpp:185-209`) into `Node::owned_bytes`. The storage is
shared. The ledger is not.

## Speicherpfad × budgetiert

"Budgetiert" means: the successful allocation is checked against
`generation_budget_` and added to `owned_budget_` before the caller can
observe a published version. "Nein" means that ceiling is not applied. There
is no second scheduler ceiling hiding under another name.

| Speicherpfad | Stelle | budgetiert? |
| --- | --- | --- |
| Untypisierte S2-Graph-Buffer | `TaskContext::publish` → `DecodeTaskGraph::publish` (`decode_task_graph.cpp:124-125`, `166-180`). Copy in `commit_copy_locked` (`185-209`): `OwnedBytes copy; copy.assign(...)` then `owned_bytes.swap` (`195-197`). Node field: `decode_task_graph.h:396`. No read of `generation_budget_` / `owned_budget_` on this path. A single untyped publish may be larger than `kPlaneGenerationBudget`; the only failure is `assign` throwing, which returns false and leaves version 0 (`190-193`). | nein |
| Typisierte S3/T4-Planes | `publish_plane` (`decode_task_graph.cpp:214-251`). Extent gate `check_signed_plane` at `237` (rules in `decode_s3_plane.h:51-71`, per-plane cap `kS3MaxPlaneBytes` = 2 MiB at `decode_s3_plane.h:24`). Budget gate at `decode_task_graph.cpp:240`. Charge `owned_budget_ += bytes` at `250`, after the copy succeeds. Ceiling expression: 3 channels × 4 wavelet5 bands × 2 MiB plus one 2 MiB lowpass (`decode_task_graph.h:259-263`). A larger `generation_budget_bytes` is clamped down (`decode_task_graph.cpp:72-74`). Zero admits no plane (`decode_task_graph.h:318`). | ja |
| Wavelet-Scratch | `InverseWavelet5To4` allocates `OwnedSignedPlane scratch(w, 4)` (`decode_spatial_scalar.cpp:156`). Storage is `::operator new(bytes_, std::align_val_t{32})` (`decode_spatial_scalar.cpp:109-116`), four rows (`157-158`). Freed by the local destructor when the call returns, including on cancel inside the row loop (`160-161`). Not a `DecodeTaskGraph` member. Not added to `owned_budget_`. | nein |
| Temporäre Inverse-Daten | Vertical/horizontal samples are written into the scratch rows and into the caller-owned lowpass (`decode_spatial_scalar.cpp:162-166`). `consume_wavelet5_lowpass` passes the caller's `OutputEdge` through (`decode_s3_consumer.cpp:62-65`) and does not allocate a graph buffer. The scalar header states that cancel or an exception may leave a partial output and that the caller must not publish it (`decode_spatial_scalar.h:55-56`). `consume_wavelet5_lowpass` itself returns `Canceled` / `Rejected` and does not publish (`decode_s3_consumer.cpp:54-69`). These bytes are outside `owned_budget_`. | nein |
| Generation-Wechsel | A graph is one immutable `Generation` (`decode_task_graph.h:324-325`, stored `429`). There is no API that replaces `generation_` or that moves `owned_bytes` to a successor graph. `cancel(generation)` returns false when the id does not match (`decode_task_graph.cpp:402-406`) and does not free the other graph. A new generation is a new object; the previous object's copies live until its destructor. The budget is not a process-wide pool and is not released by switching ids. | nein (kein Wechsel-Pfad; Budget stirbt nur mit dem Objekt) |
| Cancellation | `cancel` → `fail_locked(GraphError::Canceled, no_node)` (`decode_task_graph.cpp:402-406`, `379-400`). `fail_locked` sets `stop_`, marks the cause `Failed` when there is one, marks unstarted transitive dependents `Failed`, marks other pending/ready nodes `Canceled`, and notifies. It does not clear `owned_bytes`, does not decrement `owned_budget_`, and does not allocate. `owned_publication_bytes()` documents the same: `cancel()` does not release retained plane bytes (`decode_task_graph.h:366-368`). Already-running callbacks still finish; `complete` then marks that node `Canceled` if `stop_` is set (`decode_task_graph.cpp:415-418`). | nein (kein Rollback) |
| Error-Rollback | Three different outcomes. (1) Rejected before the copy: `publish` / `publish_plane` return false with version still 0; `publish_plane` has not incremented `owned_budget_` yet. A failed `assign` inside `commit_copy_locked` returns false before `swap` (`decode_task_graph.cpp:190-193`); `publish_plane` then zeroes the geometry fields it had staged (`244-248`) and does not charge. (2) First graph error or cancel after a successful copy: `fail_locked` (`379-400`) does not roll the copy or the charge back. Later `publish` / `publish_plane` calls refuse because `stop_` or `error_` is set (`170`, `220`), so the counter stops growing, but bytes already counted stay until the destructor. (3) Admission after stop: `admitted_input` returns an empty view when `stop_` or `error_` is set, even if a version is already non-zero (`decode_task_graph.cpp:321`). The bytes remain allocated; they are just not handed out. | nein für bereits committete Kopien; ja im engen Sinn, dass eine abgelehnte Kopie nicht zählt |

Not in the table, also unbudgeted: validation scratch (`decode_task_graph.cpp:43-62`), the node deque and successor vectors built in the constructor (`86-106`), the ready queue, and the `std::thread` vector in `run()` (`499`). None of them touch `owned_budget_`.

## What the typed path actually checks

`publish_plane` charges `bytes` (the caller span), which `check_signed_plane`
has already required to be at least `pitch * height` and at most 2 MiB
(`decode_s3_plane.h:66-70`). It does not charge a separate aligned capacity,
and it does not charge the consumer's scratch. `input_plane`
(`decode_task_graph.cpp:253-292`) re-checks generation, task key and extent. It
does not re-check 32-byte data alignment of the graph-owned pointer, and it
does not consult the budget.

`admit_owned_plane` (`decode_s3_plane.h:119-134`) is the S3 joint admission of
an already published `BufferVersion`. It forces `require_data_align=false`
(`130`). That waiver is for an arbitrary `BufferVersion`, not a second budget.
Pitch alignment and the 2 MiB extent still apply. The budget is not consulted
again on read. The comment at `decode_s3_plane.h:47-51` says why the flag stays
off even after the graph copy itself is aligned.

## Alignment

The scheduler does not contain `std::vector<uint8_t>`. The owned publication
buffer is a `vector` of `unsigned char` (`decode_task_graph.h:396`, temporary
at `decode_task_graph.cpp:188`). `unsigned char` has alignment 1. A default
`std::allocator<unsigned char>` does not request `kS3PlaneAlign`
(32, `decode_s3_plane.h:23`).

On `44827e8` that was the whole allocation. `commit_copy_locked` filled a
default `std::vector<unsigned char>` and published `owned_bytes.data()`
(`decode_task_graph.cpp:197` in this tree; the same store, previously a default
vector). `check_signed_plane` (`decode_s3_plane.h:51-56`) requires 32-byte
alignment of the **caller** pointer, and `publish_plane` runs that check before
the copy (`decode_task_graph.cpp:229-237`). The stored address is a different
pointer. `input_plane` returns it (`decode_task_graph.cpp:290`) without an
alignment gate. `InverseWavelet5To4` throws unless both pitch and the data
pointer are multiples of 32 (`decode_spatial_scalar.cpp:37-46`).
`admit_owned_plane` still passes `require_data_align=false` (`decode_s3_plane.h:130`),
so a hand-built view is not forced through that gate.

`OwnedSignedPlane` (`decode_spatial_scalar.cpp:115-120`) was already a
32-byte `::operator new`. The wavelet scratch was aligned. The scheduler copy
was not. The S3 test still copies into a distinct host plane before the inverse
(`tests/decode_task_graph/test_decode_s3_plane.cpp:269-272`) because overlapping
planes are rejected; that copy is not the alignment fix.

`consume_wavelet5_lowpass` (`decode_s3_consumer.cpp:32-50`) calls
`check_signed_plane` with alignment required on the `PlaneEdge` the caller
supplies. It does not allocate a substitute plane.

## Phase 2

Smallest additive close of the alignment hole, not of the budget split. The
budget table above is still the contract: untyped `publish()` does not spend
`owned_budget_`.

`Node::owned_bytes` is now `OwnedBytes`, a `std::vector<unsigned char,
OwnedBufferAllocator<unsigned char>>` (`decode_task_graph.h:274-301` and
`:396`). `allocate` uses `::operator new(n, std::align_val_t{32})`
(`decode_task_graph.h:287`). `commit_copy_locked` builds the same type
(`decode_task_graph.cpp:188`). `static_assert(kOwnedBufferAlign == kS3PlaneAlign)`
is at `decode_task_graph.cpp:15-16`.

Negative test, before that allocator existed, workers=1:

```text
RULE T4-owned-align BROKEN run=1 error=0 seen=16 bad=9
SCHEDULER-T4 FAIL workers=1 holds=15 failures=1
```

Sixteen legal planes were published and read back. Nine of the graph-owned
addresses were not 32-byte aligned. The other T4 rules on that same binary
held. After the allocator, `T4-owned-align` held for workers 1, 2, 4 and 8
(`SCHEDULER-T4 PASS`, 16 holds, 0 failures). Also run, workers=1 only, all
exited 0: `test_scheduler_s2_output_144`, `test_scheduler_t1_t2_144`,
`test_decode_s3_plane`, `test_scheduler_contract_195`, `test_decode_task_graph`.
No other worker counts were run for those five. No FilmScan3 or upstream
binary was built.
