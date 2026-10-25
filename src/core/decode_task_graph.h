// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once

#include <condition_variable>
#include <cstdint>
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
enum class GraphError {
    None, InvalidGraph, Allocation, Callback, Canceled, Worker, OutputNotPublished
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
class DecodeTaskGraph;
class TaskContext {
public:
    bool cancellation_requested() const;
private:
    friend class DecodeTaskGraph;
    explicit TaskContext(DecodeTaskGraph& graph) : graph_(graph) {}
    DecodeTaskGraph& graph_;
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
    bool run(std::size_t workers); // exactly once; drains every started worker
    bool cancel(Generation generation);
    GraphError error() const;
    bool quiescent() const;
    bool cancellation_requested() const;
    TaskState state(std::size_t node) const;
    // Tasks that declared no publication predicate. Immutable after
    // construction, so it is readable at any time without the graph lock.
    std::size_t uncontracted_tasks() const noexcept;
private:
    friend struct DecodeTaskGraphTestAccess;
    struct Node {
        TaskSpec task;
        std::vector<std::size_t> successors;
        std::size_t remaining = 0;
        TaskState state = TaskState::Pending;
    };
    void worker() noexcept;
    bool complete(Generation generation, std::size_t node, GraphError error);
    void fail_locked(GraphError error, std::size_t cause);
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    const Generation generation_;
    std::vector<Node> nodes_;
    std::vector<std::size_t> ready_; // reserved before any callback runs
    std::size_t next_ = 0, running_ = 0, completed_ = 0;
    std::size_t uncontracted_ = 0;
    GraphError error_ = GraphError::None;
    bool started_ = false, drained_ = true, stop_ = false;
};
} // namespace hypercine::detail
