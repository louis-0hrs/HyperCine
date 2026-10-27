<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->

# Decoder memory contract audit (synthetic scheduler)

**Status: the budget finding in the previous revision of this note was wrong
after `26730a2`.** That revision was read on
`44827e84f1658d67a506ecdd41d8d1c7ba7379c8` and then updated for the 32-byte
allocator. It said the budget split was unchanged, and it certified that
untyped `publish()` does not charge the generation budget. That certification
was true before `26730a2` (`fix(decoder): charge every owned publication and
release abandoned copies`). It is false on this tree. The line citations it
gave now point at different content. This revision says what the code does
now, and it says which earlier sentences were wrong instead of rewriting them
as if they had always been right.

This is synthetic scheduler infrastructure only. It is not a PASS for #144,
#195, or decoder readiness, and it makes no claim about the original SDK.
`upstream/` was not modified. The FilmScan3 encoder, frontend and backend
were not touched. This correction does not change codec source; two header
comments that still describe the old hole are named below and left in place.

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
(`decode_task_graph.h:265-266`), installed as `generation_budget_`
(`decode_task_graph.cpp:72-74`, member `decode_task_graph.h:450`). The only
counter is `owned_budget_` (`decode_task_graph.h:451`), exposed read-only as
`owned_publication_bytes()` (`decode_task_graph.cpp:119-121`). The header
comment on that accessor now matches the code: bytes retained by owned
publications, typed and untyped alike, with a release of every non-completed
node when `run()` returns on a stopped generation
(`decode_task_graph.h:366-371`). The previous revision cited
`decode_task_graph.h:366-368` as "bytes retained by successful
`publish_plane()` calls" only. That reading described the comment that used
to be there. It does not describe the comment or the counter on this tree.

## What the previous revision got wrong

> Die Budgetgrenze greift hauptsächlich für typisierte Plane-Kopien. Ältere
> untypisierte Buffer werden nicht über denselben Mechanismus abgerechnet.

The previous revision marked that **verified, not refuted**, and gave two
citations as the evidence:

- `GraphLimits::generation_budget_bytes` "Untyped `publish()` does not spend
  this budget." (`decode_task_graph.h:316-321`)
- `publish()` (`decode_task_graph.cpp:166-180`) never reads
  `generation_budget_` or `owned_budget_`.

Both citations are still those lines, and both were then used to conclude
that an untyped publication is free. That conclusion is false.

`publish()` is still `decode_task_graph.cpp:166-180`. It still refuses a
stopped generation, a second version, a null pointer and a zero length, and
the lines inside the function still do not mention the two counters. Line 180
is `return commit_copy_locked(target, data, bytes);`. The budget decision
moved into that callee in `26730a2`. Citing `166-180` as proof that the
publication does not pay was an incomplete reading of the call, and it
certified the hole the callee had just closed.

`commit_copy_locked()` (`decode_task_graph.cpp:185-216`) is the shared tail of
`publish()` and `publish_plane()`. Under `mutex_` it refuses before the copy
when `bytes > generation_budget_` or
`owned_budget_ > generation_budget_ - bytes` (`192`). A failed `assign`
returns false before `swap` (`195-199`) and does not charge. On success it
swaps the copy in (`201`) and then charges
`owned_budget_ += target.owned_bytes.size()` (`202`), which is the length
actually retained, not a separate aligned capacity. The version release-store
is after that charge (`213`).

`publish_plane()` (`decode_task_graph.cpp:221-259`) no longer adds to
`owned_budget_` itself. It keeps an early copy of the same predicate (`248`)
so the geometry fields are not written for a copy that cannot be paid for,
then calls `commit_copy_locked()` (`252`). A failed commit zeroes the geometry
it had staged (`253-256`) and does not leave a charge behind.

## Stale comments that still describe the hole

These two comments were not updated by `26730a2`. They contradict
`commit_copy_locked()`. They are not the contract:

- `decode_task_graph.h:316-321` still says the ceiling is "Bytes of
  graph-owned plane copies charged by `publish_plane()`" and "Untyped
  `publish()` does not spend this budget." The sentence "zero admits no
  plane" is also too narrow: a zero `generation_budget_` refuses every
  positive `bytes` in `commit_copy_locked()`, so it admits no owned
  publication, typed or untyped.
- `decode_task_graph.h:448-450` still says the effective ceiling is "Spent
  only by `publish_plane()`, under `mutex_`." The spend is under `mutex_`,
  but it is in `commit_copy_locked()`, which both publication paths call.

The comment that agrees with the code is `decode_task_graph.h:366-371` and
the `BUDGET-ALL` comment at `decode_task_graph.cpp:187-191`.

## Speicherpfad × budgetiert

"Budgetiert" means: a successful owned copy is checked against
`generation_budget_` and added to `owned_budget_` before the caller can
observe a published version. "Nein" means that ceiling is not applied. There
is no second scheduler ceiling hiding under another name.

The previous revision's table said the untyped path, cancellation and
committed-copy rollback were "nein". The untyped row was wrong after
`26730a2`. The cancellation and rollback rows were wrong about what is true
once `run()` has returned. `fail_locked()` itself still does not free.

| Speicherpfad | Stelle | budgetiert? |
| --- | --- | --- |
| Untypisierte S2-Graph-Buffer | `TaskContext::publish` → `DecodeTaskGraph::publish` (`decode_task_graph.cpp:124-125`, `166-180`) then `commit_copy_locked` (`185-216`). The copy is `OwnedBytes copy; copy.assign(...)` then `owned_bytes.swap` (`194-201`). The charge is `owned_budget_ += owned_bytes.size()` at `202`. A publish larger than the remaining ceiling returns false at `192`, before `assign`, and leaves version 0. The previous revision said this path never reads the counters and that the only failure was `assign` throwing. That was the pre-`26730a2` code. It is not this code. | ja |
| Typisierte S3/T4-Planes | `publish_plane` (`decode_task_graph.cpp:221-259`). Extent gate `check_signed_plane` at `244` (rules in `decode_s3_plane.h:53-72`, per-plane cap `kS3MaxPlaneBytes` = 2 MiB at `decode_s3_plane.h:24`). Early budget refusal at `248`, so geometry is not staged for a copy that cannot be paid for. The charge is not at `250`; `250` is `plane_height = height`. The charge is inside `commit_copy_locked` (`252`, `202`). Ceiling expression: 3 channels × 4 wavelet5 bands × 2 MiB plus one 2 MiB lowpass (`decode_task_graph.h:262-266`). A larger `generation_budget_bytes` is clamped down (`decode_task_graph.cpp:72-74`). Zero admits no owned publication. | ja |
| Wavelet-Scratch | `InverseWavelet5To4` allocates `OwnedSignedPlane scratch(w, 4)` (`decode_spatial_scalar.cpp:156`). Storage is `::operator new(bytes_, std::align_val_t{alignment})` (`decode_spatial_scalar.cpp:115-120`), four rows (`157-158`). Freed by the local destructor when the call returns, including on cancel inside the row loop (`160-161`). Not a `DecodeTaskGraph` member. Not added to `owned_budget_`. | nein |
| Temporäre Inverse-Daten | Vertical/horizontal samples are written into the scratch rows and into the caller-owned lowpass (`decode_spatial_scalar.cpp:162-166`). `consume_wavelet5_lowpass` passes the caller's `OutputEdge` through (`decode_s3_consumer.cpp:62-65`) and does not allocate a graph buffer. The scalar header states that cancel or an exception may leave a partial output and that the caller must not publish it (`decode_spatial_scalar.h:58`). `consume_wavelet5_lowpass` itself returns `Canceled` / `Rejected` and does not publish (`decode_s3_consumer.cpp:54-71`). These bytes are outside `owned_budget_`. | nein |
| Generation-Wechsel | A graph is one immutable `Generation` (`decode_task_graph.h:324-325`). There is no API that replaces `generation_` or that moves `owned_bytes` to a successor graph. `cancel(generation)` returns false when the id does not match (`decode_task_graph.cpp:409-413`) and does not free the other graph. A new generation is a new object. The budget is not a process-wide pool and is not released by switching ids. The previous revision added "Budget stirbt nur mit dem Objekt". That is still true of a generation that completes with `error_ == None`. It is not true of a stopped generation: `run()` refunds non-completed nodes before the destructor (see the next row). | nein (kein Wechsel-Pfad). Ein erfolgreicher Lauf hält bis zum Destruktor; ein gestoppter Lauf erstattet vorher. |
| Cancellation | `cancel` → `fail_locked(GraphError::Canceled, no_node)` (`decode_task_graph.cpp:409-413`, `386-408`). `fail_locked` sets `stop_`, marks the cause `Failed` when there is one, marks unstarted transitive dependents `Failed`, marks other pending/ready nodes `Canceled`, and notifies. It still does not clear `owned_bytes` and does not decrement `owned_budget_`. The previous revision stopped there and concluded there is no rollback. That is no longer the whole path. After every worker has been joined, `run()` calls `release_abandoned_locked()` when `error_ != None` (`decode_task_graph.cpp:520-524`). Cancel sets that error, so a canceled generation does release. A node already in `Completed` is kept. A node still `Running` is marked `Canceled` by `complete()` when `stop_` is set (`422-425`) and is then eligible for the release. | ja, aber erst wenn `run()` zurückkehrt, und nur für nicht-`Completed` Knoten. `cancel()` selbst gibt nichts frei. |
| Error-Rollback | Three different outcomes, and the second one changed. (1) Rejected before the copy: `publish` / `publish_plane` return false with version still 0. A failed `assign` inside `commit_copy_locked` returns false before `swap` (`195-199`); `publish_plane` then zeroes geometry (`253-256`). Nothing was charged. (2) The previous revision said a successful copy is never rolled back, and that bytes already counted stay until the destructor. `fail_locked` (`386-408`) still does not roll the copy back by itself. When `run()` then returns with `error_ != None`, `release_abandoned_locked` (`527-543`) drops every non-completed node's storage and refunds `owned_budget_` by that node's size (`542`), after clearing payload, `payload_bytes` and version (`534-536`) so a stale reader does not observe a dangling address. A node that already reached `Completed` keeps its charge until the destructor. (3) Admission after stop: `admitted_input` returns an empty view when `stop_` or `error_` is set, even if a version is already non-zero (`decode_task_graph.cpp:322-328`). Until `run()` returns, the bytes are still allocated; they are just not handed out. | ja für eine abgelehnte Kopie; ja für eine committete Kopie eines nicht-`Completed` Knotens, sobald `run()` beigetreten ist; nein für einen bereits `Completed` Knoten. |

Not in the table, also unbudgeted: validation scratch (`decode_task_graph.cpp:43-62`), the node deque and successor vectors built in the constructor (`86-106`), the ready queue, and the `std::thread` vector in `run()` (`506-509`). None of them touch `owned_budget_`.

## Release on stop, and the refund

`release_abandoned_locked()` (`decode_task_graph.cpp:527-543`) requires
`mutex_` and requires that every worker is already joined. `run()` is the
only caller, and only when `error_ != None` (`524`). For each node that is
not `Completed` and whose `owned_bytes` is non-empty it:

1. records `released = owned_bytes.size()`;
2. stores a null payload, zero `payload_bytes` and version 0;
3. swaps the buffer with an empty `OwnedBytes` and zeroes the plane extent;
4. refunds `owned_budget_ = owned_budget_ > released ? owned_budget_ - released : 0`.

The previous revision said `owned_publication_bytes()` documents that
`cancel()` does not release retained plane bytes. The comment it cited now
says the opposite (`decode_task_graph.h:366-371`): a stopped generation
releases every non-completed node's storage when `run()` returns, and the
counter drops to the completed nodes' total. The destructor releases whatever
remains, and that is only safe after `run()` has joined every worker.

A successful `run()` (`error_ == None`) does not call the release. Those
copies live until the graph is destroyed. That part of the old "dies with the
object" sentence is still true, and only for that path.

## What the typed path actually checks

`publish_plane` charges, via `commit_copy_locked`, the retained size of the
caller span. `check_signed_plane` has already required that span to be at
least `pitch * height` and at most 2 MiB (`decode_s3_plane.h:67-71`). The
charge is not a separate aligned capacity, and it is not the consumer's
scratch. `input_plane` (`decode_task_graph.cpp:260-299`) re-checks generation,
task key and extent. It does not re-check 32-byte data alignment of the
graph-owned pointer (`297` returns `view.data` with no alignment test), and
it does not consult the budget.

`admit_owned_plane` (`decode_s3_plane.h:120-144`) is the S3 joint admission of
an already published `BufferVersion`. The previous revision said it forces
`require_data_align=false` at what was then line 130, and that the comment at
`decode_s3_plane.h:47-51` explains why the flag stays off. Both sentences are
false on this tree. `26730a2` changed the call to
`match_plane_edge(..., /*require_data_align=*/true)` (`137`). The comment at
`decode_s3_plane.h:47-52` says admission no longer relaxes the flag, because a
graph-owned copy is allocated at `kOwnedBufferAlign` and a caller-supplied
descriptor that is not aligned is rejected here. Pitch alignment and the
2 MiB extent still apply. The budget is not consulted again on read.

## Alignment

The scheduler does not contain `std::vector<uint8_t>`. The owned publication
buffer is a `vector` of `unsigned char` (`decode_task_graph.h:398`, temporary
at `decode_task_graph.cpp:194`). `unsigned char` has alignment 1. A default
`std::allocator<unsigned char>` does not request `kS3PlaneAlign`
(32, `decode_s3_plane.h:23`).

On `44827e8` the owned buffer was that default vector. The allocator fix
(the commit before `26730a2`) did not close the budget split. The previous
revision was explicit that it was closing alignment only, and then it restated
the budget split as still the contract. The alignment half of that paragraph
is still right. The budget sentence is the one `26730a2` made false.

`Node::owned_bytes` is `OwnedBytes`, a `std::vector<unsigned char,
OwnedBufferAllocator<unsigned char>>` (`decode_task_graph.h:274-301` and
`:398`). `allocate` uses `::operator new(n, std::align_val_t{kOwnedBufferAlign})`
(`decode_task_graph.h:287`). `commit_copy_locked` builds the same type
(`decode_task_graph.cpp:194`). `static_assert(kOwnedBufferAlign == kS3PlaneAlign)`
is at `decode_task_graph.cpp:15-16`.

`check_signed_plane` (`decode_s3_plane.h:53-72`) requires 32-byte alignment of
the **caller** pointer when `require_data_align` is true, which is now the
default. `publish_plane` runs that check before the copy
(`decode_task_graph.cpp:244`). The stored address is a different pointer.
`input_plane` returns it (`decode_task_graph.cpp:297`) without an alignment
gate. `InverseWavelet5To4` throws unless both pitch and the data pointer are
multiples of 32 (`decode_spatial_scalar.cpp:37-38`).

`OwnedSignedPlane` (`decode_spatial_scalar.cpp:115-120`) was already a
32-byte `::operator new`. The wavelet scratch was aligned. The S3 test still
copies into a distinct host plane before the inverse
(`tests/decode_task_graph/test_decode_s3_plane.cpp:269-272`) because overlapping
planes are rejected; that copy is not the alignment fix.

`consume_wavelet5_lowpass` (`decode_s3_consumer.cpp:29-50`) calls
`check_signed_plane` with alignment required on the `PlaneEdge` the caller
supplies. It does not allocate a substitute plane.

The previous revision recorded this output from the binary **before**
`OwnedBufferAllocator` existed, workers=1:

```text
RULE T4-owned-align BROKEN run=1 error=0 seen=16 bad=9
SCHEDULER-T4 FAIL workers=1 holds=15 failures=1
```

That binary is not this tree. This revision did not rebuild it and did not
re-run it. Those two lines are UNVERIFIED here. They are kept so the earlier
observation is not silently deleted.

## Verification run for this revision

Re-run on this tree, standalone Release, upstream off, CUDA off, tests on.
Filter `scheduler_t4|decode_s3|task_graph`: 9 tests, 9 passed, ctest exit 0.
That filter is `decode_task_graph` at 1, 2, 4 and 16 workers, `decode_s3_plane`,
and `scheduler_t4_plane_144` at 1, 2, 4 and 8 workers. It does not include
`test_scheduler_s2_output_144`, `test_scheduler_t1_t2_144` or
`test_scheduler_contract_195`. The previous revision's claim that those three
exited 0 at workers=1 was not re-run. UNVERIFIED in this revision.

The T4 executable was also run directly so the summary lines below are the
ones observed, not the ctest pass regex alone.

workers=1, exit 0. Among the 19 holds:

```text
RULE T4-owned-align HOLDS sixteen graph-owned plane copies were 32-byte aligned
RULE T4-budget-all HOLDS the second untyped publication was refused by the same ceiling; owned bytes match
RULE T4-release-abandoned HOLDS a completed-then-failed publication was released and its charge refunded
SCHEDULER-T4 PASS workers=1 holds=19 failures=0
```

workers=8, exit 0. The same three rules held, and the summary was:

```text
SCHEDULER-T4 PASS workers=8 holds=19 failures=0
```

`T4-budget-all` is the untyped charge: the second `publish()` of a span that
does not fit `generation_budget_bytes` is refused and
`owned_publication_bytes()` stays at the first span. `T4-release-abandoned`
is the refund: a node that publishes and then throws leaves
`owned_publication_bytes() == 0` after `run()` returns, and its consumer does
not run. ctest also reported `scheduler_t4_plane_144` Passed at 2 and 4
workers. Those two processes were not captured line-by-line here, so this
note does not quote a holds count for them.

No FilmScan3 or upstream binary was part of the claim. This note is still not
decoder readiness for #144 or #195.
