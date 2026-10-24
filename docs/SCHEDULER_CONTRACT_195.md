<!-- Copyright (c) 2026 HyperCine Contributors.
     Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE. -->
# Scheduler contract derived from RCA-195

**Status: implemented and measured against this repository's wrapper scheduler
(`src/core/decode_task_graph.*`). Every rule below is a required CTest gate. Revision 3
adds the S6 execution witness, the S2 lifetime contract and its rule; still Draft, not
promoted.**
No frozen `upstream/` change. No claim about the reference decoder's internals.

## 0. Where this comes from, and what it is not

PR #197 produced a bounded evidence milestone on the *frozen reference decoder*: an
induced false-OKAY proof, a design model, and a scratch-SDK trial that failed closed
after one correction. That evidence is accepted; the decoder is **not** fixed, and
`#120` remains `REFERENCE_DECODER_NONDETERMINISTIC / UNRESOLVED`.

This document does something narrower: it takes the rules that evidence *earned* and
applies them to the scheduler we own, then measures them.

The distinction matters. A rule that is only written down can rot silently; a rule a
test evaluates cannot. RCA-195's own history is the cautionary case: a green
`MODEL PASS` was printed while a control failure list was discarded.

## 1. Gate semantics, and why this revision changed them

The first version of this contract classified S2 as a **documented gap**: the test
printed `GAP`, `main()` printed `SCHEDULER-CONTRACT PASS`, and the process exited 0
while an unambiguous false-OKAY mechanism had just been exercised. That is a useful
*characterization* and a worthless *safety gate* — it reproduces, at our layer, the
exact classification error RCA-195 is about. A reviewer read it that way, correctly.

This revision has no such path:

- **Every rule is required.** `required()` prints `HOLDS` or `BROKEN`; there is no
  `documented_gap()` helper left in the file.
- A `BROKEN` line fails the gate: exit 1, no `PASS` string emitted, so the CTest
  `PASS_REGULAR_EXPRESSION` cannot be satisfied.
- S2 is **implemented**, not deferred (§3).
- The rule identifiers are the same in this document, the test output and the PR
  body. They previously disagreed (the body said S1–S6 while the binary printed
  S1,S3,S4,S5,S6,S7), which is its own kind of false report: a reader could not map
  a printed result to a published claim.
- The worker count is a real parameter. CTest registers **1, 2, 4 and 16** workers
  as four separate required tests; a concurrency rule proven only at one width is an
  untested rule at the others.

## 2. The findings and the rule each becomes

### S1 — Validate the whole topology before any worker runs

*From:* the reference decoder's queue accepted entries whose producer relation was
never checked, so a transform could be admitted before its real input existed.

Rejected at construction with `invalid_argument`, before a worker thread exists:
missing producer, cycle, self-cycle, duplicate key, duplicate predecessor edge, and a
task with no callback. Rejecting after execution begins means partial work exists
with no owner.

### S2 — A node's completion means its output exists

*From:* the central RCA-195 finding. `done = 1` recorded **admission**, the queue
counter recorded that the entry **left the queue**, and neither recorded that the
transform **ran and wrote**. That is why "claimed" and "finished" were
indistinguishable, and why a discarded entry still produced a public `OKAY`.

`TaskSpec` now carries an explicit publication contract:

```cpp
struct TaskSpec {
    TaskKey key;
    std::vector<TaskKey> inputs;
    std::function<void(const TaskContext&)> execute;
    std::function<bool()> published;   // output-exists predicate
};
```

The worker that ran `execute()` evaluates `published()` **on the same thread,
immediately after the callback**, and hands the result to `complete()`.

**Lifetime: what is guaranteed and what is not.** `execute()` has already returned when
`published()` is evaluated, so the callback's *frame is gone*. Capturing a local of the
callback body is a dangling read, not a supported pattern — an earlier revision of these
comments claimed the frame "and any storage it produced" was still live, which was
simply wrong. Measured: a probe that captured the address of a callback local read the
dead slot and reported success in a Release build, while AddressSanitizer classified the
same read as **`stack-use-after-return`**.

The real requirement is on the produced storage, not the frame: it must be owned
independently of the callback (heap object, shared ownership, or a member of a
longer-lived owner) and must stay alive from publication until `run()` has drained and
downstream consumers are finished with it. The graph never owns or frees payload memory;
the caller does. This is now the required rule
`S2-predicate-reads-owned-storage`. A `false` result — or a predicate
that throws, because a check that cannot answer is not evidence of an output — becomes
the terminal error `GraphError::OutputNotPublished`. The node becomes `Failed`, its
transitive dependents become `Failed`, and **no successor is released**.

Evaluated without the graph lock: the predicate may read callback-produced state and
may call the const query API. The `Completed` transition it gates is still made under
the lock, in `complete()`.

The escape hatch stays visible: a task with no `published` predicate is *counted* by
`uncontracted_tasks()`, and every fixture in the contract test asserts that count is
zero. An implementation that quietly stops declaring its output contract fails the
gate.

Measured:

| Rule identifier | Scenario | Requirement |
| --- | --- | --- |
| `S2-silent-no-op-fails-closed` | callback returns normally, writes nothing | `run()` false, `OutputNotPublished`, dependent never runs, both nodes `Failed`, graph drains |
| `S2-throwing-predicate-fails-closed` | predicate throws | same, fail-closed rather than "check skipped" |
| `S2-publish-before-consume` | real write then read over a dependency edge | both nodes `Completed`, consumer observed the value |
| `S2-transitive-tail-fails` | false predicate in the middle of three nodes | tail never runs, tail `Failed` |
| `S2-predicate-reads-owned-storage` | predicate reads caller-owned storage that outlives the callback frame | publishes correctly, consumer observes the written value, both nodes `Completed` |

**What S2 still cannot do.** The predicate is a *declaration*: a callback that sets its
own flag without writing the buffer is still reported as published. The graph can check
what the task declares, not what the memory contains. Closing that needs a buffer
contract the graph would have to own; it is out of scope here and is stated as a limit
rather than implied as coverage.

### S3 — Cancellation drains, and a cancelled graph never reports success

*From:* `WaitForTransformThread` zeroed the queue without asking what was unfinished,
so a dropped transform became an ordinary success.

`cancel()` is observable to a running callback, stops scheduling, lets started work
finish, and makes `run()` return false with `GraphError::Canceled`. No thread outlives
`run()`. A callback that completes successfully *after* cancellation is discarded
(`Canceled`, not `Completed`).

### S4 — A failed producer never releases its consumers

*From:* an inverse handed a wavelet missing a band returned without writing and
without setting an error, so the caller reported OKAY and downstream work read
uninitialised data.

A throwing callback marks its node `Failed`, stops scheduling, marks unstarted
transitive dependents `Failed`, and cancels unrelated queued work. Consumers of a
failed producer never execute.

### S5 — Exactly one run, and success means *every* node completed

*From:* a frame reported decoded while entries had been discarded; and `I4_BOUNDED`,
which only meant something once the search actually exhausted its frontier.

`run()` succeeds only when every node reached `Completed` and no error was recorded.
A second `run()` is refused. An impossible predecessor count fails closed with
`InvalidGraph` rather than waiting forever.

### S6 — The first error is the one reported, in either arrival order

*From:* RCA-195 iterations 1 and 3. A correct check overwritten by a later local
assignment produced a public `OKAY`; then a guard that merely refused to overwrite with
`OKAY` still let a later *different* failure win.

Both arrival orders are exercised, and the order is **constructed**: the later failure
waits until the first error is visible, so "first" is a fact rather than a winner. A
publication failure is also sticky against a later callback failure, because
`OutputNotPublished` is terminal in the same way.

**A note on how this rule was almost mis-tested.** The first version of the probe let
both failures race and interpreted whoever won as "first"; it reported a false `GAP`.
Deterministic construction reversed the finding. Building the state a rule talks about
is the difference between measuring and guessing.

**Second mis-test: a constructed order is still not a witnessed event.** Constructing
*when* the later failure fires does not construct *that it fires at all*.
`fail_locked()` stops dispatch, so once the first error is recorded the other root task
can be cancelled before any worker claims it. The rule then observed
`first != None && final == first` and reported `HOLDS` without a second failure having
happened. Measured on the previous revision with an instrumented probe: in the
allocation-first arm the later callback was claimed **0 times in 12/12 runs at 1, 2, 4
and 16 workers**, while the rule passed every time.

The arms now differ by worker width, because only one claim is honest at each width:

- **workers >= 2** — both roots can be in flight. A two-party start handshake forbids
  either callback from failing before both are claimed, and a flag set immediately
  before the later throw makes "a later failure was survived" a witnessed fact. A
  missing witness is `BROKEN`, never `HOLDS`.
- **workers == 1** — the graph is strictly serial, so the later task can never be
  claimed once the first fails. Asserting a survived later failure there would report a
  fabricated event. The rule instead asserts the reachable outcome: the later task
  never ran and is `Canceled`.

Both waits in S3 are bounded, on different mechanisms. The caller's ready handshake uses
the shared `wait_for(predicate, 5)`: if the callback is never claimed — a thread-launch
failure or a `run()` that returns first — the rule reports `BROKEN` ("the cancelled
callback never entered") instead of spinning until an external watchdog kills the suite.
The callback's own `while (!release)` spin is not itself timed; it terminates because the
caller stores `release` unconditionally after the handshake and before `join()`, on the
timeout path as well. A negative control (`run()` returning before any worker exists)
reaches `BROKEN` in about five seconds with exit code 1, not at the CTest timeout.

## 3. Result against this implementation

| Rule | Mechanism | Result |
| --- | --- | --- |
| S1 | reject missing producer / cycle / duplicate / no callback at construction | HOLDS |
| S2 | completion requires a declared output that exists; false or throwing predicate is terminal and releases no successor | HOLDS (negative, throwing, positive, transitive-tail and owned-storage controls) |
| S3 | cancel drains, discards post-cancel completion, never reports success | HOLDS |
| S4 | failed producer releases no consumer | HOLDS |
| S5 | run once; success requires every node | HOLDS |
| S6 | first error sticky, both orders, publication error included, later failure witnessed | HOLDS |

19 required assertions hold, 0 broken, at 1, 2, 4 and 16 workers.

**Mutation controls.** A gate nobody has tried to break is a gate nobody has tested.
Each mutation below was applied to a scratch copy, built, and measured:

| Mutation | Expected and measured |
| --- | --- |
| Delete the predicate evaluation (`if (!exists) result = OutputNotPublished;`) | four assertions `BROKEN`, exit 1 |
| Neuter the S6 two-party handshake (so the later task can be cancelled before it is claimed) | 28–29 of 30 S6 rule instances `BROKEN` across 10 runs at width 2, 4 and 16 — the witness exposes the vacuity the previous revision reported as `HOLDS` |

The second mutation is the important one: it is the mutation that the *previous*
revision of this test would have survived silently.

## 4. What this contract deliberately does not claim

- **No memory-ordering proof.** The callback-to-consumer edge relies on the mutex
  acquire/release pairing the implementation already uses. No C11/C++ atomic order is
  verified, and the ABA question raised against pointer-identity tokens in #197 applies
  here too: a token can be reused.
- **No exhaustive interleaving coverage.** Six rules on constructed scenarios at
  1/2/4/16 workers, not every reachable schedule. ThreadSanitizer was run on the
  contract test as a supporting measurement, which is evidence of absence of reported
  races on these scenarios, not a proof.
- **No statement about the frozen decoder.** `upstream/` is byte-identical; the
  reference decoder keeps its open `#120` status.
- **No claim that a green run means safe.** It means these six rules held on these
  scenarios. A seventh rule that nobody wrote down would still be missing.

## 5. Reproducing

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DHYPERCINE_BUILD_TESTS=ON \
      -DHYPERCINE_BUILD_DECODE_TASK_GRAPH_ONLY=ON -DHYPERCINE_BUILD_UPSTREAM=OFF
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Task-graph only mode builds no SDK target and no GPU backend, so this needs neither.
ThreadSanitizer on Linux:

```
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHYPERCINE_BUILD_TESTS=ON \
      -DHYPERCINE_BUILD_DECODE_TASK_GRAPH_ONLY=ON -DHYPERCINE_BUILD_UPSTREAM=OFF \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan -j && ctest --test-dir build-tsan -R scheduler_contract --output-on-failure
```

## 6. Follow-up

S2's declaration-level check is the right size for a wrapper whose only consumer today
is a test. The next step, when the first production consumer appears, is to make the
predicate a **buffer contract** (output view plus expected extent) instead of a bool,
so "wrote something" becomes "wrote the declared extent". That is a change to the API
and belongs with the consumer that needs it, not ahead of it.
