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
// S3 -- cancellation drains and never reports success.
// From RCA-195: WaitForTransformThread zeroed the queue without asking what was
// unfinished, so a dropped transform became an ordinary success.
// ---------------------------------------------------------------------------
void rule_s3_cancel_drains_and_fails() {
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

    std::thread runner([&] { graph.run(2); });
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
// ---------------------------------------------------------------------------
void rule_s6_first_error_precedence() {
    const auto order_holds = [](bool allocation_first) {
        std::atomic<bool> release_second{false};
        DecodeTaskGraph graph(kGeneration, GraphProfile::Synthetic, {
            {key(1), {},
             [&](const TaskContext&) {
                 if (allocation_first) throw std::bad_alloc();
                 while (!release_second.load()) std::this_thread::yield();
                 throw std::runtime_error("second");
             },
             {[] { return true; }}},
            {key(2), {},
             [&](const TaskContext&) {
                 if (!allocation_first) throw std::runtime_error("first");
                 while (!release_second.load()) std::this_thread::yield();
                 throw std::bad_alloc();
             },
             {[] { return true; }}}});
        std::thread runner([&] { graph.run(2); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (graph.error() == GraphError::None && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        const GraphError first = graph.error();
        release_second = true;
        runner.join();
        return first != GraphError::None && graph.error() == first;
    };

    required("S6-first-error-allocation-first", order_holds(true),
             "the first recorded error survives a later allocation failure");
    required("S6-first-error-callback-first", order_holds(false),
             "the first recorded error survives a later callback failure");
}

// A publication failure is terminal, so it must not be masked by, and must not
// mask, a later unrelated failure either.
void rule_s6_publication_error_is_sticky() {
    std::atomic<bool> release_second{false};
    bool produced = false;
    DecodeTaskGraph graph(kGeneration, GraphProfile::Synthetic, {
        {key(1), {}, [](const TaskContext&) {}, {[&produced] { return produced; }}},
        {key(2), {},
         [&](const TaskContext&) {
             while (!release_second.load()) std::this_thread::yield();
             throw std::runtime_error("later");
         },
         {[] { return true; }}}});
    std::thread runner([&] { graph.run(2); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (graph.error() == GraphError::None && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const GraphError first = graph.error();
    release_second = true;
    runner.join();
    required("S6-publication-error-is-sticky",
             first == GraphError::OutputNotPublished &&
                 graph.error() == GraphError::OutputNotPublished,
             "an OutputNotPublished failure keeps precedence over a later callback failure");
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
    rule_s3_cancel_drains_and_fails();
    rule_s4_failure_does_not_publish(workers);
    rule_s5_must_complete(workers);
    rule_s6_first_error_precedence();
    rule_s6_publication_error_is_sticky();

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
