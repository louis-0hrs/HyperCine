// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "decode_task_graph.h"
#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>

namespace hypercine::detail {
namespace {
constexpr std::size_t no_node = std::numeric_limits<std::size_t>::max();
}
DecodeTaskGraph::DecodeTaskGraph(Generation generation, GraphProfile profile,
                               std::vector<TaskSpec> tasks, GraphLimits limits)
    : generation_(generation) {
    const auto invalid = [] { throw std::invalid_argument("Invalid task graph"); };
    // Absolute ceilings also bound validation's quadratic key lookup.
    if (profile != GraphProfile::Synthetic || !generation.session || !generation.value ||
        tasks.empty() || tasks.size() > limits.nodes || tasks.size() > 4096 ||
        limits.edges > 65536) invalid();
    std::size_t edges = 0;
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        if (!tasks[i].execute || tasks[i].inputs.size() > limits.edges - edges) invalid();
        edges += tasks[i].inputs.size();
        for (std::size_t j = 0; j < i; ++j)
            if (tasks[i].key == tasks[j].key) invalid();
    }
    nodes_.reserve(tasks.size());
    ready_.reserve(tasks.size());
    for (auto& task : tasks) nodes_.push_back({std::move(task), {}, 0, TaskState::Pending});
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const auto& inputs = nodes_[i].task.inputs;
        nodes_[i].remaining = inputs.size();
        for (std::size_t k = 0; k < inputs.size(); ++k) {
            for (std::size_t j = 0; j < k; ++j) if (inputs[j] == inputs[k]) invalid();
            const auto producer = std::find_if(nodes_.begin(), nodes_.end(),
                [&](const Node& n) { return n.task.key == inputs[k]; });
            if (producer == nodes_.end()) invalid();
            producer->successors.push_back(i);
        }
    }
    // Kahn validation runs before state publication or worker creation.
    std::vector<std::size_t> counts;
    counts.reserve(nodes_.size());
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        counts.push_back(nodes_[i].remaining);
        if (!counts.back()) ready_.push_back(i);
    }
    for (std::size_t i = 0; i < ready_.size(); ++i)
        for (const auto successor : nodes_[ready_[i]].successors)
            if (--counts[successor] == 0) ready_.push_back(successor);
    if (ready_.size() != nodes_.size()) invalid();
    ready_.clear();
    for (std::size_t i = 0; i < nodes_.size(); ++i)
        if (!nodes_[i].remaining) {
            nodes_[i].state = TaskState::Ready;
            ready_.push_back(i);
        }
}
bool TaskContext::cancellation_requested() const { return graph_.cancellation_requested(); }
bool DecodeTaskGraph::cancellation_requested() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stop_;
}
GraphError DecodeTaskGraph::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}
bool DecodeTaskGraph::quiescent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return drained_;
}
TaskState DecodeTaskGraph::state(std::size_t node) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return nodes_.at(node).state;
}
void DecodeTaskGraph::fail_locked(GraphError error, std::size_t cause) {
    if (error_ == GraphError::None) error_ = error;
    stop_ = true;
    // Mark the failing node's unstarted transitive dependents Failed, and all
    // other queued jobs Canceled. No allocation is needed in the failure path.
    // Scheduling has stopped; reuse the reserved queue as a failure traversal.
    ready_.clear();
    if (cause < nodes_.size()) {
        nodes_[cause].state = TaskState::Failed;
        ready_.push_back(cause);
    }
    for (std::size_t i = 0; i < ready_.size(); ++i)
        for (const auto child : nodes_[ready_[i]].successors)
            if (nodes_[child].state == TaskState::Pending || nodes_[child].state == TaskState::Ready) {
                nodes_[child].state = TaskState::Failed;
                ready_.push_back(child);
            }
    for (auto& n : nodes_)
        if (n.state == TaskState::Pending || n.state == TaskState::Ready) n.state = TaskState::Canceled;
    changed_.notify_all();
}
bool DecodeTaskGraph::cancel(Generation generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!(generation == generation_) || completed_ == nodes_.size() || stop_) return false;
    fail_locked(GraphError::Canceled, no_node);
    return true;
}
bool DecodeTaskGraph::complete(Generation generation, std::size_t index, GraphError error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!(generation == generation_) || index >= nodes_.size() ||
        nodes_[index].state != TaskState::Running) return false;
    --running_;
    if (error != GraphError::None) {
        fail_locked(error, index);
    } else if (stop_) {
        nodes_[index].state = TaskState::Canceled;
    } else {
        nodes_[index].state = TaskState::Completed;
        ++completed_;
        for (const auto child : nodes_[index].successors) {
            auto& n = nodes_[child];
            if (!n.remaining || n.state != TaskState::Pending) {
                fail_locked(GraphError::InvalidGraph, child);
                break;
            }
            if (--n.remaining == 0) {
                n.state = TaskState::Ready;
                ready_.push_back(child);
            }
        }
    }
    changed_.notify_all();
    return !stop_;
}
void DecodeTaskGraph::worker() noexcept {
    for (;;) {
        std::size_t index;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [&] { return stop_ || next_ < ready_.size() || running_ == 0; });
            if (stop_ || completed_ == nodes_.size()) return;
            if (next_ == ready_.size()) {
                fail_locked(GraphError::InvalidGraph, no_node);
                return;
            }
            index = ready_[next_++];
            nodes_[index].state = TaskState::Running;
            ++running_;
        }
        GraphError result = GraphError::None;
        try { nodes_[index].task.execute(TaskContext(*this)); }
        catch (const std::bad_alloc&) { result = GraphError::Allocation; }
        catch (...) { result = GraphError::Callback; }
        complete(generation_, index, result);
    }
}
bool DecodeTaskGraph::run(std::size_t workers) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_ || !workers || workers > 16) return false;
        started_ = true;
        drained_ = false;
    }
    std::vector<std::thread> threads;
    try {
        threads.reserve(workers);
        for (std::size_t i = 0; i < workers; ++i) threads.emplace_back([this] { worker(); });
    } catch (const std::bad_alloc&) {
        std::lock_guard<std::mutex> lock(mutex_);
        fail_locked(GraphError::Allocation, no_node);
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        fail_locked(GraphError::Worker, no_node);
    }
    for (auto& thread : threads) thread.join();
    std::lock_guard<std::mutex> lock(mutex_);
    drained_ = true;
    return error_ == GraphError::None && completed_ == nodes_.size();
}
} // namespace hypercine::detail
