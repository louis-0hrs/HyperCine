// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// S2 output completion for the synthetic scheduler (issues #144 / #195).
// A returning callback is not a result. Completion requires a BufferVersion of
// this generation whose bytes are retained by the graph. No SDK, no upstream/
// link, no GoPro/CFHD decode: this gate does not integrate a real decoder.
//
// Every rule is required. A BROKEN line exits non-zero. There is no partial pass.
#include "decode_task_graph.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace hypercine::detail {
struct DecodeTaskGraphTestAccess {
    static void retarget(DecodeTaskGraph& g, std::size_t node, Generation generation) {
        std::lock_guard<std::mutex> lock(g.mutex_);
        g.nodes_.at(node).published_generation = generation;
    }
    static void strip_payload(DecodeTaskGraph& g, std::size_t node) {
        std::lock_guard<std::mutex> lock(g.mutex_);
        auto& target = g.nodes_.at(node);
        target.payload.store(nullptr, std::memory_order_relaxed);
        target.payload_bytes.store(0, std::memory_order_relaxed);
    }
    static void clear_version(DecodeTaskGraph& g, std::size_t node) {
        std::lock_guard<std::mutex> lock(g.mutex_);
        g.nodes_.at(node).version.store(0, std::memory_order_relaxed);
    }
};
} // namespace hypercine::detail

namespace {

using namespace hypercine::detail;

const Generation kGeneration{7, 3, 11};
constexpr std::size_t kPayload = 64;

TaskKey key(std::uint32_t band) { return {0, 0, 0, 0, band}; }

int failures = 0;
int holds = 0;

void required(const char* rule, bool holds_now, const std::string& detail) {
    if (holds_now) {
        ++holds;
        std::printf("RULE %s HOLDS %s\n", rule, detail.c_str());
        return;
    }
    ++failures;
    std::printf("RULE %s BROKEN %s\n", rule, detail.c_str());
}

bool wait_until(const std::function<bool()>& predicate, int seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

unsigned char pattern(std::size_t i) {
    return static_cast<unsigned char>(0xA5 ^ (i & 0x0F));
}

void publish_token(const TaskContext& context) {
    static constexpr unsigned char token = 1;
    if (!context.publish(&token, 1)) throw std::runtime_error("token publish failed");
}

bool not_ready(TaskState state) {
    return state != TaskState::Ready && state != TaskState::Running &&
           state != TaskState::Completed;
}

// ---------------------------------------------------------------------------
// No-op producer. The predicate returns true and the callback publishes
// nothing. That used to be enough to Complete. It must not be: the dependent
// must not run and must not become Ready.
// Guard: published_output() rejects version == 0, and nothing after that line
// rejects a zero version.
// ---------------------------------------------------------------------------
void rule_noop_producer(std::size_t workers) {
    std::atomic<int> consumer_runs{0};
    DecodeTaskGraph graph(kGeneration, GraphProfile::Synthetic, {
        {key(1), {}, [](const TaskContext&) { /* returns, publishes nothing */ },
         {[] { return true; }}},
        {key(2), {key(1)}, [&](const TaskContext&) { ++consumer_runs; },
         {[] { return true; }}}});
    const std::size_t ready_before = graph.ready_transitions();
    const bool ran = graph.run(workers);
    const bool consumer_held = not_ready(graph.state(1));
    char detail[384];
    std::snprintf(detail, sizeof(detail),
                  "run=%d error=%d (expected %d OutputNotPublished) producer=%d "
                  "consumer=%d consumer_runs=%d ready %zu->%zu",
                  static_cast<int>(ran), static_cast<int>(graph.error()),
                  static_cast<int>(GraphError::OutputNotPublished),
                  static_cast<int>(graph.state(0)), static_cast<int>(graph.state(1)),
                  consumer_runs.load(), ready_before, graph.ready_transitions());
    required("S2-noop-producer",
             !ran && graph.error() == GraphError::OutputNotPublished && graph.quiescent() &&
                 graph.state(0) == TaskState::Failed && graph.state(1) == TaskState::Failed &&
                 consumer_runs.load() == 0 && consumer_held &&
                 graph.ready_transitions() == ready_before,
             detail);
}

// ---------------------------------------------------------------------------
// Premature release. The producer publishes, poisons and frees its buffer,
// then returns. Both dependents must still read the original bytes from a
// pointer that is not the freed address. One retained copy serves every
// consumer; freeing the caller's buffer must not free that copy.
// Guard: publish() copies into owned_bytes and points the version at that copy.
// ---------------------------------------------------------------------------
void rule_premature_release(std::size_t workers) {
    const void* owner = nullptr;
    std::atomic<int> reads{0};
    std::atomic<int> bad{0};
    std::atomic<int> dangling{0};
    std::atomic<int> generation_mismatch{0};
    DecodeTaskGraph graph({7, 3, 12}, GraphProfile::Synthetic, {
        {key(1), {},
         [&](const TaskContext& context) {
             auto* raw = new unsigned char[kPayload];
             for (std::size_t i = 0; i < kPayload; ++i) raw[i] = pattern(i);
             owner = raw;
             if (!context.publish(raw, kPayload)) {
                 delete[] raw;
                 throw std::runtime_error("producer publish failed");
             }
             for (std::size_t i = 0; i < kPayload; ++i) raw[i] = 0x5A;
             delete[] raw;
         },
         {[] { return true; }}},
        {key(2), {key(1)},
         [&](const TaskContext& context) {
             context.wait_for_input(0, std::chrono::milliseconds(2000));
             const BufferVersion view = context.input(0);
             if (view.data == owner) {
                 ++dangling;
                 return;
             }
             if (!(view.generation == Generation{7, 3, 12}) || !view.valid() || !view.data ||
                 view.bytes != kPayload) {
                 ++bad;
                 return;
             }
             const auto* bytes = static_cast<const unsigned char*>(view.data);
             for (std::size_t i = 0; i < kPayload; ++i)
                 if (bytes[i] != pattern(i)) {
                     ++bad;
                     return;
                 }
             ++reads;
             publish_token(context);
         },
         {[] { return true; }}},
        {key(3), {key(1)},
         [&](const TaskContext& context) {
             context.wait_for_input(0, std::chrono::milliseconds(2000));
             const BufferVersion view = context.input(0);
             if (view.data == owner) {
                 ++dangling;
                 return;
             }
             if (!(view.generation == Generation{7, 3, 12}) || !view.valid() || !view.data ||
                 view.bytes != kPayload) {
                 ++bad;
                 return;
             }
             const auto* bytes = static_cast<const unsigned char*>(view.data);
             for (std::size_t i = 0; i < kPayload; ++i)
                 if (bytes[i] != pattern(i)) {
                     ++bad;
                     return;
                 }
             ++reads;
             publish_token(context);
         },
         {[] { return true; }}}});
    const bool ran = graph.run(workers);
    char detail[384];
    std::snprintf(detail, sizeof(detail),
                  "run=%d error=%d reads=%d (expected 2) bad=%d dangling=%d "
                  "generation_mismatch=%d producer=%d c1=%d c2=%d",
                  static_cast<int>(ran), static_cast<int>(graph.error()), reads.load(),
                  bad.load(), dangling.load(), generation_mismatch.load(),
                  static_cast<int>(graph.state(0)), static_cast<int>(graph.state(1)),
                  static_cast<int>(graph.state(2)));
    required("S2-premature-release",
             ran && graph.error() == GraphError::None && reads.load() == 2 && bad.load() == 0 &&
                 dangling.load() == 0 && graph.state(0) == TaskState::Completed &&
                 graph.state(1) == TaskState::Completed && graph.state(2) == TaskState::Completed,
             detail);
}

// ---------------------------------------------------------------------------
// Missing publication. publish() did run, but the record is not a readable
// buffer of this generation: either the generation was replaced, or the
// address/length was stripped while the version word stayed non-zero.
// Guard: published_output() rejects that record. The version==0 check does
// not, because the version word is already 1.
// ---------------------------------------------------------------------------
void rule_missing_publication(std::size_t workers) {
    const auto arm = [&](bool foreign_generation, Generation generation, int* runs, int* error,
                         int* producer, int* consumer, int* ran_out) {
        std::atomic<int> consumer_runs{0};
        DecodeTaskGraph* graph_ptr = nullptr;
        unsigned char local[4] = {1, 2, 3, 4};
        DecodeTaskGraph graph(generation, GraphProfile::Synthetic, {
            {key(1), {},
             [&](const TaskContext& context) {
                 if (!context.publish(local, sizeof(local)))
                     throw std::runtime_error("publish failed");
                 if (foreign_generation)
                     DecodeTaskGraphTestAccess::retarget(*graph_ptr, 0, Generation{9, 9, 9});
                 else
                     DecodeTaskGraphTestAccess::strip_payload(*graph_ptr, 0);
             },
             {[] { return true; }}},
            {key(2), {key(1)}, [&](const TaskContext&) { ++consumer_runs; },
             {[] { return true; }}}});
        graph_ptr = &graph;
        const bool ran = graph.run(workers);
        *runs = consumer_runs.load();
        *error = static_cast<int>(graph.error());
        *producer = static_cast<int>(graph.state(0));
        *consumer = static_cast<int>(graph.state(1));
        *ran_out = static_cast<int>(ran);
        return !ran && graph.error() == GraphError::OutputNotPublished && graph.quiescent() &&
               graph.state(0) == TaskState::Failed && consumer_runs.load() == 0 &&
               not_ready(graph.state(1));
    };
    int gen_runs = -1, gen_error = -1, gen_producer = -1, gen_consumer = -1, gen_ran = -1;
    int byte_runs = -1, byte_error = -1, byte_producer = -1, byte_consumer = -1, byte_ran = -1;
    const bool foreign = arm(true, {7, 3, 13}, &gen_runs, &gen_error, &gen_producer,
                             &gen_consumer, &gen_ran);
    const bool stripped = arm(false, {7, 3, 14}, &byte_runs, &byte_error, &byte_producer,
                              &byte_consumer, &byte_ran);
    char detail[512];
    std::snprintf(detail, sizeof(detail),
                  "foreign run=%d error=%d producer=%d consumer=%d runs=%d; "
                  "stripped run=%d error=%d producer=%d consumer=%d runs=%d "
                  "(expected error %d, producer Failed, consumer not run)",
                  gen_ran, gen_error, gen_producer, gen_consumer, gen_runs, byte_ran, byte_error,
                  byte_producer, byte_consumer, byte_runs,
                  static_cast<int>(GraphError::OutputNotPublished));
    required("S2-missing-publication", foreign && stripped, detail);
}

// ---------------------------------------------------------------------------
// Removed dependency. C needs both P1 and P2. P1 completing must not drop C's
// remaining dependency on P2. While P2 is still inside its callback, C stays
// Pending and must not have started.
// Guard: complete() readies a successor only when --remaining hits 0.
// ---------------------------------------------------------------------------
void rule_removed_dependency(std::size_t workers) {
    std::atomic<bool> p2_entered{false};
    std::atomic<bool> release{false};
    std::atomic<int> consumer_entered{0};
    std::atomic<int> saw_both{0};
    unsigned char first[4] = {9, 8, 7, 6};
    unsigned char second[4] = {4, 3, 2, 1};
    DecodeTaskGraph graph({7, 3, 15}, GraphProfile::Synthetic, {
        {key(1), {},
         [&](const TaskContext& context) {
             if (!context.publish(first, sizeof(first)))
                 throw std::runtime_error("p1 publish failed");
         },
         {[] { return true; }}},
        {key(2), {},
         [&](const TaskContext& context) {
             p2_entered.store(true);
             const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
             while (!release.load() && std::chrono::steady_clock::now() < deadline)
                 std::this_thread::sleep_for(std::chrono::milliseconds(1));
             if (!context.publish(second, sizeof(second)))
                 throw std::runtime_error("p2 publish failed");
         },
         {[] { return true; }}},
        {key(3), {key(1), key(2)},
         [&](const TaskContext& context) {
             ++consumer_entered;
             context.wait_for_input(0, std::chrono::milliseconds(2000));
             context.wait_for_input(1, std::chrono::milliseconds(2000));
             const BufferVersion a = context.input(0);
             const BufferVersion b = context.input(1);
             const auto* ab = static_cast<const unsigned char*>(a.data);
             const auto* bb = static_cast<const unsigned char*>(b.data);
             if (a.valid() && b.valid() && a.bytes == sizeof(first) && b.bytes == sizeof(second) &&
                 ab && bb && ab[0] == first[0] && bb[0] == second[0] &&
                 (a.generation == Generation{7, 3, 15}) && (b.generation == Generation{7, 3, 15}))
                 ++saw_both;
             publish_token(context);
         },
         {[] { return true; }}}});
    std::thread runner([&] { graph.run(workers); });
    const bool p1_completed = wait_until(
        [&] {
            return p2_entered.load() && graph.state(0) == TaskState::Completed;
        },
        5);
    const TaskState mid = p1_completed ? graph.state(2) : TaskState::Failed;
    const int entered_mid = consumer_entered.load();
    release.store(true);
    runner.join();
    char detail[384];
    std::snprintf(detail, sizeof(detail),
                  "p1_completed_while_p2_held=%d consumer_state_then=%d entered_then=%d "
                  "final_entered=%d saw_both=%d run_error=%d consumer_final=%d",
                  static_cast<int>(p1_completed), static_cast<int>(mid), entered_mid,
                  consumer_entered.load(), saw_both.load(), static_cast<int>(graph.error()),
                  static_cast<int>(graph.state(2)));
    required("S2-removed-dependency",
             p1_completed && mid == TaskState::Pending && entered_mid == 0 &&
                 graph.error() == GraphError::None && graph.quiescent() &&
                 consumer_entered.load() == 1 && saw_both.load() == 1 &&
                 graph.state(2) == TaskState::Completed,
             detail);
}

// ---------------------------------------------------------------------------
// Timeout and cancellation must not ready a dependent. The timeout is a real
// wait_for_input deadline: the consumer clears the source version and waits.
// ---------------------------------------------------------------------------
void rule_timeout_does_not_ready(std::size_t workers) {
    std::atomic<int> tail_runs{0};
    DecodeTaskGraph* graph_ptr = nullptr;
    DecodeTaskGraph graph({7, 3, 16}, GraphProfile::Synthetic, {
        {key(1), {},
         [&](const TaskContext& context) {
             static constexpr unsigned char token = 1;
             if (!context.publish(&token, 1)) throw std::runtime_error("publish failed");
         },
         {[] { return true; }}},
        {key(2), {key(1)},
         [&](const TaskContext& context) {
             DecodeTaskGraphTestAccess::clear_version(*graph_ptr, 0);
             context.wait_for_input(0, std::chrono::milliseconds(40));
         },
         {[] { return true; }}},
        {key(3), {key(2)}, [&](const TaskContext&) { ++tail_runs; }, {[] { return true; }}}});
    graph_ptr = &graph;
    const auto began = std::chrono::steady_clock::now();
    const bool ran = graph.run(workers);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - began)
                             .count();
    char detail[320];
    std::snprintf(detail, sizeof(detail),
                  "run=%d error=%d (expected %d Timeout) tail_runs=%d tail_state=%d elapsed=%lld",
                  static_cast<int>(ran), static_cast<int>(graph.error()),
                  static_cast<int>(GraphError::Timeout), tail_runs.load(),
                  static_cast<int>(graph.state(2)), static_cast<long long>(elapsed));
    required("S2-timeout-does-not-ready",
             !ran && graph.error() == GraphError::Timeout && graph.quiescent() &&
                 tail_runs.load() == 0 && graph.state(1) == TaskState::Failed &&
                 graph.state(2) == TaskState::Failed && not_ready(graph.state(2)) &&
                 elapsed < 3000,
             detail);
}

void rule_cancel_does_not_ready(std::size_t workers) {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<int> consumer_runs{0};
    DecodeTaskGraph graph({7, 3, 17}, GraphProfile::Synthetic, {
        {key(1), {},
         [&](const TaskContext& context) {
             entered.store(true);
             const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
             while (!release.load() && std::chrono::steady_clock::now() < deadline)
                 std::this_thread::sleep_for(std::chrono::milliseconds(1));
             static constexpr unsigned char token = 1;
             (void)context.publish(&token, 1);
         },
         {[] { return true; }}},
        {key(2), {key(1)}, [&](const TaskContext&) { ++consumer_runs; },
         {[] { return true; }}}});
    std::thread runner([&] { graph.run(workers); });
    const bool saw = wait_until([&] { return entered.load(); }, 5);
    const bool pending = saw && graph.state(1) == TaskState::Pending;
    const bool canceled = saw && graph.cancel({7, 3, 17});
    release.store(true);
    runner.join();
    char detail[320];
    std::snprintf(detail, sizeof(detail),
                  "entered=%d pending_before_cancel=%d canceled=%d error=%d producer=%d "
                  "consumer=%d runs=%d",
                  static_cast<int>(saw), static_cast<int>(pending), static_cast<int>(canceled),
                  static_cast<int>(graph.error()), static_cast<int>(graph.state(0)),
                  static_cast<int>(graph.state(1)), consumer_runs.load());
    required("S2-cancel-does-not-ready",
             saw && pending && canceled && graph.quiescent() &&
                 graph.error() == GraphError::Canceled && consumer_runs.load() == 0 &&
                 graph.state(0) == TaskState::Canceled && graph.state(1) == TaskState::Canceled &&
                 not_ready(graph.state(1)),
             detail);
}

// ---------------------------------------------------------------------------
// First error sticks. The no-buffer failure is recorded first; a later
// Allocation from a callback that actually ran must not replace it.
// At one worker the later callback is never claimed, so it must be Canceled
// rather than a fabricated second write.
// ---------------------------------------------------------------------------
void rule_first_error(std::size_t workers) {
    const bool concurrent = workers >= 2;
    std::atomic<int> started{0};
    std::atomic<bool> both{false};
    std::atomic<bool> later_ran{false};
    std::atomic<bool> later_threw{false};
    std::atomic<int> consumer_runs{0};
    DecodeTaskGraph* graph_ptr = nullptr;

    const auto enter = [&] {
        if (started.fetch_add(1) + 1 >= 2) both.store(true);
    };
    const auto await_both = [&] {
        return wait_until([&] { return both.load(); }, 5);
    };

    DecodeTaskGraph graph({7, 3, 18}, GraphProfile::Synthetic, {
        {key(1), {},
         [&](const TaskContext&) {
             enter();
             if (concurrent) await_both();
         },
         {[] { return true; }}},
        {key(2), {},
         [&](const TaskContext&) {
             later_ran.store(true);
             enter();
             if (concurrent) {
                 if (!await_both() ||
                     !wait_until([&] { return graph_ptr->error() != GraphError::None; }, 5))
                     return;
             }
             later_threw.store(true);
             throw std::bad_alloc();
         },
         {[] { return true; }}},
        {key(3), {key(1)}, [&](const TaskContext&) { ++consumer_runs; },
         {[] { return true; }}}});
    graph_ptr = &graph;
    const std::size_t ready_at_start = graph.ready_transitions();
    const bool ran = graph.run(workers);
    const bool witness_ok = concurrent ? later_threw.load() : !later_ran.load();
    const bool later_state_ok = concurrent ? graph.state(1) == TaskState::Failed
                                           : graph.state(1) == TaskState::Canceled;
    char detail[384];
    std::snprintf(detail, sizeof(detail),
                  "concurrent=%d run=%d error=%d (expected %d OutputNotPublished) "
                  "later_ran=%d later_threw=%d later_state=%d consumer_runs=%d ready %zu->%zu",
                  static_cast<int>(concurrent), static_cast<int>(ran),
                  static_cast<int>(graph.error()), static_cast<int>(GraphError::OutputNotPublished),
                  static_cast<int>(later_ran.load()), static_cast<int>(later_threw.load()),
                  static_cast<int>(graph.state(1)), consumer_runs.load(), ready_at_start,
                  graph.ready_transitions());
    required("S2-first-error-not-overwritten",
             !ran && graph.error() == GraphError::OutputNotPublished && witness_ok &&
                 later_state_ok && consumer_runs.load() == 0 &&
                 graph.state(2) == TaskState::Failed && not_ready(graph.state(2)) &&
                 graph.ready_transitions() == ready_at_start && graph.quiescent(),
             detail);
}

} // namespace

int main(int argc, char** argv) {
    std::size_t workers = 1;
    if (argc > 1) workers = static_cast<std::size_t>(std::strtoul(argv[1], nullptr, 10));
    if (workers == 0 || workers > 16) {
        std::printf("usage: %s [workers 1..16]\n", argv[0]);
        return 2;
    }
    std::printf("INFO workers=%zu synthetic S2 output completion only\n", workers);
    rule_noop_producer(workers);
    rule_premature_release(workers);
    rule_missing_publication(workers);
    rule_removed_dependency(workers);
    rule_timeout_does_not_ready(workers);
    rule_cancel_does_not_ready(workers);
    rule_first_error(workers);
    std::printf("SUMMARY %d rule(s) hold, %d broken\n", holds, failures);
    if (failures) {
        std::printf("SCHEDULER-S2-OUTPUT FAIL\n");
        return 1;
    }
    std::printf("SCHEDULER-S2-OUTPUT PASS\n");
    return 0;
}
