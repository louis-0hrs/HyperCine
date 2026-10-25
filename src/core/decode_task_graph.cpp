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
#ifdef HYPERCINE_DECODE_TASK_GRAPH_TEST_HOOK
std::atomic<void (*)()> publish_seam{nullptr};
#endif
}

// Admission control (T1). Runs to completion before any node state exists and
// therefore before any worker thread can be created. Every violation is
// InvalidGraph: unknown producer, duplicate key, duplicate edge and cycle are
// all "this topology cannot be scheduled", not four different failures.
GraphError DecodeTaskGraph::validate(const std::vector<TaskSpec>& tasks, GraphProfile profile,
                                    Generation generation, GraphLimits limits) {
    // Absolute ceilings also bound validation's quadratic key lookup.
    if (profile != GraphProfile::Synthetic || !generation.session || !generation.value ||
        tasks.empty() || tasks.size() > limits.nodes || tasks.size() > 4096 ||
        limits.edges > 65536) return GraphError::InvalidGraph;
    std::size_t edges = 0;
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        if (!tasks[i].execute || tasks[i].inputs.size() > limits.edges - edges)
            return GraphError::InvalidGraph;
        edges += tasks[i].inputs.size();
        for (std::size_t j = 0; j < i; ++j)
            if (tasks[i].key == tasks[j].key) return GraphError::InvalidGraph;
    }
    // Input edges: each must name an existing producer, exactly once. A cycle
    // is only reachable through these edges, so it is checked on the same
    // structure the workers will later use -- not on a copy.
    std::vector<std::size_t> remaining(tasks.size(), 0);
    std::vector<std::vector<std::size_t>> successors(tasks.size());
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        const auto& inputs = tasks[i].inputs;
        remaining[i] = inputs.size();
        for (std::size_t k = 0; k < inputs.size(); ++k) {
            for (std::size_t j = 0; j < k; ++j)
                if (inputs[j] == inputs[k]) return GraphError::InvalidGraph;
            const auto producer = std::find_if(tasks.begin(), tasks.end(),
                [&](const TaskSpec& t) { return t.key == inputs[k]; });
            if (producer == tasks.end()) return GraphError::InvalidGraph;
            successors[static_cast<std::size_t>(producer - tasks.begin())].push_back(i);
        }
    }
    // Kahn: a node that never reaches zero predecessors is inside a cycle.
    std::vector<std::size_t> counts = remaining;
    std::vector<std::size_t> queue;
    queue.reserve(tasks.size());
    for (std::size_t i = 0; i < tasks.size(); ++i) if (!counts[i]) queue.push_back(i);
    for (std::size_t i = 0; i < queue.size(); ++i)
        for (const auto successor : successors[queue[i]])
            if (--counts[successor] == 0) queue.push_back(successor);
    if (queue.size() != tasks.size()) return GraphError::InvalidGraph;
    return GraphError::None;
}

DecodeTaskGraph::DecodeTaskGraph(Generation generation, GraphProfile profile,
                               std::vector<TaskSpec> tasks, GraphLimits limits)
    : generation_(generation) {
    // Throws before nodes_ is populated: a rejected topology owns no state and
    // starts no thread, so no callback can have run.
    if (validate(tasks, profile, generation, limits) != GraphError::None)
        throw std::invalid_argument("Invalid task graph");
    ready_.reserve(tasks.size());
    // Node owns atomics, so it is neither copyable nor movable: it is built in
    // place from the caller's specs and never relocated (see the deque member).
    for (auto& task : tasks) {
        nodes_.emplace_back();
        nodes_.back().task = std::move(task);
    }
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const auto& inputs = nodes_[i].task.inputs;
        nodes_[i].remaining = inputs.size();
        nodes_[i].sources.reserve(inputs.size());
        for (std::size_t k = 0; k < inputs.size(); ++k) {
            const auto producer = std::find_if(nodes_.begin(), nodes_.end(),
                [&](const Node& n) { return n.task.key == inputs[k]; });
            // validate() already proved this; the check stays fail-closed
            // because a silent no_node here would schedule an unreadable node.
            if (producer == nodes_.end()) throw std::invalid_argument("Invalid task graph");
            producer->successors.push_back(i);
            nodes_[i].sources.push_back(static_cast<std::size_t>(producer - nodes_.begin()));
        }
    }
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (!nodes_[i].task.published) ++uncontracted_;
        if (!nodes_[i].remaining) {
            nodes_[i].state = TaskState::Ready;
            ++ready_transitions_;
            ready_.push_back(i);
        }
    }
}
std::size_t DecodeTaskGraph::uncontracted_tasks() const noexcept { return uncontracted_; }
#ifdef HYPERCINE_DECODE_TASK_GRAPH_TEST_HOOK
void DecodeTaskGraph::set_publish_seam(void (*seam)()) {
    publish_seam.store(seam, std::memory_order_relaxed);
}
#endif
std::size_t DecodeTaskGraph::ready_transitions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_transitions_;
}
bool TaskContext::cancellation_requested() const { return graph_.cancellation_requested(); }
bool TaskContext::publish(const void* data, std::size_t bytes) const noexcept {
    return graph_.publish(node_, data, bytes);
}
void TaskContext::wait_for_input(std::size_t index, std::chrono::milliseconds timeout) const {
    graph_.wait_for_input(node_, index, timeout);
}
bool TaskContext::input_published(std::size_t index) const {
    return graph_.input_published(node_, index);
}
BufferVersion TaskContext::input(std::size_t index) const { return graph_.input(node_, index); }
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
// Publication (T2). The payload record is written first; only the final
// release-store makes it observable, and the store is made under the graph
// mutex so the Ready transition of a successor is ordered with it. One publish
// per task and generation: a second call is refused rather than silently
// replacing bytes a consumer may already be reading.
bool DecodeTaskGraph::publish(std::size_t node, const void* data, std::size_t bytes) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (node >= nodes_.size()) return false;
    Node& target = nodes_[node];
    if (target.state != TaskState::Running || stop_ || error_ != GraphError::None) return false;
    if (target.version.load(std::memory_order_relaxed) != 0) return false;
    target.payload.store(data, std::memory_order_relaxed);
    target.payload_bytes.store(bytes, std::memory_order_relaxed);
#ifdef HYPERCINE_DECODE_TASK_GRAPH_TEST_HOOK
    // Test seam: sits between the payload record and its release. A build
    // without the macro has no seam, no branch and no extra store.
    if (auto seam = publish_seam.load(std::memory_order_relaxed)) seam();
#endif
    // Release. Everything above is visible to a thread that acquire-loads this
    // version. Nothing before this line is readable by a consumer.
    target.version.store(1, std::memory_order_release);
    changed_.notify_all();
    return true;
}
bool DecodeTaskGraph::input_published(std::size_t node, std::size_t input) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (node >= nodes_.size() || input >= nodes_[node].sources.size()) return false;
    return nodes_[nodes_[node].sources[input]].version.load(std::memory_order_acquire) != 0;
}
BufferVersion DecodeTaskGraph::input(std::size_t node, std::size_t input) const {
    std::lock_guard<std::mutex> lock(mutex_);
    BufferVersion view;
    if (node >= nodes_.size() || input >= nodes_[node].sources.size()) return view;
    const Node& source = nodes_[nodes_[node].sources[input]];
    view.generation = generation_;
    // Acquire pairs with the release-store in publish(). The payload fields are
    // read only when that store was observed; otherwise the view stays empty.
    view.version = source.version.load(std::memory_order_acquire);
    if (view.version != 0) {
        view.data = source.payload.load(std::memory_order_relaxed);
        view.bytes = source.payload_bytes.load(std::memory_order_relaxed);
    }
    return view;
}
// Bounded consumer wait (T2/T4). One mutex, one condition variable, no spin and
// no polling loop over the version word: every unproductive iteration is parked
// in wait_until. A deadline, a cancel or an earlier error is fail-closed -- the
// caller gets PublicationTimeout and has read no payload byte.
void DecodeTaskGraph::wait_for_input(std::size_t node, std::size_t input,
                                     std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    if (node >= nodes_.size() || input >= nodes_[node].sources.size())
        throw std::out_of_range("no such input");
    const Node& source = nodes_[nodes_[node].sources[input]];
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (source.version.load(std::memory_order_acquire) != 0) return;
        if (stop_ || error_ != GraphError::None) throw PublicationTimeout();
        if (std::chrono::steady_clock::now() >= deadline) throw PublicationTimeout();
        // Releases the mutex; publish()/fail_locked() can then take it.
        changed_.wait_until(lock, deadline);
    }
}
void DecodeTaskGraph::fail_locked(GraphError error, std::size_t cause) {
    // Write-once (First-Error): the slot moves out of None exactly once, and no
    // later writer can replace it with another code or with None.
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
                ++ready_transitions_;
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
        // The callback runs with no graph lock held. execute() has RETURNED by the
        // time the publication predicate runs, so the callback frame itself is
        // not live -- only storage the task published into independently owned
        // memory is. The predicate may call the const query API; it must not
        // mutate graph state, and it must not read a callback local.
        try { nodes_[index].task.execute(TaskContext(*this, index)); }
        catch (const std::bad_alloc&) { result = GraphError::Allocation; }
        catch (const PublicationTimeout&) { result = GraphError::Timeout; }
        catch (...) { result = GraphError::Callback; }
        if (result == GraphError::None) {
            const auto& produced = nodes_[index].task.published;
            if (produced) {
                bool exists = false;
                // A predicate that cannot answer is not evidence of an output.
                // Both a false result and a throw fail closed the same way.
                try { exists = produced(); } catch (...) { exists = false; }
                if (!exists) result = GraphError::OutputNotPublished;
            }
        }
        complete(generation_, index, result);
    }
}
bool DecodeTaskGraph::run(std::size_t workers) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_ || !workers || workers > 16) return false;
        started_ = true;
        drained_ = false;
        // Nothing runnable and nothing running means the topology cannot make
        // progress. Reject it here, before the first thread exists, instead of
        // letting a worker discover it.
        if (ready_.empty()) {
            fail_locked(GraphError::InvalidGraph, no_node);
            drained_ = true;
            return false;
        }
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
