// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Scheduler contract derived from RCA-195 (PR #197), measured against this
// repository's own wrapper scheduler in src/core/decode_task_graph.*.
//
// This file is a GATE, not a report. Every rule below is REQUIRED: a rule that
// does not hold exits non-zero. There is deliberately no "GAP that still
// passes" path -- RCA-195 exists because "no failure observed" was printed as
// "cannot fail", and a gate that prints PASS over a known shortfall would be
// that same defect with better formatting.
//
// Scope: the *wrapper* scheduler. Nothing here grades the frozen reference
// decoder in upstream/, and nothing here proves memory ordering.
#include "decode_task_graph.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace hypercine::detail;

const Generation kGeneration{1, 1, 1};

TaskKey key(std::uint32_t band, std::uint32_t channel = 0) {
    return {channel, 0, 0, 0, band};
}

int failures = 0;
int holds = 0;

// A required rule. There is no softer variant in this file on purpose.
void required(const char* rule, bool holds_now, const std::string& detail) {
    if (holds_now) {
        ++holds;
        std::printf("RULE %s HOLDS %s\n", rule, detail.c_str());
        return;
    }
    ++failures;
    std::printf("RULE %s BROKEN %s\n", rule, detail.c_str());
}

void note(const std::string& text) { std::printf("INFO %s\n", text.c_str()); }

// S2's escape hatch must itself be counted. A graph that declares no
// publication predicates is exactly the shape RCA-195 punished, so this test
// refuses to build one: every fixture below names its output contract.
void require_fully_contracted(const DecodeTaskGraph& graph, const char* where) {
    if (graph.uncontracted_tasks() != 0) {
        std::printf("RULE S2-contract-inventory BROKEN fixture %s has %zu task(s) "
                    "with no publication predicate\n",
                    where, graph.uncontracted_tasks());
        ++failures;
        return;
    }
    ++holds;
    std::printf("RULE S2-contract-inventory HOLDS fixture %s declares an output "
                "contract for every task\n", where);
}

// ---------------------------------------------------------------------------
// S1 -- validate the whole topology before any worker runs.
// From RCA-195: the reference queue accepted entries whose producer relation
// was never checked, so work could be admitted before its real input existed.
// ---------------------------------------------------------------------------
void rule_s1_reject_before_execution() {
    const auto rejects = [](std::vector<TaskSpec> tasks) {
        try {
            DecodeTaskGraph bad(kGeneration, GraphProfile::Synthetic, std::move(tasks));
        } catch (const std::invalid_argument&) {
            return true;
        } catch (...) {
            return false;
        }
        return false;
    };

    const bool missing_producer = rejects({{key(2), {key(1)}, [](const TaskContext&) {}, {[] { return true; }}}});
    const bool cycle = rejects({{key(1), {key(2)}, [](const TaskContext&) {}, {[] { return true; }}},
                                {key(2), {key(1)}, [](const TaskContext&) {}, {[] { return true; }}}});
    const bool self_cycle = rejects({{key(1), {key(1)}, [](const TaskContext&) {}, {[] { return true; }}}});
    const bool duplicate_key = rejects({{key(1), {}, [](const TaskContext&) {}, {[] { return true; }}},
                                        {key(1), {}, [](const TaskContext&) {}, {[] { return true; }}}});
    const bool duplicate_edge = rejects({{key(1), {}, [](const TaskContext&) {}, {[] { return true; }}},
                                         {key(2), {key(1), key(1)}, [](const TaskContext&) {}, {[] { return true; }}}});
    const bool no_callback = rejects({{key(1), {}, std::function<void(const TaskContext&)>{}, {[] { return true; }}}});

    required("S1-reject-before-execution",
             missing_producer && cycle && self_cycle && duplicate_key && duplicate_edge &&
                 no_callback,
             "missing producer, cycle, self-cycle, duplicate key, duplicate edge and a "
             "callback-less task are all rejected at construction");
}

// ---------------------------------------------------------------------------
// S2 -- a node's completion means its output exists.
// From RCA-195: `done = 1` recorded admission, the queue counter recorded
// departure, and neither recorded that the transform ran and wrote. That is why
// a discarded entry still produced a public OKAY.
//
// Negative control first: a callback that returns normally but produces nothing
// must NOT be reported as success, and its consumer must NOT run.
// ---------------------------------------------------------------------------
void rule_s2_output_completeness(std::size_t workers) {
    {
        int consumer_ran = 0;
        bool wrote = false;
        DecodeTaskGraph graph(kGeneration, GraphProfile::Synthetic, {
            {key(1), {}, [](const TaskContext&) { /* returns, writes nothing */ },
             {[&wrote] { return wrote; }}},
            {key(2), {key(1)}, [&](const TaskContext&) { ++consumer_ran; },
             {[] { return true; }}}});
        require_fully_contracted(graph, "S2-negative-control");
        const bool success = graph.run(workers);
        const GraphError error = graph.error();
        const bool quiescent = graph.quiescent();
        required("S2-silent-no-op-fails-closed",
                 !success && error == GraphError::OutputNotPublished && consumer_ran == 0 &&
                     graph.state(0) == TaskState::Failed &&
                     graph.state(1) == TaskState::Failed && quiescent,
                 "a callback that produces nothing is a terminal OutputNotPublished "
                 "error; the dependent never runs and the graph still drains");
    }
    {
        // A predicate that cannot answer is not evidence of an output.
        int consumer_ran = 0;
        DecodeTaskGraph graph(kGeneration, GraphProfile::Synthetic, {
            {key(1), {}, [](const TaskContext&) {},
             {[]() -> bool { throw std::runtime_error("predicate unavailable"); }}},
            {key(2), {key(1)}, [&](const TaskContext&) { ++consumer_ran; },
             {[] { return true; }}}});
        require_fully_contracted(graph, "S2-throwing-predicate");
        const bool success = graph.run(workers);
        required("S2-throwing-predicate-fails-closed",
                 !success && graph.error() == GraphError::OutputNotPublished &&
                     consumer_ran == 0 && graph.state(0) == TaskState::Failed,
                 "a throwing publication predicate fails closed instead of being "
                 "treated as a passing check");
    }
    {
        // Positive control: publish-before-consume over a real dependency edge.
        bool wrote = false;
        int value = 0;
        int consumed = 0;
        DecodeTaskGraph graph(kGeneration, GraphProfile::Synthetic, {
            {key(1), {}, [&](const TaskContext&) { value = 147; wrote = true; },
             {[&wrote] { return wrote; }}},
            {key(2), {key(1)},
             [&](const TaskContext&) {
                 if (!wrote || value != 147) throw std::runtime_error("read before publish");
                 ++consumed;
             },
             {[&consumed] { return consumed == 1; }}}});
        require_fully_contracted(graph, "S2-positive-control");
        const bool success = graph.run(workers);
        required("S2-publish-before-consume",
                 success && graph.error() == GraphError::None && consumed == 1 &&
                     wrote && graph.state(1) == TaskState::Completed,
                 "a declared output that exists lets the consumer run; both nodes complete");
    }
    {
        // Deeper topology: a false predicate in the middle must fail the whole
        // transitive tail, not just its own node.
        int tail_ran = 0;
        bool wrote = false;
        DecodeTaskGraph graph(kGeneration, GraphProfile::Synthetic, {
            {key(1), {}, [&](const TaskContext&) { wrote = true; }, {[&wrote] { return wrote; }}},
            {key(2), {key(1)}, [](const TaskContext&) {}, {[] { return false; }}},
            {key(3), {key(2)}, [&](const TaskContext&) { ++tail_ran; }, {[] { return true; }}}});
        require_fully_contracted(graph, "S2-transitive-tail");
        const bool success = graph.run(workers);
        required("S2-transitive-tail-fails",
                 !success && graph.error() == GraphError::OutputNotPublished && tail_ran == 0 &&
                     graph.state(1) == TaskState::Failed && graph.state(2) == TaskState::Failed,
                 "a false predicate fails its own node and its transitive dependents");
    }
    note("S2 limit: the predicate is a declaration. A callback that sets its own flag "
         "without writing the buffer is still reported as published; the graph can "
         "check what the task declares, not what the memory contains.");
}

// ---------------------------------------------------------------------------
// S2-lifetime -- the publication predicate must read storage that outlives the
// callback frame.
//
// The earlier text in decode_task_graph.h said the callback "frame and any
// storage it produced" is still live when published() runs. That is false:
// execute() has returned, so a callback local is already dead. Measured with a
// probe that captured the address of one such local: a plain Release build read
// the dead slot and reported success, and AddressSanitizer classified the same
// read as `stack-use-after-return`. Guidance that tells consumers to capture
// callback locals is therefore a use-after-return generator, not a contract.
//
// The supported pattern is what this rule exercises: the payload is owned
// independently of the frame (here a heap object the caller owns) and stays
// alive through predicate evaluation and the downstream consumer.
// ---------------------------------------------------------------------------
void rule_s2_predicate_reads_owned_storage(std::size_t workers) {
    // Caller-owned payload: outlives both the callback frame and the graph.
    struct Payload {
        int written = 0;
        int consumed = 0;
    };
    auto payload = std::make_shared<Payload>();
    bool callback_ran = false;

    DecodeTaskGraph graph(kGeneration, GraphProfile::Synthetic, {
        {key(1), {},
         // The lambda's own frame dies here; only `payload` survives it.
         [payload, &callback_ran](const TaskContext&) {
             payload->written = 147;
             callback_ran = true;
         },
         // Reads independently owned storage, not a callback local.
         [payload] { return payload && payload->written == 147; }},
        {key(2), {key(1)},
         [payload, &callback_ran](const TaskContext&) {
             // The producer's storage is still valid for downstream consumers.
             if (!callback_ran || payload->written != 147)
                 throw std::runtime_error("consumer read a dead or unwritten payload");
             ++payload->consumed;
         },
         [payload] { return payload->consumed == 1; }}});

    require_fully_contracted(graph, "S2-owned-storage");
    // Requested width, including workers left idle by a two-node chain.
    // Width 1 is the serial producer-then-consumer order; it is still this
    // rule, not a skipped copy of a hardcoded run(2).
    const bool success = graph.run(workers);
    required("S2-predicate-reads-owned-storage",
             success && graph.error() == GraphError::None && payload->written == 147 &&
                 payload->consumed == 1 &&
                 graph.state(0) == TaskState::Completed &&
                 graph.state(1) == TaskState::Completed,
             "a predicate that reads caller-owned storage outliving the callback frame "
             "publishes correctly and its consumer sees the written value");
}

// ---------------------------------------------------------------------------
// S3 -- cancellation drains and never reports success.
// From RCA-195: WaitForTransformThread zeroed the queue without asking what was
// unfinished, so a dropped transform became an ordinary success.
// ---------------------------------------------------------------------------
void rule_s3_cancel_drains_and_fails(std::size_t workers) {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    bool wrote = false;
    DecodeTaskGraph graph(kGeneration, GraphProfile::Synthetic, {
        {key(1), {},
         [&](const TaskContext&) {
             entered = true;
             while (!release.load()) std::this_thread::yield();
             wrote = true;
         },
         {[&wrote] { return wrote; }}},
        {key(2), {key(1)}, [](const TaskContext&) {}, {[] { return true; }}}});

    // Requested width. Cancel is issued from this thread while one callback
    // is inside the graph, so the rule does not borrow a second worker to
    // look concurrent -- and it still has to hold when the pool is wider than
    // the ready set. A CTest argument of 4 or 16 must not replay run(2).
    std::thread runner([&] { graph.run(workers); });
    while (!entered.load()) std::this_thread::yield();
    const bool cancelled = graph.cancel(kGeneration);
    const bool not_quiescent_while_running = !graph.quiescent();
    release = true;
    runner.join();

    required("S3-cancel-drains-and-fails",
             cancelled && not_quiescent_while_running && graph.quiescent() &&
                 graph.error() == GraphError::Canceled &&
                 graph.state(0) == TaskState::Canceled &&
                 graph.state(1) == TaskState::Canceled,
             "cancel is observable, the graph drains before releasing buffers, and a "
             "completion that arrives after cancellation is discarded");
}

// ---------------------------------------------------------------------------
// S4 -- a failed producer never releases its consumers.
// From RCA-195: an inverse handed a wavelet missing a band returned without
// writing and without setting an error, so the caller reported OKAY.
// ---------------------------------------------------------------------------
void rule_s4_failure_does_not_publish(std::size_t workers) {
    int reads = 0;
    DecodeTaskGraph graph(kGeneration, GraphProfile::Synthetic, {
        {key(1), {}, [](const TaskContext&) { throw std::runtime_error("boom"); },
         {[] { return true; }}},
        {key(2), {key(1)}, [&](const TaskContext&) { ++reads; }, {[&reads] { return reads > 0; }}},
        {key(3), {key(2)}, [&](const TaskContext&) { ++reads; }, {[&reads] { return reads > 0; }}}});
    require_fully_contracted(graph, "S4-producer-throws");
    const bool success = graph.run(workers);
    required("S4-failure-does-not-publish",
             !success && reads == 0 && graph.error() == GraphError::Callback &&
                 graph.state(0) == TaskState::Failed && graph.state(1) == TaskState::Failed &&
                 graph.state(2) == TaskState::Failed,
             "a failed producer leaves its whole dependent tail unexecuted and Failed");
}

// ---------------------------------------------------------------------------
// S5 -- exactly one run, and success means every node completed.
// From RCA-195: a frame was reported decoded while entries had been discarded;
// and I4_BOUNDED only meant something once the search exhausted its frontier.
// ---------------------------------------------------------------------------
void rule_s5_must_complete(std::size_t workers) {
    int ran = 0;
    DecodeTaskGraph graph(kGeneration, GraphProfile::Synthetic,
                          {{key(1), {}, [&](const TaskContext&) { ++ran; },
                            {[&ran] { return ran == 1; }}}});
    require_fully_contracted(graph, "S5-run-once");
    const bool first = graph.run(workers);
    const bool refuses_second = !graph.run(workers);
    required("S5-run-once-and-all-completed",
             first && refuses_second && ran == 1 && graph.error() == GraphError::None &&
                 graph.state(0) == TaskState::Completed,
             "a complete graph runs once, refuses re-submission, and reports success only "
             "with every node Completed");
}

// ---------------------------------------------------------------------------
// S6 -- the first recorded error is the one reported, in either arrival order.
// The order is constructed, not raced for: the second failure waits until the
// first error is visible, so "first" is a fact rather than a winner.
//
// Constructed order alone was not enough, and that was measured rather than
// argued: `fail_locked()` stops dispatch, so the later task can be cancelled
// before any worker claims it. The rule then saw `first != None && final ==
// first` and reported HOLDS although the second failure never happened. An
// instrumented probe of the previous revision confirmed it -- in the
// allocation-first arm the later callback was claimed 0 times in 12/12 runs at
// every worker width while the rule still passed.
//
// The arms therefore differ by worker width, because only one claim is honest at
// each width:
//
//   workers >= 2  both root tasks can be in flight. A two-party start handshake
//                 (neither callback may fail before both are claimed) plus a
//                 flag written immediately before the later throw makes
//                 "survived a later failure" a witnessed fact. Missing witness
//                 is BROKEN, never HOLDS.
//   workers == 1  the graph is strictly serial: the later task can never be
//                 claimed once the first fails. Claiming to have survived a
//                 later failure there would be a fabricated event, so the rule
//                 asserts what is actually reachable instead -- the later task
//                 was never executed and is Canceled.
//
// run() receives the requested worker count on every arm. An earlier revision
// substituted run(2) whenever workers >= 2 (and run(1) otherwise), so the
// CTest entries for 4 and 16 repeated the 2-worker measurement and were not
// evidence about those widths. The assertion split above is unchanged: width
// 1 still must not invent the two-party witness.
//
// Widths 4 and 16 still execute both root callbacks, and that is a property
// of this graph rather than a longer timeout. Both S6 roots are
// dependency-free, so both are Ready before any worker is created. A worker
// claims the next ready index under the graph mutex, and the concurrent
// handshake does not fail until both callbacks have entered, so the first
// error cannot cancel the other root before that claim. Workers beyond the
// two roots block on the ready queue; they do not skip or replace the second
// claim. A missing witness stays BROKEN. The wait is not widened to launder
// a schedule the queue does not actually provide.
//
// Every wait is bounded, so a broken handshake fails the rule instead of hanging.
// ---------------------------------------------------------------------------
namespace s6 {

// Bounded spin, never an unbounded wait: a handshake that cannot complete has to
// fail the rule rather than wedge the gate.
bool wait_for(const std::function<bool()>& predicate, int seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

// Shared two-party start gate. Both root callbacks call enter(); the gate opens
// when the second one arrives.
struct StartGate {
    std::atomic<int> started{0};
    std::atomic<bool> both_started{false};
    std::atomic<bool> timed_out{false};

    void enter() {
        if (started.fetch_add(1) + 1 >= 2) both_started = true;
    }
    // False means the other party never arrived in time: a broken witness, not a
    // passing rule.
    bool await_both() {
        if (!wait_for([this] { return both_started.load(); }, 5)) {
            timed_out = true;
            return false;
        }
        return true;
    }
};

}  // namespace s6

void rule_s6_first_error_precedence(std::size_t workers) {
    const bool concurrent = workers >= 2;

    const auto order_holds = [concurrent, workers](bool allocation_first, std::string& why) {
        s6::StartGate gate;
        std::atomic<bool> second_threw{false};
        std::atomic<bool> later_ran{false};
        const GraphError first_error =
            allocation_first ? GraphError::Allocation : GraphError::Callback;
        // Published after construction, so the callbacks can observe the recorded
        // error without referring to the graph object during its own initializer
        // (rejected by MSVC, and it would be a half-constructed object anyway).
        DecodeTaskGraph* graph_ptr = nullptr;

        DecodeTaskGraph graph(kGeneration, GraphProfile::Synthetic, {
            // The first failure. In the concurrent arm it may not fail before the
            // other task has been claimed.
            {key(1), {},
             [&](const TaskContext&) {
                 gate.enter();
                 if (concurrent) gate.await_both();
                 if (allocation_first) throw std::bad_alloc();
                 throw std::runtime_error("first");
             },
             {[] { return true; }}},
            // The later failure, only after the first error is visible.
            {key(2), {},
             [&](const TaskContext&) {
                 later_ran = true;
                 gate.enter();
                 if (concurrent &&
                     (!gate.await_both() ||
                      !s6::wait_for([&] { return graph_ptr->error() != GraphError::None; }, 5))) {
                     gate.timed_out = true;
                     return;  // produces output; the witness check below fails the rule
                 }
                 second_threw = true;
                 if (allocation_first) throw std::runtime_error("second");
                 throw std::bad_alloc();
             },
             {[] { return true; }}}});
        graph_ptr = &graph;

        std::thread runner([&] { graph.run(workers); });
        GraphError first = GraphError::None;
        if (concurrent) {
            // The later callback waits on this error becoming visible, so it is
            // safe to read before joining: the first error is already recorded.
            s6::wait_for([&] { return graph.error() != GraphError::None; }, 5);
            first = graph.error();
            runner.join();
        } else {
            // Serial run: the error only exists once the worker has reached it,
            // so join first and read the settled value.
            runner.join();
            first = graph.error();
        }

        bool holds = first == first_error && graph.error() == first;
        if (concurrent) {
            const bool witnessed = !gate.timed_out && second_threw.load();
            holds = holds && witnessed && graph.state(0) == TaskState::Failed &&
                    graph.state(1) == TaskState::Failed;
            why = witnessed
                      ? std::string("two-party witness: both callbacks were claimed and the "
                                    "later one threw while the first error stayed reported")
                      : std::string("later failure was NOT exercised (handshake incomplete or "
                                    "the later callback never threw)");
        } else {
            holds = holds && !later_ran.load() && graph.state(0) == TaskState::Failed &&
                    graph.state(1) == TaskState::Canceled;
            why = std::string("single worker: strictly serial, the later task was never claimed "
                              "and is Canceled, so no later failure is claimed");
        }
        return holds;
    };

    {
        std::string why;
        const bool holds = order_holds(true, why);
        required("S6-first-error-allocation-first", holds,
                 "the first recorded error (allocation) is the reported one -- " + why);
    }
    {
        std::string why;
        const bool holds = order_holds(false, why);
        required("S6-first-error-callback-first", holds,
                 "the first recorded error (callback) is the reported one -- " + why);
    }
}

// A publication failure is terminal, so it must not be masked by, and must not
// mask, a later unrelated failure either.
// Same vacuity risk as above, so the same witness is required: at width >= 2 the
// later callback must be claimed and must actually throw; at width 1 the rule
// asserts the reachable serial outcome instead.
void rule_s6_publication_error_is_sticky(std::size_t workers) {
    const bool concurrent = workers >= 2;
    s6::StartGate gate;
    std::atomic<bool> second_threw{false};
    std::atomic<bool> later_ran{false};
    bool produced = false;
    DecodeTaskGraph* graph_ptr = nullptr;

    DecodeTaskGraph graph(kGeneration, GraphProfile::Synthetic, {
        {key(1), {},
         [&](const TaskContext&) {
             gate.enter();
             if (concurrent) gate.await_both();
             // Returns normally and publishes nothing. The publication predicate
             // below is what has to catch it.
         },
         {[&produced] { return produced; }}},
        {key(2), {},
         [&](const TaskContext&) {
             later_ran = true;
             gate.enter();
             if (concurrent &&
                 (!gate.await_both() ||
                  !s6::wait_for([&] { return graph_ptr->error() != GraphError::None; }, 5))) {
                 gate.timed_out = true;
                 return;
             }
             second_threw = true;
             throw std::runtime_error("later");
         },
         {[] { return true; }}}});
    graph_ptr = &graph;

    std::thread runner([&] { graph.run(workers); });
    GraphError first = GraphError::None;
    if (concurrent) {
        s6::wait_for([&] { return graph.error() != GraphError::None; }, 5);
        first = graph.error();
        runner.join();
    } else {
        runner.join();
        first = graph.error();
    }

    const bool precedence = first == GraphError::OutputNotPublished &&
                            graph.error() == GraphError::OutputNotPublished &&
                            graph.state(0) == TaskState::Failed;
    if (concurrent) {
        const bool witnessed = !gate.timed_out && second_threw.load();
        required("S6-publication-error-is-sticky",
                 precedence && witnessed && graph.state(1) == TaskState::Failed,
                 witnessed
                     ? std::string("an OutputNotPublished failure keeps precedence over a later "
                                   "callback failure that actually executed")
                     : std::string("an OutputNotPublished failure was reported, but the later "
                                   "callback was NOT exercised -- witness missing"));
    } else {
        required("S6-publication-error-is-sticky",
                 precedence && !later_ran.load() && graph.state(1) == TaskState::Canceled,
                 "single worker: OutputNotPublished is the reported error and the dependent "
                 "work was never claimed");
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::printf("SCHEDULER-CONTRACT FAIL usage: %s <workers>\n", argc ? argv[0] : "test");
        return 2;
    }
    const auto workers = std::stoul(argv[1]);
    if (workers != 1 && workers != 2 && workers != 4 && workers != 16) {
        std::printf("SCHEDULER-CONTRACT FAIL unsupported worker count %zu\n", workers);
        return 2;
    }

    std::printf("scheduler contract, workers=%zu, synthetic wrapper graph only\n", workers);
    rule_s1_reject_before_execution();
    rule_s2_output_completeness(workers);
    rule_s2_predicate_reads_owned_storage(workers);
    rule_s3_cancel_drains_and_fails(workers);
    rule_s4_failure_does_not_publish(workers);
    rule_s5_must_complete(workers);
    rule_s6_first_error_precedence(workers);
    rule_s6_publication_error_is_sticky(workers);

    std::printf("%d rules hold, %d broken\n", holds, failures);
    // Fail-closed gate: no rule is optional here, so an unmet rule is a failure
    // and PASS is unreachable with a known shortfall.
    if (failures) {
        std::printf("SCHEDULER-CONTRACT FAIL\n");
        return 1;
    }
    std::printf("SCHEDULER-CONTRACT PASS\n");
    return 0;
}
