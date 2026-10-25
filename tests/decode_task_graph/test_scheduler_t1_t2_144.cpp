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
    static BufferVersion observe(const DecodeTaskGraph& g, std::size_t node) {
        BufferVersion view;
        if (node >= g.nodes_.size()) return view;
        const auto& source = g.nodes_[node];
        view.generation = g.generation_;
        view.version = source.version.load(std::memory_order_acquire);
        if (view.version != 0) {
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
// T2 -- deadline. A producer returns without publishing (no output contract
// declared, i.e. exactly the RCA-195 assumption), its consumer is released and
// waits on that edge. The wait must end in a defined error, not a hang, not a
// spin and not a read.
// ---------------------------------------------------------------------------
void rule_t2_deadline_is_terminal() {
    std::atomic<int> data_accesses{0};
    std::atomic<long long> elapsed_ms{-1};
    DecodeTaskGraph graph(kGeneration, GraphProfile::Synthetic, {
        {key(1), {}, [](const TaskContext&) { /* returns, publishes nothing */ },
         {/* deliberately no contract: this is the RCA-195 assumption */}},
        {key(2), {key(1)},
         [&](const TaskContext& context) {
             const auto begin = std::chrono::steady_clock::now();
             bool loaded = false;
             try {
                 context.wait_for_input(0, std::chrono::milliseconds(50));
                 loaded = context.input_published(0);
             } catch (const PublicationTimeout&) {
                 elapsed_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - begin).count());
                 // A deadline is terminal, not a retry hint: rethrow so the node
                 // fails closed instead of continuing with an unreadable edge.
                 throw;
             }
             if (loaded) ++data_accesses;
         },
         {[] { return true; }}}});
    const auto began = std::chrono::steady_clock::now();
    const bool ran = graph.run(1);
    const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - began).count();
    required("T2-deadline-terminal-not-hang",
             !ran && graph.error() == GraphError::Timeout && graph.quiescent() &&
                 data_accesses.load() == 0 && elapsed_ms.load() >= 0 &&
                 elapsed_ms.load() < 2000 && total_ms < 3000 &&
                 graph.state(1) == TaskState::Failed,
             "an unpublished edge with an expired deadline is a terminal Timeout: run() "
             "returned, the consumer failed, and no payload was touched");
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
    rule_first_error_write_once(workers);
    std::printf("SUMMARY %d rule(s) hold, %d broken\n", holds, failures);
    if (failures) {
        std::printf("SCHEDULER-T1T2 FAIL\n");
        return 1;
    }
    std::printf("SCHEDULER-T1T2 PASS\n");
    return 0;
}
