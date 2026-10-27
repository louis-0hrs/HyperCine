// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "decode_task_graph.h"
#include "decode_s3_plane.h"
#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>

namespace hypercine::detail {
namespace {
constexpr std::size_t no_node = std::numeric_limits<std::size_t>::max();
static_assert(kOwnedBufferAlign == kS3PlaneAlign,
              "graph-owned copies must satisfy the signed-plane alignment");
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
    : generation_(generation), require_published_output_(limits.require_published_output),
      generation_budget_(limits.generation_budget_bytes < kPlaneGenerationBudget
                             ? limits.generation_budget_bytes
                             : kPlaneGenerationBudget) {
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
std::size_t DecodeTaskGraph::owned_publication_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return owned_budget_;
}
bool TaskContext::cancellation_requested() const { return graph_.cancellation_requested(); }
bool TaskContext::publish(const void* data, std::size_t bytes) const noexcept {
    return graph_.publish(node_, data, bytes);
}
bool TaskContext::publish_plane(const void* data, std::size_t bytes, std::uint32_t width,
                               std::uint32_t height, std::size_t pitch) const noexcept {
    return graph_.publish_plane(node_, data, bytes, width, height, pitch);
}
PlaneView TaskContext::input_plane(std::size_t index, const PlaneIdentity& expected) const {
    return graph_.input_plane(node_, index, expected);
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
// Publication (T2 + S2). The caller's bytes are copied into the node first.
// Only then is the payload record pointed at that copy, and only the final
// release-store makes the version observable. The store is made under the graph
// mutex. One publish per task and generation: a second call is refused rather
// than replacing bytes a consumer may already be reading.
//
// The copy is the lifetime. After this returns, the caller may free `data`.
// Every dependent reads `owned_bytes`, which is not resized again and which
// dies with the graph, after run() has joined the workers.
bool DecodeTaskGraph::publish(std::size_t node, const void* data, std::size_t bytes) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (node >= nodes_.size()) return false;
    Node& target = nodes_[node];
    if (target.state != TaskState::Running || stop_ || error_ != GraphError::None) return false;
    if (target.version.load(std::memory_order_relaxed) != 0) return false;
    // Null / zero-length rejection, before any store. A refusal must not publish
    // a valid-looking version, change TaskState, notify a waiter, or move
    // ready_transitions(). The version word is the publication counter and stays
    // 0, so a later valid publish of this task is still admitted.
    //   nullptr && bytes > 0 : unreadable length, not a payload.
    //   bytes == 0            : not a decoder output, with or without an address
    //                           (non-null zero-length, and nullptr with bytes 0).
    if (data == nullptr || bytes == 0) return false;
    return commit_copy_locked(target, data, bytes);
}
// Shared tail of publish() and publish_plane(). mutex_ is held. The copy is
// installed and the payload record written before the release-store. A failed
// allocation leaves version at 0 and does not notify.
bool DecodeTaskGraph::commit_copy_locked(Node& target, const void* data,
                                        std::size_t bytes) noexcept {
    // Guard (BUDGET-ALL): the ceiling covers every owned publication, typed or not.
    // It is charged here rather than in publish_plane() so that no future caller can
    // reach the copy without paying for it -- an uncharged untyped publish() was
    // exactly the hole this closes. A refusal happens before the copy, so a rejected
    // publication retains nothing.
    if (bytes > generation_budget_ || owned_budget_ > generation_budget_ - bytes) return false;
    const auto* source = static_cast<const unsigned char*>(data);
    OwnedBytes copy;
    try {
        copy.assign(source, source + bytes);
    } catch (...) {
        // noexcept: a failed copy is not a publication. The version stays 0.
        return false;
    }
    target.owned_bytes.swap(copy);
    owned_budget_ += target.owned_bytes.size();
    target.published_generation = generation_;
    target.payload.store(target.owned_bytes.data(), std::memory_order_relaxed);
    target.payload_bytes.store(target.owned_bytes.size(), std::memory_order_relaxed);
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
// Plane publication (T4). Extent, key and budget are decided before the copy.
// A rejection does not store a version, does not notify and does not charge
// the generation. Geometry is written before commit_copy_locked()'s release
// so a consumer that observes the version also observes the extent.
bool DecodeTaskGraph::publish_plane(std::size_t node, const void* data, std::size_t bytes,
                                   std::uint32_t width, std::uint32_t height,
                                   std::size_t pitch) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (node >= nodes_.size()) return false;
    Node& target = nodes_[node];
    if (target.state != TaskState::Running || stop_ || error_ != GraphError::None) return false;
    if (target.version.load(std::memory_order_relaxed) != 0) return false;
    if (data == nullptr || bytes == 0 || width == 0 || height == 0 || pitch == 0) return false;
    const TaskKey& key = target.task.key;
    // Guard (T4-KEY): only Y/V/U, wavelet5 bands 0..3, or the wavelet4 lowpass
    // (level 4, band 0). A control record or a foreign channel is not a plane.
    if (key.channel > 2 || key.band > 3 || key.transform != 0 ||
        (key.level != 4 && key.level != 5) || (key.level == 4 && key.band != 0))
        return false;
    SignedPlaneDesc claimed;
    claimed.data = static_cast<const std::int16_t*>(data);
    claimed.width = width;
    claimed.height = height;
    claimed.pitch = pitch;
    claimed.bytes = bytes;
    // Guard (T4-EXTENT): alignment, geometry, stride and the pitched byte count
    // are one decision. A short span does not become a version.
    if (check_signed_plane(claimed) != PlaneStatus::Ok) return false;
    // The budget is decided and charged inside commit_copy_locked(), which every
    // publication path goes through. Checked here only as a fast refusal so the
    // geometry fields are not written for a copy that cannot be paid for.
    if (bytes > generation_budget_ || owned_budget_ > generation_budget_ - bytes) return false;
    target.plane_width = width;
    target.plane_height = height;
    target.plane_pitch = pitch;
    if (!commit_copy_locked(target, data, bytes)) {
        target.plane_width = 0;
        target.plane_height = 0;
        target.plane_pitch = 0;
        return false;
    }
    return true;
}
PlaneView DecodeTaskGraph::input_plane(std::size_t node, std::size_t input,
                                      const PlaneIdentity& expected) const {
    std::lock_guard<std::mutex> lock(mutex_);
    PlaneView empty;
    if (node >= nodes_.size() || input >= nodes_[node].sources.size()) return empty;
    // Guard (T4-STALE): a read named for another generation must not observe
    // this generation's bytes, even when the address would otherwise be valid.
    if (!(expected.generation == generation_)) return empty;
    if (stop_ || error_ != GraphError::None) return empty;
    const Node& source = nodes_[nodes_[node].sources[input]];
    // Guard (T4-IDENTITY): channel, wavelet, band, frame and transform are the
    // producer key of this edge. Another band's plane does not satisfy it.
    if (source.task.key.channel != expected.channel || source.task.key.band != expected.band ||
        source.task.key.level != expected.wavelet || source.task.key.transform != expected.transform ||
        source.task.key.frame != expected.frame)
        return empty;
    const BufferVersion view = admitted_input(source);
    if (!view.valid()) return empty;
    // Guard (T4-GEOMETRY): the stored extent must be the one this edge named.
    // A readable span of a different width is a different plane, not a view
    // the caller may index at the size it hoped for.
    if (source.plane_width != expected.width || source.plane_height != expected.height ||
        source.plane_pitch != expected.pitch || expected.width == 0 || expected.pitch == 0)
        return empty;
    if (expected.height != 0 &&
        expected.pitch > std::numeric_limits<std::size_t>::max() / expected.height)
        return empty;
    const std::size_t need = expected.pitch * static_cast<std::size_t>(expected.height);
    if (view.bytes < need) return empty;
    PlaneView out;
    out.generation = view.generation;
    out.channel = source.task.key.channel;
    out.wavelet = source.task.key.level;
    out.band = source.task.key.band;
    out.width = source.plane_width;
    out.height = source.plane_height;
    out.pitch = source.plane_pitch;
    out.data = view.data;
    out.bytes = view.bytes;
    return out;
}
bool DecodeTaskGraph::published_output(std::size_t index) const {
    const Node& node = nodes_[index];
    const auto version = node.version.load(std::memory_order_relaxed);
    const void* data = node.payload.load(std::memory_order_relaxed);
    const auto bytes = node.payload_bytes.load(std::memory_order_relaxed);
    // Guard (no-op producer): the callback returned and publish() never stored
    // a version. Later checks are reached only when version != 0, so this line
    // is the only rejection of "nothing was published".
    if (version == 0) return false;
    // Guard (missing publication): a version word is not a result unless it is
    // a readable buffer that was published for THIS generation.
    if (version != 0 && (!(node.published_generation == generation_) || data == nullptr ||
                         bytes == 0))
        return false;
    // The bytes the version names are the graph-owned copy, not the caller's
    // address. An external pointer is not a retained result.
    if (version != 0 && (node.owned_bytes.empty() || data != node.owned_bytes.data() ||
                         bytes != node.owned_bytes.size()))
        return false;
    return true;
}
BufferVersion DecodeTaskGraph::admitted_input(const Node& source) const {
    BufferVersion view;
    // Same precedence as wait_for_input(), which throws instead of returning a
    // view. Stop or the first recorded error is terminal even when this source
    // already published a version. Returning that version here would let a
    // non-blocking caller bypass the decision the waiting gate already closes.
    if (stop_ || error_ != GraphError::None) return view;
    // Acquire pairs with the release-store in publish(). Payload fields are
    // read only after that store was observed.
    const auto version = source.version.load(std::memory_order_acquire);
    if (version == 0) return view;
    // Guard (foreign generation): a version word from another generation is not
    // an input of this graph, even when the address still looks readable.
    if (!(source.published_generation == generation_)) return view;
    const void* data = source.payload.load(std::memory_order_relaxed);
    const auto bytes = source.payload_bytes.load(std::memory_order_relaxed);
    // Guard (joint admission): generation, version, address and length are one
    // decision. A non-zero version with a null address, a zero length, or a
    // pointer that is not the graph-owned copy is not a readable buffer. Any
    // one of those missing leaves the view empty, so valid() stays false.
    if (data == nullptr || bytes == 0 || source.owned_bytes.empty() ||
        data != source.owned_bytes.data() || bytes != source.owned_bytes.size())
        return view;
    view.generation = source.published_generation;
    view.version = version;
    view.data = data;
    view.bytes = bytes;
    return view;
}
bool DecodeTaskGraph::input_published(std::size_t node, std::size_t input) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (node >= nodes_.size() || input >= nodes_[node].sources.size()) return false;
    return admitted_input(nodes_[nodes_[node].sources[input]]).valid();
}
BufferVersion DecodeTaskGraph::input(std::size_t node, std::size_t input) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (node >= nodes_.size() || input >= nodes_[node].sources.size()) return {};
    return admitted_input(nodes_[nodes_[node].sources[input]]);
}
// Bounded consumer wait (T2/T4). One mutex, one condition variable, no spin and
// no polling loop over the version word: every unproductive iteration is parked
// in wait_until. A deadline, a cancel or an earlier error is fail-closed -- the
// caller gets PublicationTimeout and has read no payload byte. Stop or a
// recorded error is checked before the version, so an already published buffer
// cannot outrank that decision.
void DecodeTaskGraph::wait_for_input(std::size_t node, std::size_t input,
                                     std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    if (node >= nodes_.size() || input >= nodes_[node].sources.size())
        throw std::out_of_range("no such input");
    const Node& source = nodes_[nodes_[node].sources[input]];
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        // Precedence: stop or a recorded error is terminal even when this source
        // already published a version. Checking the version first would admit
        // that earlier buffer and bypass the later decision. The caller has
        // read no payload byte when this throws.
        if (stop_ || error_ != GraphError::None) throw PublicationTimeout();
        if (source.version.load(std::memory_order_acquire) != 0) return;
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
        // Cancellation outranks a missing buffer: the node is Canceled, not a
        // later OutputNotPublished, and its dependents were already stopped.
        nodes_[index].state = TaskState::Canceled;
    } else if (require_published_output_ && !published_output(index)) {
        // No valid buffer for this generation. Do not Complete, and do not
        // ready any dependent: fail_locked marks them Failed, never Ready.
        // The rule is graph-wide and on by default; see GraphLimits.
        fail_locked(GraphError::OutputNotPublished, index);
    } else {
        nodes_[index].state = TaskState::Completed;
        ++completed_;
        for (const auto child : nodes_[index].successors) {
            auto& n = nodes_[child];
            if (!n.remaining || n.state != TaskState::Pending) {
                fail_locked(GraphError::InvalidGraph, child);
                break;
            }
            // Guard (removed dependency): a successor becomes Ready only when
            // EVERY declared predecessor has completed. Decrementing is not
            // enough; remaining must actually hit zero.
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
    // Guard (RELEASE-ABANDONED): a stopped generation retains nothing. Safe here and
    // only here: every worker has been joined, so no callback frame is live, and
    // fail_locked() already marked every dependent Failed, so no node whose storage
    // is released was ever handed to a consumer.
    if (error_ != GraphError::None) release_abandoned_locked();
    return error_ == GraphError::None && completed_ == nodes_.size();
}
void DecodeTaskGraph::release_abandoned_locked() noexcept {
    for (auto& node : nodes_) {
        if (node.state == TaskState::Completed) continue;
        if (node.owned_bytes.empty()) continue;
        const std::size_t released = node.owned_bytes.size();
        // Drop the payload record and the version before the bytes go away, so a
        // stale reader observes "nothing published" rather than a dangling address.
        node.payload.store(nullptr, std::memory_order_relaxed);
        node.payload_bytes.store(0, std::memory_order_relaxed);
        node.version.store(0, std::memory_order_relaxed);
        OwnedBytes empty;
        node.owned_bytes.swap(empty);
        node.plane_width = 0;
        node.plane_height = 0;
        node.plane_pitch = 0;
        owned_budget_ = owned_budget_ > released ? owned_budget_ - released : 0;
    }
}
} // namespace hypercine::detail
