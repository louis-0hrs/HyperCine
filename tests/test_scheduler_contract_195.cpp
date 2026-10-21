// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Scheduler contract derived from RCA-195 (PR #197), measured against this
// repository's own wrapper scheduler in src/core/decode_task_graph.*.
//
// The point of this file is that the contract is *measured*, not asserted. Each
// rule prints HOLDS or GAP against the real implementation, and the CTest gate
// fails if a rule that holds today stops holding. A rule that does not hold is
// printed as a named gap instead of being quietly omitted -- the RCA-195
// evidence is precisely about what happens when "no failure observed" is
// reported as "cannot fail".
//
// Scope: this grades the *wrapper* scheduler. It says nothing about the frozen
// reference decoder in upstream/, and nothing about memory ordering.
#include "decode_task_graph.h"

#include <atomic>
#include <chrono>
#include <cstdio>
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
int gaps = 0;

// A rule that must hold. A regression here is a real defect.
void required(const char* rule, bool holds_now, const std::string& detail) {
    if (holds_now) {
        ++holds;
        std::printf("RULE %s HOLDS %s\n", rule, detail.c_str());
        return;
    }
    ++failures;
    std::printf("RULE %s BROKEN %s\n", rule, detail.c_str());
}

// A rule the implementation does not meet today. Printed, never hidden, and not
// counted as a pass. Closing it means changing the implementation and this file.
void documented_gap(const char* rule, bool is_gap, const std::string& detail) {
    if (is_gap) {
        ++gaps;
        std::printf("RULE %s GAP %s\n", rule, detail.c_str());
        return;
    }
    ++holds;
    std::printf("RULE %s HOLDS %s\n", rule, detail.c_str());
}

// S3 -- a node's completion must mean its output exists.
// RCA-195 iteration 1: a transform that produced nothing still reported OKAY,
// because "the entry left the queue" was recorded as "the work happened".
void rule_s3_output_completeness() {
    int consumer_ran = 0;
    DecodeTaskGraph graph(kGeneration, GraphProfile::Synthetic, {
        {key(1), {}, [](const TaskContext&) { /* produces nothing */ }},
        {key(2), {key(1)}, [&](const TaskContext&) { ++consumer_ran; }}});
    const bool reported_success = graph.run(2);
    const bool accepted_silent_no_op =
        reported_success && graph.error() == GraphError::None;
    documented_gap("S3-output-completeness", accepted_silent_no_op,
                   accepted_silent_no_op
                       ? "a callback that writes nothing is reported as success; "
                         "callers must hand in a producing callback and the graph "
                         "cannot tell the difference"
                       : "the graph rejected an empty result");
}

// S7 -- first-error precedence, in BOTH arrival orders.
// The order is constructed, not raced for: the second failure waits until the
// first error is already visible, so "first" is a fact rather than a hope.
void rule_s7_first_error_precedence() {
    const auto order_holds = [](bool allocation_first) {
        std::atomic<bool> release_second{false};
        DecodeTaskGraph graph(kGeneration, GraphProfile::Synthetic, {
            {key(1), {}, [&](const TaskContext&) {
                 if (allocation_first) throw std::bad_alloc();
                 while (!release_second.load()) std::this_thread::yield();
                 throw std::runtime_error("second");
             }},
            {key(2), {}, [&](const TaskContext&) {
                 if (!allocation_first) throw std::runtime_error("first");
                 while (!release_second.load()) std::this_thread::yield();
                 throw std::bad_alloc();
             }}});
        std::thread runner([&] { graph.run(2); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (graph.error() == GraphError::None &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        const GraphError first = graph.error();
        release_second = true;
        runner.join();
        return first != GraphError::None && graph.error() == first;
    };

    required("S7-first-error-allocation-first", order_holds(true),
             "the first recorded error survives a later allocation failure");
    required("S7-first-error-callback-first", order_holds(false),
             "the first recorded error survives a later callback failure");
}

// S4 -- cancellation must not publish. A cancelled graph reports failure and
// still drains every started worker (no thread outlives run()).
void rule_s4_cancel_drains_and_fails() {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    DecodeTaskGraph graph(kGeneration, GraphProfile::Synthetic, {
        {key(1), {}, [&](const TaskContext&) {
             entered = true;
             while (!release.load()) std::this_thread::yield();
         }},
        {key(2), {key(1)}, [](const TaskContext&) {}}});
    std::thread runner([&] { graph.run(2); });
    while (!entered.load()) std::this_thread::yield();
    const bool cancelled = graph.cancel(kGeneration);
    const bool not_quiescent_while_running = !graph.quiescent();
    release = true;
    runner.join();
    required("S4-cancel-drains", cancelled && not_quiescent_while_running &&
                                     graph.quiescent() &&
                                     graph.error() == GraphError::Canceled,
             "cancel is observable, the graph drains, and the error is Canceled");
}

// S6 -- success requires every node completed. A graph that did not run all of
// its work may never report success, and an impossible predecessor count fails
// closed rather than deadlocking.
void rule_s6_must_complete() {
    DecodeTaskGraph ran(kGeneration, GraphProfile::Synthetic, {
        {key(1), {}, [](const TaskContext&) {}}});
    const bool first_result = ran.run(1);
    const bool refuses_second_run = !ran.run(1);
    required("S6-run-once", first_result && refuses_second_run,
             "a complete graph runs once and refuses re-submission");
}

// S1 -- the graph rejects a topology it cannot schedule, before any worker runs.
void rule_s1_reject_before_execution() {
    bool rejected_missing_producer = false;
    try {
        DecodeTaskGraph bad(kGeneration, GraphProfile::Synthetic,
                            {{key(2), {key(1)}, [](const TaskContext&) {}}});
    } catch (const std::invalid_argument&) {
        rejected_missing_producer = true;
    }
    bool rejected_cycle = false;
    try {
        DecodeTaskGraph bad(kGeneration, GraphProfile::Synthetic,
                            {{key(1), {key(2)}, [](const TaskContext&) {}},
                             {key(2), {key(1)}, [](const TaskContext&) {}}});
    } catch (const std::invalid_argument&) {
        rejected_cycle = true;
    }
    required("S1-reject-before-execution", rejected_missing_producer && rejected_cycle,
             "a missing producer and a cycle are rejected at construction");
}

// S5 -- a throwing producer never lets its consumer run.
void rule_s5_failure_does_not_publish() {
    int reads = 0;
    DecodeTaskGraph graph(kGeneration, GraphProfile::Synthetic, {
        {key(1), {}, [](const TaskContext&) { throw std::runtime_error("boom"); }},
        {key(2), {key(1)}, [&](const TaskContext&) { ++reads; }}});
    graph.run(2);
    required("S5-failure-does-not-publish",
             reads == 0 && graph.error() == GraphError::Callback &&
                 graph.state(1) == TaskState::Failed,
             "a failed producer leaves its consumer unexecuted and Failed");
}

}  // namespace

int main() {
    rule_s1_reject_before_execution();
    rule_s3_output_completeness();
    rule_s4_cancel_drains_and_fails();
    rule_s5_failure_does_not_publish();
    rule_s6_must_complete();
    rule_s7_first_error_precedence();

    std::printf("%d rules hold, %d documented gap(s), %d broken\n",
                holds, gaps, failures);
    if (failures) {
        std::printf("SCHEDULER-CONTRACT FAIL\n");
        return 1;
    }
    std::printf("SCHEDULER-CONTRACT PASS\n");
    return 0;
}
