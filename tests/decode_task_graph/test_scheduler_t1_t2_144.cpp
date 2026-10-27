// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// T1/T2 gate for the synthetic scheduler (issues #144 / #195), measured against
// this repository's own wrapper in src/core/decode_task_graph.*. No SDK, no
// upstream/ link, no bitstream, no pixel hash: the hash is not an oracle here.
//
// T1 -- topology is rejected before the first worker exists. Measured: the
// structural verdict, the executed-callback counter and the OS thread count.
// T2 -- a versioned publication with release/acquire. Measured: a data-access
// counter that only moves once the acquire-load of a version observed the
// publish, and the payload bytes themselves.
//
// This file is a gate, not a report: every rule either HOLDS or the process
// exits non-zero. There is no partial-credit path.
#include "decode_task_graph.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <dirent.h>
#endif

namespace hypercine::detail {
// Test access to the publication record. observe() deliberately takes NO graph
// mutex: it is the same lock-free acquire sequence a consumer uses, so a publish
// that stored the version before the payload is observable here instead of being
// hidden behind the scheduler's mutex.
struct DecodeTaskGraphTestAccess {
    static void wait(const DecodeTaskGraph& g, std::size_t node, std::size_t input,
                     std::chrono::milliseconds timeout) {
        g.wait_for_input(node, input, timeout);
    }
    static BufferVersion observe(const DecodeTaskGraph& g, std::size_t node) {
        BufferVersion view;
        if (node >= g.nodes_.size()) return view;
        const auto& source = g.nodes_[node];
        // Acquire first. published_generation and the payload are written
        // before the release-store, so they are read only after that store.
        view.version = source.version.load(std::memory_order_acquire);
        if (view.version != 0) {
            view.generation = source.published_generation;
            view.data = source.payload.load(std::memory_order_relaxed);
            view.bytes = source.payload_bytes.load(std::memory_order_relaxed);
        }
        return view;
    }
};
} // namespace hypercine::detail

namespace {

using namespace hypercine::detail;

const Generation kGeneration{7, 3, 1};
constexpr std::size_t kPayload = 64;

TaskKey key(std::uint32_t band, std::uint32_t channel = 0) {
    return {channel, 0, 0, 0, band};
}

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

// Thread count of this process, or 0 where the platform does not expose it.
std::size_t thread_count() {
#if defined(__linux__)
    std::size_t count = 0;
    if (DIR* dir = opendir("/proc/self/task")) {
        while (dirent* entry = readdir(dir))
            if (entry->d_name[0] != '.') ++count;
        closedir(dir);
    }
    return count;
#else
    return 0;
#endif
}

bool rejected(std::vector<TaskSpec> tasks, GraphProfile profile = GraphProfile::Synthetic,
              Generation generation = kGeneration) {
    const GraphError verdict = DecodeTaskGraph::validate(tasks, profile, generation);
    if (verdict != GraphError::InvalidGraph) return false;
    try {
        DecodeTaskGraph graph(generation, profile, std::move(tasks));
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// T1 -- cycle, unknown producer and duplicate key are InvalidGraph before any
// worker thread exists. The callback counter proves "before", not "after".
// ---------------------------------------------------------------------------
void rule_t1_topology_before_workers() {
    std::atomic<int> callbacks{0};
    const auto body = [&callbacks](const TaskContext&) { callbacks.fetch_add(1); };

    // These fixtures are rejected before any predicate could be evaluated, so
    // the contract slot is explicitly empty rather than silently omitted.
    const std::function<bool()> none;
    const std::size_t threads_before = thread_count();
    // Two-node cycle: neither node can ever be released.
    const bool cycle = rejected({{key(1), {key(2)}, body, none}, {key(2), {key(1)}, body, none}});
    // Self cycle.
    const bool self_cycle = rejected({{key(1), {key(1)}, body, none}});
    // Input naming a TaskKey that no task declares.
    const bool unknown = rejected({{key(1), {}, body, none}, {key(2), {key(9)}, body, none}});
    // The same key declared twice.
    const bool duplicate = rejected({{key(1), {}, body, none}, {key(1), {}, body, none}});
    // The same edge declared twice by one consumer.
    const bool duplicate_edge = rejected({{key(1), {}, body, none}, {key(2), {key(1), key(1)}, body, none}});
    // A three-node cycle beside two valid roots: the roots are schedulable, so a
    // check that only proves "the roots can start" would admit the whole graph.
    const bool mixed = rejected({{key(1), {}, body, none},
                                  {key(2), {}, body, none},
                                  {key(3), {key(4)}, body, none},
                                  {key(4), {key(3)}, body, none}});
    const std::size_t threads_after = thread_count();
    const bool no_side_effects = callbacks.load() == 0 &&
                                 (threads_before == 0 || threads_before == threads_after);

    required("T1-cycle-rejected-before-worker",
             cycle && self_cycle && mixed,
             "cycle, self-cycle and a cycle beside valid roots are InvalidGraph at validate() "
             "and at construction");
    required("T1-unknown-producer-rejected",
             unknown,
             "an input naming an undeclared TaskKey is InvalidGraph before any worker exists");
    required("T1-duplicate-key-rejected",
             duplicate && duplicate_edge,
             "a duplicate TaskKey and a duplicate input edge are InvalidGraph before any "
             "worker exists");
    required("T1-zero-callbacks-zero-threads",
             no_side_effects,
             "none of the six rejected topologies executed a callback or changed the "
             "process thread count");
}

// ---------------------------------------------------------------------------
// T2 -- delayed publication. The producer writes a known payload, then holds
// inside publish() after the payload record and before its release. An observer
// samples the same acquire path a consumer uses and counts every data access it
// makes. This is the exact window the ordering claim is about; a publish that
// stored the version before the payload would be visible here, because the
// observer takes no lock and reads the payload fields directly.
// ---------------------------------------------------------------------------
struct Hold {
    std::atomic<bool> entered{false}, released{false};
    std::atomic<long> reads{0}, reads_with_version{0}, observations{0};
    std::size_t null_span = 0, wrong_bytes = 0;
};

Hold* active_hold = nullptr;

void hold_seam() {
    Hold& hold = *active_hold;
    hold.entered.store(true);
    while (!hold.released.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void rule_t2_no_read_before_release() {
    Hold hold;
    active_hold = &hold;
    std::vector<unsigned char> payload(kPayload, 0x5A); // pre-publish fill
    // Content the producer writes: not the 0x5A fill and not zeros, so "read the
    // old bytes" and "read zeros" are both distinguishable outcomes.
    const auto expected = [](std::size_t i) {
        return static_cast<unsigned char>(0xA5 ^ (i & 0x0F));
    };
    std::atomic<int> consumer_reads{0};
    std::atomic<int> consumer_failures{0};
    std::atomic<bool> consumer_ok{false};

    DecodeTaskGraph graph(kGeneration, GraphProfile::Synthetic, {
        {key(1), {},
         [&](const TaskContext& context) {
             for (std::size_t i = 0; i < kPayload; ++i) payload[i] = expected(i);
             consumer_ok.store(context.publish(payload.data(), payload.size()));
         },
         {[&] { return consumer_ok.load(); }}},
        {key(2), {key(1)},
         [&](const TaskContext& context) {
             // The gate: no payload access until the version was observed.
             try {
                 context.wait_for_input(0, std::chrono::milliseconds(3000));
             } catch (const PublicationTimeout&) {
                 ++consumer_failures;
                 return;
             }
             const BufferVersion view = context.input(0);
             if (!view.valid() || !view.data || view.bytes != kPayload) {
                 ++consumer_failures;
                 return;
             }
             const auto* bytes = static_cast<const unsigned char*>(view.data);
             for (std::size_t i = 0; i < kPayload; ++i)
                 if (bytes[i] != expected(i)) ++consumer_failures;
             ++consumer_reads;
         },
         {[] { return true; }}}});

    DecodeTaskGraph::set_publish_seam(&hold_seam);
    std::thread runner([&] { graph.run(2); });
    while (!hold.entered.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));

    // Sample for longer than the whole publish-hold window. Every observation is
    // an independent attempt to read a version before its payload.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    while (std::chrono::steady_clock::now() < until) {
        ++hold.observations;
        const BufferVersion view = DecodeTaskGraphTestAccess::observe(graph, 0);
        if (!view.valid()) continue;
        ++hold.reads_with_version;
        if (!view.data || view.bytes != kPayload) ++hold.null_span;
        else {
            ++hold.reads;
            const auto* bytes = static_cast<const unsigned char*>(view.data);
            for (std::size_t i = 0; i < kPayload; ++i)
                if (bytes[i] != expected(i)) { ++hold.wrong_bytes; break; }
        }
    }
    hold.released.store(true);
    runner.join();
    DecodeTaskGraph::set_publish_seam(nullptr);

    const BufferVersion final = DecodeTaskGraphTestAccess::observe(graph, 0);
    char detail[512];
    std::snprintf(detail, sizeof(detail),
                  "inside the hold: observations=%ld versioned=%ld payload_reads=%ld "
                  "null_spans=%zu wrong_bytes=%zu; after release: consumer_reads=%d "
                  "consumer_failures=%d final_bytes=%zu (expected 0 versioned, 1 read, "
                  "0 failures, %zu bytes)",
                  hold.observations.load(), hold.reads_with_version.load(), hold.reads.load(),
                  hold.null_span, hold.wrong_bytes, consumer_reads.load(),
                  consumer_failures.load(), final.bytes, kPayload);
    required("T2-no-payload-access-before-release",
             hold.reads_with_version.load() == 0 && hold.reads.load() == 0 &&
                 hold.null_span == 0 && hold.wrong_bytes == 0 && consumer_reads.load() == 1 &&
                 consumer_failures.load() == 0 && final.valid() && final.bytes == kPayload,
             detail);
}

// A version is a generation, not a bare pointer: the same version must not be
// reusable for another generation, and a second publish must not replace bytes a
// consumer may already hold.
void rule_t2_version_is_generation_bound() {
    std::atomic<int> published_twice{0};
    bool refused_second = false;
    DecodeTaskGraph graph({7, 3, 2}, GraphProfile::Synthetic, {
        {key(1), {},
         [&](const TaskContext& context) {
             static std::size_t first_value = 1;
             static std::size_t second_value = 2;
             if (context.publish(&first_value, sizeof(first_value))) ++published_twice;
             refused_second = !context.publish(&second_value, sizeof(second_value));
         },
         {[] { return true; }}}});
    const bool ok = graph.run(1);
    required("T2-version-single-publish",
             ok && published_twice.load() == 1 && refused_second &&
                 graph.uncontracted_tasks() == 0,
             "one generation publishes one version; a second publish for the same task and "
             "generation is refused instead of silently replacing the span");
}

// ---------------------------------------------------------------------------
// T2 -- deadline, and the closed escape hatch.
//
// A producer that returns without publishing no longer Completes and no longer
// releases its consumer (that was the RCA-195 assumption). The consumer must
// not become Ready. The bounded wait itself is still terminal: asked directly
// about an unpublished edge, wait_for_input throws PublicationTimeout, does not
// hang, and does not hand back a payload.
// ---------------------------------------------------------------------------
void rule_t2_deadline_is_terminal() {
    std::atomic<int> consumer_runs{0};
    DecodeTaskGraph graph(kGeneration, GraphProfile::Synthetic, {
        {key(1), {}, [](const TaskContext&) { /* returns, publishes nothing */ },
         {/* no predicate and no buffer: must not Complete */}},
        {key(2), {key(1)}, [&](const TaskContext&) { ++consumer_runs; }, {[] { return true; }}}});
    const auto began = std::chrono::steady_clock::now();
    const bool ran = graph.run(1);
    const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - began).count();

    bool threw = false;
    long long elapsed_ms = -1;
    DecodeTaskGraph parked({7, 3, 9}, GraphProfile::Synthetic, {
        {key(1), {}, [](const TaskContext&) {}, {[] { return true; }}},
        {key(2), {key(1)}, [](const TaskContext&) {}, {[] { return true; }}}});
    const auto wait_began = std::chrono::steady_clock::now();
    try {
        DecodeTaskGraphTestAccess::wait(parked, 1, 0, std::chrono::milliseconds(50));
    } catch (const PublicationTimeout&) {
        threw = true;
        elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - wait_began)
                         .count();
    }
    const BufferVersion unseen = DecodeTaskGraphTestAccess::observe(parked, 0);
    const bool consumer_not_ready = graph.state(1) != TaskState::Ready &&
                                    graph.state(1) != TaskState::Running &&
                                    graph.state(1) != TaskState::Completed;
    required("T2-deadline-terminal-not-hang",
             !ran && graph.error() == GraphError::OutputNotPublished && graph.quiescent() &&
                 consumer_runs.load() == 0 && consumer_not_ready &&
                 graph.state(0) == TaskState::Failed && total_ms < 3000 && threw &&
                 elapsed_ms >= 0 && elapsed_ms < 2000 && !unseen.valid(),
             "an unpublished producer is OutputNotPublished and does not ready its "
             "consumer; a direct wait on that unpublished edge throws PublicationTimeout "
             "instead of hanging or returning a payload");
}

// ---------------------------------------------------------------------------
// publish() null / zero-length. A positive length with no address, a zero-length
// span, and a null zero-length call are not publications. Each must return
// false and leave version, payload, payload_bytes, TaskState and the ready
// counter untouched. The version word is the publication counter; there is no
// second one. A later real publish of the same task is still admitted.
// ---------------------------------------------------------------------------
// One graph, one publish. Isolating each shape is required: a single task can
// publish only once, so a later call that returns false can be the second-publish
// refusal rather than the null/zero-length rule.
bool reject_publish_isolated(const void* data, std::size_t bytes, Generation generation,
                             std::string* detail) {
    bool rejected = false;
    bool running_before = false, running_after = false;
    long long ready_before = -1, ready_after = -1;
    BufferVersion seen{};
    DecodeTaskGraph* graph_ptr = nullptr;
    DecodeTaskGraph graph(generation, GraphProfile::Synthetic, {
        {key(1), {},
         [&](const TaskContext& context) {
             running_before = graph_ptr->state(0) == TaskState::Running;
             ready_before = static_cast<long long>(graph_ptr->ready_transitions());
             rejected = !context.publish(data, bytes);
             seen = DecodeTaskGraphTestAccess::observe(*graph_ptr, 0);
             running_after = graph_ptr->state(0) == TaskState::Running;
             ready_after = static_cast<long long>(graph_ptr->ready_transitions());
         },
         {/* no output contract: rejection itself must not fail the task */}}});
    graph_ptr = &graph;
    const bool ran = graph.run(1);
    // Rejection itself must not publish. Returning from the callback with no
    // later valid publish must also not Complete: there is still no buffer.
    const bool holds = !ran && rejected && running_before && running_after && seen.version == 0 &&
                       seen.data == nullptr && seen.bytes == 0 && ready_before >= 0 &&
                       ready_before == ready_after && graph.state(0) == TaskState::Failed &&
                       graph.error() == GraphError::OutputNotPublished;
    char line[256];
    std::snprintf(line, sizeof(line),
                  "[bytes=%zu data=%s rejected=%d version=%llu payload=%s span=%zu "
                  "running %d->%d ready %lld->%lld run=%d state=%d]",
                  bytes, data == nullptr ? "null" : "set", static_cast<int>(rejected),
                  static_cast<unsigned long long>(seen.version),
                  seen.data == nullptr ? "null" : "set", seen.bytes,
                  static_cast<int>(running_before), static_cast<int>(running_after),
                  ready_before, ready_after, static_cast<int>(ran),
                  static_cast<int>(graph.state(0)));
    if (!detail->empty()) *detail += ' ';
    *detail += line;
    return holds;
}

void rule_publish_rejects_null_and_zero_length() {
    unsigned char byte = 0x11;
    std::string detail;
    // Each shape on its own task, so a refusal cannot be the single-version rule.
    const bool null_positive = reject_publish_isolated(nullptr, 16, {7, 3, 4}, &detail);
    const bool zero_length = reject_publish_isolated(&byte, 0, {7, 3, 6}, &detail);
    const bool null_zero = reject_publish_isolated(nullptr, 0, {7, 3, 7}, &detail);

    // Rejections must not consume the one publication. Three refusals, then one
    // real publish, on a single task.
    bool rej_null = false, rej_zero = false, rej_both = false, accepted = false;
    bool still_running = false;
    DecodeTaskGraph* graph_ptr = nullptr;
    DecodeTaskGraph graph({7, 3, 8}, GraphProfile::Synthetic, {
        {key(1), {},
         [&](const TaskContext& context) {
             rej_null = !context.publish(nullptr, 16);
             rej_zero = !context.publish(&byte, 0);
             rej_both = !context.publish(nullptr, 0);
             still_running = graph_ptr->state(0) == TaskState::Running;
             accepted = context.publish(&byte, 1);
         },
         {[&] { return accepted; }}}});
    graph_ptr = &graph;
    const bool ran = graph.run(1);
    const BufferVersion final_view = DecodeTaskGraphTestAccess::observe(graph, 0);
    char follow[256];
    std::snprintf(follow, sizeof(follow),
                  " follow reject %d/%d/%d running=%d later=%d run=%d final_version=%llu "
                  "final_bytes=%zu",
                  static_cast<int>(rej_null), static_cast<int>(rej_zero),
                  static_cast<int>(rej_both), static_cast<int>(still_running),
                  static_cast<int>(accepted), static_cast<int>(ran),
                  static_cast<unsigned long long>(final_view.version), final_view.bytes);
    detail += follow;
    required("T2-publish-rejects-null-and-zero",
             null_positive && zero_length && null_zero && ran && rej_null && rej_zero &&
                 rej_both && still_running && accepted &&
                 graph.state(0) == TaskState::Completed && final_view.valid() &&
                 final_view.bytes == 1 && final_view.data != nullptr &&
                 *static_cast<const unsigned char*>(final_view.data) == byte,
             detail);
}

// ---------------------------------------------------------------------------
// wait_for_input precedence. A buffer that was published earlier must not let a
// consumer return once a stop or a recorded error exists. The consumer is
// already inside its callback, the version is already non-zero, and only then
// is the decision recorded. Checking the version first would admit the payload.
// ---------------------------------------------------------------------------
void rule_stop_precedes_published_version(std::size_t workers) {
    std::vector<unsigned char> payload(kPayload, 0x3C);
    std::atomic<bool> consumer_entered{false};
    std::atomic<int> wait_admitted{0};
    std::atomic<int> wait_threw{0};
    std::atomic<int> payload_reads{0};
    DecodeTaskGraph* graph_ptr = nullptr;
    DecodeTaskGraph graph(kGeneration, GraphProfile::Synthetic, {
        {key(1), {},
         [&](const TaskContext& context) {
             if (!context.publish(payload.data(), payload.size()))
                 throw std::runtime_error("producer publish failed");
         },
         {[] { return true; }}},
        {key(2), {key(1)},
         [&](const TaskContext& context) {
             consumer_entered.store(true);
             const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
             while (graph_ptr->error() == GraphError::None &&
                    std::chrono::steady_clock::now() < until)
                 std::this_thread::sleep_for(std::chrono::milliseconds(1));
             try {
                 context.wait_for_input(0, std::chrono::milliseconds(1000));
                 ++wait_admitted;
                 const BufferVersion view = context.input(0);
                 if (view.valid() && view.data != nullptr && view.bytes == kPayload)
                     ++payload_reads;
             } catch (const PublicationTimeout&) {
                 ++wait_threw;
                 throw;
             }
         },
         {[] { return true; }}}});
    graph_ptr = &graph;
    std::thread runner([&] { graph.run(workers); });
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!consumer_entered.load() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool canceled = consumer_entered.load() && graph.cancel(kGeneration);
    runner.join();
    const BufferVersion published = DecodeTaskGraphTestAccess::observe(graph, 0);
    char detail[512];
    std::snprintf(detail, sizeof(detail),
                  "canceled=%d entered=%d admitted=%d threw=%d payload_reads=%d "
                  "error=%d (expected %d Canceled) producer_state=%d consumer_state=%d "
                  "published_version=%llu",
                  static_cast<int>(canceled), static_cast<int>(consumer_entered.load()),
                  wait_admitted.load(), wait_threw.load(), payload_reads.load(),
                  static_cast<int>(graph.error()), static_cast<int>(GraphError::Canceled),
                  static_cast<int>(graph.state(0)), static_cast<int>(graph.state(1)),
                  static_cast<unsigned long long>(published.version));
    required("T2-stop-precedes-published-version",
             canceled && wait_admitted.load() == 0 && wait_threw.load() == 1 &&
                 payload_reads.load() == 0 && graph.error() == GraphError::Canceled &&
                 graph.quiescent() && graph.state(0) == TaskState::Completed &&
                 graph.state(1) == TaskState::Failed && published.valid(),
             detail);
}

// Same precedence for a recorded callback error rather than cancel(). The error
// node waits until the consumer is inside its callback, so the version is
// already published when the slot is written. Needs two workers: with one, the
// error node cannot run while the consumer spins. The scenario therefore runs
// at max(workers, 2).
void rule_recorded_error_precedes_published_version(std::size_t workers) {
    const std::size_t width = workers < 2 ? 2 : workers;
    std::vector<unsigned char> payload(kPayload, 0xC3);
    std::atomic<bool> consumer_entered{false};
    std::atomic<int> wait_admitted{0};
    std::atomic<int> wait_threw{0};
    std::atomic<int> payload_reads{0};
    DecodeTaskGraph* graph_ptr = nullptr;
    DecodeTaskGraph graph({7, 3, 5}, GraphProfile::Synthetic, {
        {key(1), {},
         [&](const TaskContext& context) {
             if (!context.publish(payload.data(), payload.size()))
                 throw std::runtime_error("producer publish failed");
         },
         {[] { return true; }}},
        {key(2), {},
         [&](const TaskContext&) {
             const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
             while (!consumer_entered.load() && std::chrono::steady_clock::now() < until)
                 std::this_thread::sleep_for(std::chrono::milliseconds(1));
             throw std::runtime_error("recorded error after an earlier publish");
         },
         {[] { return true; }}},
        {key(3), {key(1)},
         [&](const TaskContext& context) {
             consumer_entered.store(true);
             const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
             while (graph_ptr->error() == GraphError::None &&
                    std::chrono::steady_clock::now() < until)
                 std::this_thread::sleep_for(std::chrono::milliseconds(1));
             try {
                 context.wait_for_input(0, std::chrono::milliseconds(1000));
                 ++wait_admitted;
                 const BufferVersion view = context.input(0);
                 if (view.valid() && view.data != nullptr && view.bytes == kPayload)
                     ++payload_reads;
             } catch (const PublicationTimeout&) {
                 ++wait_threw;
                 throw;
             }
         },
         {[] { return true; }}}});
    graph_ptr = &graph;
    const bool ran = graph.run(width);
    const BufferVersion published = DecodeTaskGraphTestAccess::observe(graph, 0);
    char detail[512];
    std::snprintf(detail, sizeof(detail),
                  "width=%zu entered=%d admitted=%d threw=%d payload_reads=%d "
                  "error=%d (expected %d Callback) consumer_state=%d "
                  "published_version=%llu run=%d",
                  width, static_cast<int>(consumer_entered.load()), wait_admitted.load(),
                  wait_threw.load(), payload_reads.load(), static_cast<int>(graph.error()),
                  static_cast<int>(GraphError::Callback), static_cast<int>(graph.state(2)),
                  static_cast<unsigned long long>(published.version), static_cast<int>(ran));
    required("T2-error-precedes-published-version",
             !ran && consumer_entered.load() && wait_admitted.load() == 0 &&
                 wait_threw.load() == 1 && payload_reads.load() == 0 &&
                 graph.error() == GraphError::Callback && graph.quiescent() &&
                 graph.state(2) == TaskState::Failed && published.valid(),
             detail);
}

// ---------------------------------------------------------------------------
// Non-blocking abort. Two producers, one consumer. A publishes a real buffer
// and completes, which is what releases the consumer. B then records a callback
// error while the consumer is already inside its callback. The consumer must
// not call wait_for_input(); it probes input_published() and input() directly.
// A version that was published earlier must not become a readable view after
// that error: zero payload bytes may be read.
//
// Needs two workers so B can run while the consumer is inside the callback.
// At workers == 1 the scenario therefore runs at width 2, same as the waiting
// error-precedence rule above. Removing the stop/error admission in
// admitted_input() makes input_published() succeed and input() hand out the
// buffer, so this rule goes BROKEN. That is the negative control.
// ---------------------------------------------------------------------------
void rule_nonblocking_abort_admits_no_payload(std::size_t workers) {
    const std::size_t width = workers < 2 ? 2 : workers;
    const Generation generation{7, 3, 21};
    std::vector<unsigned char> payload(kPayload);
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<unsigned char>(0x40 + (i & 0x0F));
    std::atomic<bool> consumer_entered{false};
    std::atomic<int> published_probes{0};
    std::atomic<int> payload_reads{0};
    std::atomic<int> partial_views{0};
    std::atomic<unsigned> payload_sink{0};
    DecodeTaskGraph* graph_ptr = nullptr;
    DecodeTaskGraph graph(generation, GraphProfile::Synthetic, {
        {key(1), {},
         [&](const TaskContext& context) {
             if (!context.publish(payload.data(), payload.size()))
                 throw std::runtime_error("producer A publish failed");
         },
         {[] { return true; }}},
        {key(2), {},
         [&](const TaskContext&) {
             const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
             while (!consumer_entered.load() && std::chrono::steady_clock::now() < until)
                 std::this_thread::sleep_for(std::chrono::milliseconds(1));
             throw std::runtime_error("producer B callback error after A published");
         },
         {[] { return true; }}},
        {key(3), {key(1)},
         [&](const TaskContext& context) {
             consumer_entered.store(true);
             const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
             while (graph_ptr->error() == GraphError::None &&
                    std::chrono::steady_clock::now() < until)
                 std::this_thread::sleep_for(std::chrono::milliseconds(1));
             // No wait_for_input(). These two probes are the whole gate.
             if (context.input_published(0)) ++published_probes;
             const BufferVersion view = context.input(0);
             const bool empty = !view.valid() && view.version == 0 && view.data == nullptr &&
                                view.bytes == 0 && view.generation.session == 0 &&
                                view.generation.slot == 0 && view.generation.value == 0;
             if (!empty) ++partial_views;
             const bool readable = view.valid() && view.data != nullptr && view.bytes == kPayload &&
                                   (view.generation == generation);
             if (readable) {
                 const auto* bytes = static_cast<const unsigned char*>(view.data);
                 unsigned sum = 0;
                 for (std::size_t i = 0; i < view.bytes; ++i) sum += bytes[i];
                 payload_sink.fetch_add(sum + 1, std::memory_order_relaxed);
                 ++payload_reads;
             }
         },
         {[] { return true; }}}});
    graph_ptr = &graph;
    const bool ran = graph.run(width);
    const BufferVersion published = DecodeTaskGraphTestAccess::observe(graph, 0);
    char detail[640];
    std::snprintf(detail, sizeof(detail),
                  "width=%zu entered=%d published_probes=%d partial_views=%d "
                  "payload_reads=%d (expected 0) error=%d (expected %d Callback) "
                  "A=%d B=%d consumer=%d published_version=%llu run=%d",
                  width, static_cast<int>(consumer_entered.load()), published_probes.load(),
                  partial_views.load(), payload_reads.load(), static_cast<int>(graph.error()),
                  static_cast<int>(GraphError::Callback), static_cast<int>(graph.state(0)),
                  static_cast<int>(graph.state(1)), static_cast<int>(graph.state(2)),
                  static_cast<unsigned long long>(published.version), static_cast<int>(ran));
    required("T2-nonblocking-abort-admits-no-payload",
             !ran && consumer_entered.load() && published_probes.load() == 0 &&
                 partial_views.load() == 0 && payload_reads.load() == 0 &&
                 graph.error() == GraphError::Callback && graph.quiescent() &&
                 graph.state(0) == TaskState::Completed && graph.state(1) == TaskState::Failed &&
                 graph.state(2) == TaskState::Canceled && published.valid() &&
                 published.bytes == kPayload,
             detail);
}

// ---------------------------------------------------------------------------
// First-Error, write-once. Two producers are made Running at the same time by a
// rendezvous; the first records Callback, the second (already Running, so its
// completion is accepted) raises Allocation. The slot must keep Callback, and no
// successor may become Ready, Running or Completed afterwards.
//
// The rendezvous is why this rule needs two workers: with one, the second
// producer is never Running and its later error is not a second write at all.
// The scenario therefore runs at max(workers, 2) and says so; the width
// parameter is exercised by T1/T2 above.
// ---------------------------------------------------------------------------
void rule_first_error_write_once(std::size_t workers) {
    std::atomic<bool> first_running{false}, second_running{false};
    std::atomic<long long> ready_at_failure{-1};
    std::atomic<int> consumer_runs{0};
    DecodeTaskGraph* graph_ptr = nullptr;
    DecodeTaskGraph graph(kGeneration, GraphProfile::Synthetic, {
        {key(1), {},
         [&](const TaskContext&) {
             first_running.store(true);
             // Wait until both producers are inside their callbacks, so the
             // second error cannot be "a later admission" but a real second
             // write to an occupied slot.
             const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
             while (!second_running.load() && std::chrono::steady_clock::now() < until)
                 std::this_thread::sleep_for(std::chrono::milliseconds(1));
             throw std::runtime_error("first producer fails");
         },
         {[] { return true; }}},
        {key(2), {},
         [&](const TaskContext&) {
             second_running.store(true);
             const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
             while (graph_ptr->error() == GraphError::None &&
                    std::chrono::steady_clock::now() < until)
                 std::this_thread::sleep_for(std::chrono::milliseconds(1));
             ready_at_failure.store(static_cast<long long>(graph_ptr->ready_transitions()));
             throw std::bad_alloc();
         },
         {[] { return true; }}},
        {key(3), {key(1), key(2)}, [&](const TaskContext&) { ++consumer_runs; },
         {[] { return true; }}}});
    graph_ptr = &graph;
    const bool ran = graph.run(workers < 2 ? 2 : workers);
    const std::size_t ready_final = graph.ready_transitions();

    bool any_ready = false, any_completed = false, any_running = false;
    for (std::size_t i = 0; i < 3; ++i) {
        any_ready = any_ready || graph.state(i) == TaskState::Ready;
        any_completed = any_completed || graph.state(i) == TaskState::Completed;
        any_running = any_running || graph.state(i) == TaskState::Running;
    }
    const bool observed = ready_at_failure.load() >= 0;
    const bool frozen = !observed ||
                        ready_final == static_cast<std::size_t>(ready_at_failure.load());
    char detail[512];
    std::snprintf(detail, sizeof(detail),
                  "both producers entered=%d/%d; slot=%d (expected %d Callback); "
                  "ready_transitions at failure=%lld, final=%zu; consumer_runs=%d",
                  static_cast<int>(first_running.load()), static_cast<int>(second_running.load()),
                  static_cast<int>(graph.error()), static_cast<int>(GraphError::Callback),
                  ready_at_failure.load(), ready_final, consumer_runs.load());
    required("First-Error-write-once",
             !ran && graph.error() == GraphError::Callback && !any_ready && !any_completed &&
                 !any_running && consumer_runs.load() == 0 && observed && frozen,
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
    std::printf("INFO workers=%zu\n", workers);
    rule_t1_topology_before_workers();
    rule_t2_no_read_before_release();
    rule_t2_version_is_generation_bound();
    rule_t2_deadline_is_terminal();
    rule_publish_rejects_null_and_zero_length();
    rule_stop_precedes_published_version(workers);
    rule_recorded_error_precedes_published_version(workers);
    rule_nonblocking_abort_admits_no_payload(workers);
    rule_first_error_write_once(workers);
    std::printf("SUMMARY %d rule(s) hold, %d broken\n", holds, failures);
    if (failures) {
        std::printf("SCHEDULER-T1T2 FAIL\n");
        return 1;
    }
    std::printf("SCHEDULER-T1T2 PASS\n");
    return 0;
}
