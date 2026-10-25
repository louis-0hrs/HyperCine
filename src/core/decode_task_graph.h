// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <vector>

namespace hypercine::detail {

// Synthetic infrastructure only: no codec profile is qualified by this module.
enum class GraphProfile { Synthetic, Unsupported };
enum class TaskState { Pending, Ready, Running, Completed, Failed, Canceled };
// OutputNotPublished is a terminal, fail-closed error: a task declared a
// publication predicate and that predicate reported its output does not exist.
// The node fails, no successor is released, and run() returns false. Appended
// last so existing enumerator values stay stable for pinned ordinal checks.
// Timeout (also appended last) is the bounded-wait failure: an input edge was
// still unpublished when the consumer's deadline expired, or the graph stopped
// while it waited. A consumer that sees it has read no payload byte.
enum class GraphError {
    None, InvalidGraph, Allocation, Callback, Canceled, Worker, OutputNotPublished, Timeout
};
struct Generation {
    std::uint64_t session, slot, value;
    bool operator==(const Generation& b) const noexcept {
        return session == b.session && slot == b.slot && value == b.value;
    }
};
struct TaskKey {
    std::uint32_t channel, transform, level, frame, band;
    bool operator==(const TaskKey& b) const noexcept {
        return channel == b.channel && transform == b.transform && level == b.level &&
               frame == b.frame && band == b.band;
    }
};

// One immutable, published version of one task output.
//   version == 0  : nothing has been published for that task in this generation.
//   version >= 1  : data/bytes describe the payload of that version and are
//                   valid for reading. The version is only ever produced by
//                   DecodeTaskGraph::publish(), whose release-store orders the
//                   producer's payload stores before the consumer's acquire.
// A BufferVersion carries the generation it was published for; a consumer must
// compare `generation` against its own graph generation before reading.
struct BufferVersion {
    Generation generation{};
    std::uint64_t version = 0;
    const void* data = nullptr;
    std::size_t bytes = 0;
    bool valid() const noexcept { return version != 0; }
};

// Thrown by TaskContext::wait_for_input() when the deadline expires, the graph
// was canceled, or an earlier error stopped the generation. It is a terminal
// condition, not a retry hint: the caller has observed no payload bytes.
class PublicationTimeout : public std::exception {
public:
    const char* what() const noexcept override { return "input publication deadline expired"; }
};

class DecodeTaskGraph;
class TaskContext {
public:
    bool cancellation_requested() const;

    // Producer side. Publishes the bytes at `data` as the next version of this
    // task's output. The payload record is written before the release-store of
    // the version; a consumer can only observe the version after that store.
    // Returns false when the graph already failed, was canceled, or this task
    // is no longer Running -- the payload is then NOT published. `data` must
    // outlive the graph; this graph never copies or frees it.
    // const: the callback receives a const TaskContext&; publication moves the
    // graph's own record, not this handle.
    bool publish(const void* data, std::size_t bytes) const noexcept;

    // Consumer side, bounded. Waits (condition variable, no spin) until the
    // `index`-th declared input has been published, then returns. Throws
    // PublicationTimeout if the deadline expires, the graph is canceled, or an
    // error was recorded. It never returns for an unpublished edge, so a
    // caller cannot read before publication by ignoring a return value.
    void wait_for_input(std::size_t index, std::chrono::milliseconds timeout) const;

    // Read-only probe of the same gate, without waiting. Returns true only when
    // the acquire-load of that input's version observed a publish.
    bool input_published(std::size_t index) const;

    // const view of a published input; empty (valid() == false) when nothing
    // was published. Only meaningful after wait_for_input() returned, or when
    // input_published() is true.
    BufferVersion input(std::size_t index) const;

private:
    friend class DecodeTaskGraph;
    TaskContext(DecodeTaskGraph& graph, std::size_t node) : graph_(graph), node_(node) {}
    DecodeTaskGraph& graph_;
    std::size_t node_;
};
struct TaskSpec {
    TaskKey key;
    std::vector<TaskKey> inputs;
    std::function<void(const TaskContext&)> execute;
    // Publication predicate (rule S2). When set, the worker that ran execute()
    // evaluates it immediately afterwards on that same thread, and only then
    // hands the result to complete(). A false or throwing predicate becomes a
    // terminal graph error: the node is Failed, its transitive dependents are
    // Failed, and no consumer is released. It is evaluated without the graph
    // lock held and may call the const query API; the Completed transition it
    // gates is still made under the lock.
    //
    // Lifetime requirement -- note what is NOT guaranteed: execute() has already
    // returned when published() is evaluated, so the callback's stack frame is
    // gone. Capturing a local of the callback body is therefore a dangling read,
    // not a supported pattern. The real requirement is on the produced storage:
    // it must be owned independently of the callback frame (heap object, shared
    // ownership, or a member of a longer-lived owner) and must stay alive from
    // the point the callback publishes it until run() has drained and downstream
    // consumers have finished with it. This graph never owns or frees payload
    // memory; the caller does.
    //
    // An empty predicate means "no output contract declared": completion then
    // rests on execute() returning, which is exactly the assumption RCA-195
    // punished. Those tasks are counted by uncontracted_tasks() so the escape
    // hatch is visible instead of silent.
    std::function<bool()> published;
};
struct GraphLimits { std::size_t nodes = 4096, edges = 65536; };

// One immutable graph per unique session/slot/generation. The owner must never
// reuse a generation and must keep this object and callback storage alive until
// run() returns. No asynchronous completion API is exposed.
class DecodeTaskGraph {
public:
    DecodeTaskGraph(Generation generation, GraphProfile profile,
                    std::vector<TaskSpec> tasks, GraphLimits limits = {});
    ~DecodeTaskGraph() = default;
    DecodeTaskGraph(const DecodeTaskGraph&) = delete;
    DecodeTaskGraph& operator=(const DecodeTaskGraph&) = delete;

    // Every structural admission check, without building a graph: profile,
    // generation identity, task and edge counts, duplicate keys, duplicate
    // input edges, unknown TaskKey in inputs, and cycles. Returns
    // GraphError::InvalidGraph for any violation and GraphError::None when the
    // topology is admissible. The constructor calls this first and throws
    // before any node state exists, so a rejected topology can never start a
    // worker thread or run an execute() callback. Public because a caller that
    // must not use exceptions can pre-flight a candidate graph.
    static GraphError validate(const std::vector<TaskSpec>& tasks, GraphProfile profile,
                               Generation generation, GraphLimits limits = {});

    bool run(std::size_t workers); // exactly once; drains every started worker
    bool cancel(Generation generation);
    GraphError error() const;
    bool quiescent() const;
    bool cancellation_requested() const;
    TaskState state(std::size_t node) const;
#ifdef HYPERCINE_DECODE_TASK_GRAPH_TEST_HOOK
    // Fault-injection seam for tests/decode_task_graph only. `seam` runs inside
    // publish() after the payload record is written and immediately before the
    // release-store of the version -- the exact point the ordering claim is
    // about, which is otherwise unobservable because both stores are adjacent.
    // Compiled out unless the test CMakeLists defines
    // HYPERCINE_DECODE_TASK_GRAPH_TEST_HOOK; null by default, so it cannot
    // change behaviour. It must not call back into this graph.
    static void set_publish_seam(void (*seam)());
#endif
    // Tasks that declared no publication predicate. Immutable after
    // construction, so it is readable at any time without the graph lock.
    std::size_t uncontracted_tasks() const noexcept;
    // Number of Pending -> Ready transitions since construction, including the
    // initial roots. A generation that failed its first error must not gain a
    // single one afterwards; this counter makes that observable instead of
    // inferring it from state snapshots.
    std::size_t ready_transitions() const;
private:
    friend struct DecodeTaskGraphTestAccess;
    friend class TaskContext;
    struct Node {
        TaskSpec task;
        std::vector<std::size_t> successors;
        // Producer index for each declared input, in the declared order.
        std::vector<std::size_t> sources;
        std::size_t remaining = 0;
        TaskState state = TaskState::Pending;
        // Publication record (T2). The producer writes the payload record and
        // then release-stores `version`; a consumer acquire-loads `version` and
        // only then reads payload/payload_bytes. Nothing else orders them --
        // there is no flag word shared with any other node.
        std::atomic<std::uint64_t> version{0};
        std::atomic<const void*> payload{nullptr};
        std::atomic<std::size_t> payload_bytes{0};
    };
    bool publish(std::size_t node, const void* data, std::size_t bytes) noexcept;
    void wait_for_input(std::size_t node, std::size_t input,
                        std::chrono::milliseconds timeout) const;
    BufferVersion input(std::size_t node, std::size_t input) const;
    bool input_published(std::size_t node, std::size_t input) const;
    void worker() noexcept;
    bool complete(Generation generation, std::size_t node, GraphError error);
    void fail_locked(GraphError error, std::size_t cause);
    // Both are mutable: wait_for_input() is a const query that parks on the
    // condition variable instead of spinning on the version word.
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    const Generation generation_;
    // std::deque: Node holds the publication atomics and is therefore neither
    // copyable nor movable; a deque grows without relocating existing nodes.
    std::deque<Node> nodes_;
    std::vector<std::size_t> ready_; // reserved before any callback runs
    std::size_t next_ = 0, running_ = 0, completed_ = 0, ready_transitions_ = 0;
    std::size_t uncontracted_ = 0;
    GraphError error_ = GraphError::None;
    bool started_ = false, drained_ = true, stop_ = false;
};
} // namespace hypercine::detail
