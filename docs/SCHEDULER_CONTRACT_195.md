<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Scheduler contract derived from RCA-195

**Status: applied to this repository's wrapper scheduler (`src/core/decode_task_graph.*`), measured, with one documented gap.**
No frozen `upstream/` change. No claim about the reference decoder's internals.

## 0. Where this comes from, and what it is not

PR #197 produced a bounded evidence milestone on the *frozen reference decoder*: an
induced false-OKAY proof, a design model, and a scratch-SDK trial that failed closed
after one correction. That evidence is accepted; the decoder is **not** fixed, and
`#120` remains `REFERENCE_DECODER_NONDETERMINISTIC / UNRESOLVED`.

This document does something narrower and more useful: it takes the rules that
evidence *earned* and applies them to the scheduler we own. The rules below are
checked against the real implementation by `tests/test_scheduler_contract_195.cpp`,
which prints `HOLDS`, `GAP` or `BROKEN` per rule and is a CTest gate.

The distinction matters. A rule that is only written down can rot silently; a rule
that a test evaluates cannot. RCA-195's own history is the cautionary case: a green
`MODEL PASS` was printed while a control failure list was discarded.

## 1. The five findings, and the rule each one becomes

Each rule names the specific RCA-195 finding that produced it, so the contract can
be re-derived rather than trusted.

### S1 — Validate the whole topology before any worker runs

*From:* the reference decoder's queue accepted entries whose producer relation was
never checked; a transform could be admitted before its real input existed.

The graph must reject a missing producer, a duplicate key, a duplicate predecessor
edge and any cycle **at construction**, with `invalid_argument`, before a worker
thread exists. Rejecting after execution has begun means partial work exists with
no owner.

Tested: missing producer and cycle rejected at construction. Predecessor counts are
derived from actual edges; a caller cannot supply them.

### S2 — A node's completion means its output exists, not that it left the queue

*From:* the central RCA-195 finding. `done = 1` recorded **admission**, the queue
counter recorded that the entry **left the queue**, and neither recorded that the
transform **ran and wrote**. That is why "claimed" and "finished" were
indistinguishable and why a discarded entry still produced a public `OKAY`.

> **GAP (documented, not hidden).** The wrapper graph sets `Completed` when the
> callback returns without throwing. It cannot tell "wrote its output" from "did
> nothing", so a silent no-op is reported as success and its consumers run.

This is the same defect at our layer that RCA-195 found in the decoder's, and it is
the reason this rule is first among the gaps rather than a footnote. The graph is
not wrong to trust its callback — it is wrong to *claim* the output exists.

**Smallest step that closes it** (not implemented here, deliberately): require a
completion predicate per task — `bool produced()` evaluated under the lock before
`Completed` is set — with the default being "no predicate, caller asserts
production". A predicate that returns false is a terminal `GraphError`, not a
retry, and its consumers must never be released. Anything less reintroduces exactly
the state the reference decoder was stuck in.

### S3 — Cancellation drains, and a cancelled graph never reports success

*From:* the reference decoder's `WaitForTransformThread` zeroed the queue without
asking whether anything in it was unfinished, so a dropped transform became an
ordinary success.

`cancel()` must be observable to a running callback, must stop scheduling new work,
must let already-started work finish, and must make `run()` return false with
`GraphError::Canceled`. No thread may outlive `run()`.

Tested: cancel is visible while running, `quiescent()` is false during the drain and
true after, the error is `Canceled`, and a successful callback that completes after
cancellation is discarded.

### S4 — A failed producer never releases its consumers

*From:* the reference decoder's silent-guard behaviour — an inverse handed a wavelet
missing a band returned without writing and without setting an error, so the caller
reported OKAY and downstream work read uninitialised data.

A callback that throws marks the node `Failed`, stops scheduling, and marks its
unstarted transitive dependents `Failed` while cancelling unrelated queued work.
Consumers of a failed producer must never execute.

Tested: the consumer's callback is never entered, the producer is `Failed`, and the
graph error is the thrown class.

### S5 — Exactly one run, and success means *all* nodes completed

*From:* the reference decoder reporting a frame as decoded while entries had been
discarded; and the model's `I4_BOUNDED`, which only meant something once the search
actually exhausted its frontier.

`run()` succeeds only when every node reached `Completed` and no error was recorded.
A graph submits once; a second `run()` is refused. An impossible predecessor count
fails closed rather than waiting forever.

Tested: a complete graph runs once and refuses re-submission; a corrupted count
fails closed with `InvalidGraph`.

### S6 — The first error is the one reported, in either arrival order

*From:* RCA-195 iteration 1/3. A check that recorded the right error and was then
overwritten by a later local assignment produced a public `OKAY`; and a guard that
merely refused to overwrite with `OKAY` still let a later *different* failure
replace the first one.

The scheduler must keep the **first** recorded error. Both arrival orders must be
exercised, because an experiment that only ever produces one error at a time cannot
distinguish first-error from last-error semantics — that was a real review finding.

Tested: with the order *constructed* (the second failure waits until the first error
is visible, so "first" is a fact rather than a race), both `Allocation` first and
`Callback` first survive a later failure.

**A note on how this rule was almost mis-tested.** The first version of the probe
let both failures race and interpreted whoever won as "first"; it reported a false
`GAP`. Deterministic construction reversed the finding. Building the state the rule
talks about is the difference between measuring and guessing.

## 2. Result against this implementation

| Rule | Mechanism | Result |
| --- | --- | --- |
| S1 | reject missing producer / cycle before execution | HOLDS |
| S2 | completion implies output exists | **GAP** |
| S3 | cancel drains, never reports success | HOLDS |
| S4 | failed producer releases no consumer | HOLDS |
| S5 | run once; success requires all nodes | HOLDS |
| S6 | first error sticky, both orders | HOLDS (both orders) |

Six rules hold, one is a documented gap, none is broken.

The gap is reported as a gap. Reporting it as a pass — which is what the reference
decoder's false-OKAY did, and what the rev-5 control did when it labelled a
truncated search `ok` — would be the same error in a new place.

## 3. What this contract deliberately does not claim

- **No memory-ordering proof.** The callback-to-consumer edge relies on the mutex
  acquire/release pairing that the implementation already uses. No C11/C++ atomic
  order is verified, and the ABA question raised against pointer-identity tokens in
  #197 applies here too: a token can be reused.
- **No exhaustive interleaving coverage.** Six rules are checked on constructed
  scenarios at 1/2/4/16 workers, not over every reachable schedule.
- **No statement about the frozen decoder.** `upstream/` is untouched; this is the
  wrapper's contract, and the reference decoder keeps its open `#120` status.
- **No claim that a green run means safe.** It means these six rules held on these
  scenarios. A seventh rule that nobody wrote down would still be missing.

## 4. Reproducing

```
cmake -S . -B build -DHYPERCINE_BUILD_TESTS=ON
cmake --build build --target test_scheduler_contract_195
ctest --test-dir build -R scheduler_contract --output-on-failure
```

A `BROKEN` line fails the gate. A `GAP` line does not fail it — it is a known,
named shortfall, and closing it requires changing the implementation rather than
the test.

## 5. Follow-up

S2 is the one rule this repository does not yet meet. The smallest change that
closes it is a per-task completion predicate evaluated under the lock before
`Completed` is set, with a false result becoming a terminal `GraphError` that
releases no consumers.

It is not implemented in this change, and the reason is stated precisely rather
than conveniently: the graph has **no production caller** (`src/` never references
it), but it is exercised by a second test, `tests/test_decode_spatial_scalar.cpp`,
which drives real scalar wavelet arithmetic (`InverseWavelet5To4`) through it. So
the gap is not theoretical — that test has to carry its own `published` flag to
assert that a downstream consumer ran "after arithmetic completion", which is
exactly the responsibility S2 says the graph should hold itself. Adding the
predicate API before any production consumer exists would fix the shape of the
solution before the requirement is real; recording the rule means the first
consumer inherits it instead of rediscovering it.
