<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Synthetic decode task graph — issue 147

This isolated implementation follows the option-C scheduling contract in
[design PR 145](https://github.com/louis-0hrs/HyperCine/pull/145), reviewed design
commit `19e768f7e7096a4b02afbb2c75964950bc775bdd`. Its parent is current main
`e87ef0258ba37f5c78f1557c59d9c7331df5521c`. Coordination issue 62 and the open PR
inventory were checked before implementation; no competing scheduler claim was
present. Branch: `codex/issue147-decode-task-graph`.

## Boundary and ownership

`src/core/decode_task_graph.{h,cpp}` is private C++17 infrastructure. Every node
is required for success. It does not decode packets, implement codec arithmetic,
provide a public decoder API, or link the SDK/GPU libraries. Only the synthetic
profile is accepted. Real codec profiles remain unsupported. Dimensions, pixel
precision, buffer extents and pitches are deliberately not modelled; a future
validated frame/buffer adapter must check those before constructing this graph.
There are no codec sample buffers or vector loads in this slice.

The constructor accepts the complete immutable node inventory and producer keys.
Generation identity is session + slot + generation; node identity is channel +
transform + level + frame/field + output band. Missing callbacks/producers,
duplicate keys/edges, self/general cycles, empty graphs, invalid identities and
excessive topology are rejected with `invalid_argument` before execution.
Allocation failure unwinds with `bad_alloc`. Defaults/absolute ceilings are 4096
nodes and 65536 edges. Smaller caller limits are honored. Predecessor counts are
derived from actual edges, never accepted from an untrusted count. A highpass-only
node needs exactly its declared inputs. No universal band mask is applied.

Each graph runs at most once with 1–16 workers. All state and queue transitions
hold the mutex; callbacks execute without it. Producer writes precede completion's
mutex unlock, and consumer acquisition supplies the happens-before relation.
Only successful completed producers decrement successors. The ready queue is
reserved before execution. Queue exhaustion cannot imply success: all nodes must
be Completed. Unexpected unresolved work with no running producer fails closed.
Callbacks must not wait for work queued on their own graph's worker pool.

The synchronous worker owns completion. No asynchronous completion handle escapes
to clients; retaining `TaskContext` beyond the callback or starting unjoined child
work is unsupported. The private publication guard still rejects a foreign
generation, a non-Running node and repeated/too-late completion. Tests access
that private boundary through a friend peer, without adding a product API.

The owner allocates a fresh, nonzero, never-reused session/generation identity per
slot use; the graph does not own a session-wide generation counter. At exhaustion
the future session owner must retire the session rather than wrap the counter.
A stale cancellation/completion cannot affect a different graph generation.
The graph object and all callback storage must outlive `run()`; destruction
concurrent with `run()` is not supported.

## Failure, cancellation and storage lifetime

The first causal GraphError is retained. A throwing callback becomes Callback
or Allocation failure; its unstarted transitive dependents become Failed and
other queued jobs become Canceled. Worker startup/storage failures stop scheduling
and drain any workers already created. Invalid/repeated run requests return false
without modifying an active graph. Validation rejection is an exception, never a
skip or a runnable partially valid graph.

`cancel()` requests cooperative cancellation. `cancellation_requested()` can be
queried reentrantly by callbacks. A successful callback returning after cancel
becomes Canceled and cannot publish successors. Later exceptions cannot overwrite
the first error. Running nodes remain Running until their callbacks return;
`quiescent()` stays false until every worker thread is joined. Callback-owned
storage is retained by the graph through the drain and released on destruction.
No detached workers or background agents are created.

There is no in-process hard deadline guarantee. A stuck arbitrary C++ callback
can prevent `run()` from returning; callers must supply bounded/cooperative work.
Each test gate uses a three-second failure deadline and each CTest process has a
30-second watchdog. Process timeout is a test failure, not proof of safe in-process
teardown. Hard termination would require a separately reviewed process boundary.

## Deterministic verification

Each of the four CTests executes **20 scenarios**, at worker counts **1/2/4/16**
(80 scenario executions total). No random data or timing-based success oracle is
used. Negative scenarios require explicit rejection/failure classifications.

- A producer is held before writing a non-atomic output-valid flag and value.
  The controller observes the consumer Pending and untouched, then releases the
  producer. The consumer must read the committed data. Foreign completion is
  rejected while the producer is Running.
- Reversed declaration order, fan-out/fan-in, three channels and highpass-only
  dependencies exercise publication independently of an LL mask.
- Eight successive generations of the same slot reject stale/foreign messages,
  nonexistent nodes and premature/duplicate completion. Repeated run is rejected.
- Missing producers, duplicate keys/edges, self/cyclic dependencies, invalid
  profiles/identities/callbacks, node/edge limits and an injected impossible
  predecessor count all fail closed.
- Producer exceptions fail transitive dependents. Allocation failures are injected
  at every allocation position until constructor success, at worker-vector reserve,
  and inside a callback. Actual OS thread-creation failure is handled but was not
  forced by this suite.
- Pre-run cancellation executes no callbacks. Running cancellation suppresses
  completion and consumers. A weak-pointer fixture proves storage survives the
  requested cancellation and worker drain, then expires on graph destruction.
- The first producer failure is forced before a peer's later allocation failure;
  multi-worker cases hold that peer until after failure to verify drain state.
  With one worker the peer is queued and must be Canceled. Reentrant callbacks
  query state, reject resubmission and request cancellation without deadlock.

Native Windows x64, MSVC 19.44.35229, Release, 2026-10-18:

| Gate | Result |
| --- | --- |
| Full regular SDK/CPU CTest, existing gates unchanged | 14/14 PASS, zero skips, 2.06 s |
| SDK-free graph CTest | 4/4 PASS, zero skips |
| SDK-free graph MSVC AddressSanitizer | 4/4 PASS, zero skips, 0.32 s; no diagnostics |
| Native Linux GCC/Clang + TSan/ASan/UBSan | PENDING; no Linux/WSL installation available |
| Independent native macOS Clang | PENDING; unavailable on this host |

Windows ASan covers the graph and synthetic harness, not the upstream codec.
TSan/UBSan are not provided by this MSVC toolchain; no equivalent result is
inferred from ASan. Hosted checks are recorded separately on the Draft PR.
No acceleration/performance improvement is claimed, so benchmark_codec was not
run for this scheduler-only change.

Portable isolated build (CMake 3.25+):

```sh
cmake -S . -B build-graph -DHYPERCINE_BUILD_DECODE_TASK_GRAPH_ONLY=ON -DHYPERCINE_BUILD_UPSTREAM=OFF
cmake --build build-graph --config Release
ctest --test-dir build-graph -C Release --output-on-failure
```

For Linux sanitizer builds, use separate fresh build directories with
`-DCMAKE_CXX_FLAGS='-fsanitize=thread -fno-omit-frame-pointer'` or
`-DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'` and
matching `CMAKE_EXE_LINKER_FLAGS`. Do not combine TSan with ASan. These are
reproduction instructions, not results obtained on this Windows host.

The native MSVC ASan build used `/EHsc /fsanitize=address /Zi` and executable
linker flags `/INCREMENTAL:NO /DEBUG`, with the toolchain's ASan runtime directory
on PATH and `ASAN_OPTIONS=halt_on_error=1`.

## Downstream handoff

The held-producer, channel fan-in, generation, failure/drain and final all-node
completion fixtures are the contract anchors for later real packet regressions:

- Issue 120 / Draft 138 immutable 128 packet SHA prefix `60ed81a3...` and 256
  prefix `7655842a...`: exercise the lowpass producer-to-inverse-consumer edge.
  Retrieve full hashes and original immutable packets from the owner's evidence
  before integration; these prefixes alone do not authenticate input.
- Cross-host recreated 1080p packet SHA-256
  `2b9a28dec5b015bf76a1ba3bce5a458167bd07bd061dbb59a97c00eece4fec52`, source
  `45c166c3d6a3710d620ff5d738980224edf53cf9ce1bceb87267e474294afff1`:
  future whole-frame/channel completion and repeated-generation qualification.
  Windows issue 120 observed 15/15 stable short-run decodes; M4's Windows host
  observed original SDK CUDA-free variation in 9/20 processes × 8 calls.
  The original in-memory M4 failing invocation was not archived; the newly
  recreated SHA-matched packet is distinct evidence. Preserve both facts.

No packets/media or arbitrary substitute packet bytes are bundled here. These
tests establish no CFHD bitstream, pixel, PSNR or reference-decoder correctness.
Issue 144/16 owners must review the source/lifetime contracts before scalar
entropy/inverse/color stages or GPU adapters are added. The potential unchecked
CUDA event/wait/copy return statuses in PR 12's CheckAdvanceInverseDag remain
with its owner; this change neither touches nor qualifies that bridge. Protected
main, frozen upstream and the other decoder/SDK/GPU owner branches are untouched.
Issues 120/144 and production release gates remain open.
