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
enum class GraphError { None, InvalidGraph, Allocation, Callback, Canceled, Worker };
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
    GraphError error_ = GraphError::None;
    bool started_ = false, drained_ = true, stop_ = false;
};
} // namespace hypercine::detail
