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
#include <limits>
#include <mutex>
#include <new>
#include <type_traits>
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
//                   A rejected publish is included: a null address or a
//                   zero-length span never advances the version.
//   version >= 1  : data/bytes describe the payload of that version and are
//                   valid for reading. The version is only ever produced by
//                   DecodeTaskGraph::publish(), whose release-store orders the
//                   producer's payload stores before the consumer's acquire.
// A BufferVersion carries the generation it was published for; a consumer must
// compare `generation` against its own graph generation before reading.
//
// T4 plane reads do that comparison inside input_plane(). The untyped input()
// span stays the S2 byte view: it does not know channel or band. A decoder-shaped
// consumer must use input_plane(), which is empty unless generation, producer
// key and extent were admitted together.
//
// Ownership (S2). publish() copies the bytes into storage owned by this graph
// before the version becomes visible. The pointer in a valid BufferVersion
// addresses that copy, not the caller's buffer. The copy lives until the graph
// is destroyed. run() joins every worker before it returns and there is no
// asynchronous completion API, so the copy outlives every dependent callback
// this graph can schedule. A per-node copy is used instead of a refcount: the
// API has no retain/release, and dropping one successor from a count would free
// bytes another dependent still has to read. See docs/SCHEDULER_S2_OUTPUT_144.md.
//
// This is still a synthetic graph. It does not decode a GoPro/CFHD bitstream and
// it is not wired to a CUDA or Metal consumer.
//
// Precedence (fail-closed). A non-zero version is not admission. Once the graph
// has been stopped or a GraphError has been recorded, that decision outranks an
// already published version. wait_for_input(), input_published() and input()
// all apply it. The waiting gate checks stop/error first, then the version,
// then the deadline, and throws PublicationTimeout without handing the caller
// the payload. The non-blocking probes do not throw: after stop or the first
// error they report "not published" and an empty view, so a buffer published
// earlier cannot reopen an aborted generation. A non-empty view additionally
// requires generation, version, payload address and length to be jointly valid.
struct BufferVersion {
    Generation generation{};
    std::uint64_t version = 0;
    const void* data = nullptr;
    std::size_t bytes = 0;
    bool valid() const noexcept { return version != 0; }
};

// T4. The identity a consumer claims for one declared input. Every field is
// part of the admission: a stale generation, another channel, or another band
// is a different edge, not a readable subset of this plane.
struct PlaneIdentity {
    Generation generation{};
    std::uint32_t channel = 0;
    std::uint32_t wavelet = 0;
    std::uint32_t band = 0;
    std::uint32_t transform = 0;
    std::uint32_t frame = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t pitch = 0;
};

// Empty unless input_plane() admitted generation, identity, extent and the
// graph-owned bytes together. valid() is false for a stale generation, a
// foreign channel or band, a short span, or a generation that has stopped.
struct PlaneView {
    Generation generation{};
    std::uint32_t channel = 0;
    std::uint32_t wavelet = 0;
    std::uint32_t band = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t pitch = 0;
    const void* data = nullptr;
    std::size_t bytes = 0;
    bool valid() const noexcept { return data != nullptr && bytes != 0 && width != 0; }
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
    // is no longer Running -- the payload is then NOT published. On success the
    // bytes are copied into graph-owned storage; the caller may free or reuse
    // `data` as soon as publish() returns. Consumers read the copy.
    //
    // Null and zero-length are not publications (fail-closed). A rejected call
    // returns false before any store:
    //   data == nullptr && bytes > 0  -> version stays 0. A positive length with
    //       no address would otherwise look published and be unreadable.
    //   data != nullptr && bytes == 0 -> version stays 0. A zero-length span is
    //       not a decoder output and is not a meaningful empty buffer.
    //   data == nullptr && bytes == 0 -> version stays 0. Absence of a payload
    //       is not an empty successful publish.
    // The rejection leaves TaskState unchanged, does not store payload or
    // payload_bytes, does not release-store version, does not notify waiters,
    // and does not bump ready_transitions(). There is no separate published
    // counter: the version word is that counter, and it stays 0. A later valid
    // publish of the same task is still admitted.
    // const: the callback receives a const TaskContext&; publication moves the
    // graph's own record, not this handle.
    bool publish(const void* data, std::size_t bytes) const noexcept;

    // Consumer side, bounded. Waits (condition variable, no spin) until the
    // `index`-th declared input has been published, then returns. Throws
    // PublicationTimeout if the deadline expires, the graph is canceled, or an
    // error was recorded. It never returns for an unpublished edge, so a
    // caller cannot read before publication by ignoring a return value.
    //
    // Precedence, checked in this order on every wake while the graph mutex is
    // held:
    //   1. stop, or a recorded error -> PublicationTimeout, even when the
    //      source version is already non-zero. A buffer published earlier must
    //      not let the consumer bypass a later cancellation or failure.
    //   2. source version != 0       -> return. Only then may the caller read.
    //   3. deadline expired          -> PublicationTimeout.
    //   4. otherwise park on the condition variable.
    // A throw means the caller has read no payload byte.
    void wait_for_input(std::size_t index, std::chrono::milliseconds timeout) const;

    // Read-only probe of the same gate, without waiting. Returns true only when
    // the generation is still open (not stopped, no recorded error) and the
    // acquire-load observed a publish whose generation, version, address and
    // length are jointly a readable graph-owned buffer. Stop or the first error
    // makes this return false even if that version is already non-zero.
    bool input_published(std::size_t index) const;

    // const view of a published input. Empty (valid() == false, null data,
    // zero length) when nothing readable was published, and also after stop or
    // a recorded error. Same precedence as wait_for_input(): an earlier publish
    // does not reopen an aborted generation. A non-empty view means generation,
    // version, address and length were admitted together, and the address is
    // the graph-owned copy. Only meaningful when input_published() is true;
    // after wait_for_input() returns it is still re-checked, so a stop that
    // lands between the wait and this call cannot hand out the payload.
    // This span is untyped. It does not check channel, band or plane extent.
    BufferVersion input(std::size_t index) const;

    // Plane publication (T4). Copies into graph-owned storage before the version
    // release-store, like publish(). The caller may free `data` when this
    // returns. Not a publication, and does not wake a waiter or charge the
    // budget, when the signed-plane extent is illegal, the node key is not a
    // Y/V/U wavelet5 band or the wavelet4 lowpass, or the copy would exceed the
    // generation budget. Channel and band are the node key, not a caller tag.
    // A later legal publish of the same task is still admitted.
    bool publish_plane(const void* data, std::size_t bytes, std::uint32_t width,
                       std::uint32_t height, std::size_t pitch) const noexcept;

    // Typed read of one declared input. Empty unless this generation is still
    // open and `expected` names that producer exactly: generation, channel,
    // wavelet, band, frame, transform and extent. Stop, the first error, a
    // stale generation and a foreign channel or band return an empty view and
    // do not expose payload bytes. A valid pointer is the graph-owned copy.
    PlaneView input_plane(std::size_t index, const PlaneIdentity& expected) const;

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
    // not a supported pattern. The predicate may read storage that outlives the
    // callback frame. It is not itself the buffer, and completion does not trust
    // it.
    //
    // S2 ownership is the completion rule. A task becomes Completed only when
    // publish() has installed a valid BufferVersion for this graph's generation:
    // version != 0, generation match, a non-null non-zero span, and those bytes
    // are the graph-owned copy. A callback that returns without that result is
    // OutputNotPublished, whether or not `published` returned true. Dependents
    // read the owned copy, including after the caller frees its own buffer.
    // Failed publication, cancellation and timeout do not move a dependent to
    // Ready. The first recorded GraphError is not overwritten.
    //
    // Not a decoder integration. No GoPro/CFHD producer is attached here; a real
    // decoder consumer stays gated until this lifetime rule is qualified.
    //
    // An empty predicate means "no extra boolean contract". It is not permission
    // to complete without a buffer. Tasks with an empty predicate are counted by
    // uncontracted_tasks() so that omission stays visible.
    //
    // The only exception is graph-wide and explicit: GraphLimits::
    // require_published_output == false, for pools that never publish. It cannot
    // be expressed per task, so a single lax task cannot hide inside a strict graph.
    std::function<bool()> published{};
};

// Three SDK channels (Y, V, U) times four wavelet5 bands times the 2 MiB plane
// cap is 24 MiB of owned input, plus one 2 MiB lowpass. Synthetic ceiling for
// one generation, not a frame cache and not a licence to overlap generations.
inline constexpr std::size_t kPlaneGenerationBudget =
    (24u * 1024u * 1024u) + (2u * 1024u * 1024u);

// Graph-owned copies are read by later SIMD consumers that require 32-byte
// data alignment (kS3PlaneAlign). std::vector<unsigned char> does not promise
// that. This allocator is the publication buffer's only alignment contract.
inline constexpr std::size_t kOwnedBufferAlign = 32;

template <class T>
struct OwnedBufferAllocator {
    using value_type = T;
    using is_always_equal = std::true_type;
    OwnedBufferAllocator() noexcept = default;
    template <class U>
    OwnedBufferAllocator(const OwnedBufferAllocator<U>&) noexcept {}
    template <class U>
    struct rebind {
        using other = OwnedBufferAllocator<U>;
    };
    T* allocate(std::size_t n) {
        if (n == 0 || n > std::numeric_limits<std::size_t>::max() / sizeof(T))
            throw std::bad_alloc();
        return static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t{kOwnedBufferAlign}));
    }
    void deallocate(T* p, std::size_t) noexcept {
        ::operator delete(p, std::align_val_t{kOwnedBufferAlign});
    }
};
template <class T, class U>
bool operator==(const OwnedBufferAllocator<T>&, const OwnedBufferAllocator<U>&) noexcept {
    return true;
}
template <class T, class U>
bool operator!=(const OwnedBufferAllocator<T>&, const OwnedBufferAllocator<U>&) noexcept {
    return false;
}
using OwnedBytes = std::vector<unsigned char, OwnedBufferAllocator<unsigned char>>;


struct GraphLimits {
    std::size_t nodes = 4096, edges = 65536;
    // S2 completion rule. Default ON: a task becomes Completed only with a valid
    // published BufferVersion for this generation. This is the decoder-facing
    // contract and it stays the default so that omitting it is never silently lax.
    //
    // Setting it false is an explicit, per-graph opt-out for callers that use the
    // graph purely as a worker pool: their callbacks write into storage the caller
    // already owns and never call publish(). Such a graph then reports
    // uncontracted_tasks() == nodes, so the omission stays observable. It is NOT a
    // way to make a decoder graph lax -- a graph that publishes keeps the rule.
    bool require_published_output = true;
    // T4. Ceiling for graph-owned publication, typed and untyped alike. The
    // absolute maximum is kPlaneGenerationBudget; a smaller value is a tighter
    // cap; a larger value is clamped down to it; zero admits no owned publication
    // of any kind. The decision and the charge both live in
    // commit_copy_locked(), which every publication path calls, so a plain
    // publish() pays exactly like publish_plane(). This comment previously said
    // untyped publish() did not spend the budget -- true before BUDGET-ALL, false
    // after it, and the kind of drift that lets a reader certify a closed hole.
    // Appended last so existing positional GraphLimits braces keep their meaning.
    std::size_t generation_budget_bytes = kPlaneGenerationBudget;
};

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
    bool requires_published_output() const noexcept { return require_published_output_; }
    // Bytes retained by owned publications, typed and untyped alike -- the ceiling
    // covers both. A rejection adds nothing. A stopped generation releases every
    // non-completed node's storage when run() returns, so this drops to the
    // completed nodes' total. The graph destructor releases whatever remains, and
    // that is only safe after run() has joined every worker.
    std::size_t owned_publication_bytes() const;
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
        // Publication record (T2 + S2). The producer copies into `owned_bytes`,
        // writes the payload record (which addresses that copy), and then
        // release-stores `version`. A consumer acquire-loads `version` and only
        // then reads payload/payload_bytes. `owned_bytes` is not resized after
        // that copy, and Node is never relocated, so the address stays valid
        // until the graph is destroyed.
        std::atomic<std::uint64_t> version{0};
        std::atomic<const void*> payload{nullptr};
        std::atomic<std::size_t> payload_bytes{0};
        Generation published_generation{};
        // 32-byte aligned. A default std::vector<unsigned char> is not.
        OwnedBytes owned_bytes;
        // Set before the version release, only by publish_plane(). Zero on an
        // untyped publish(), so input_plane() cannot treat a token as a plane.
        std::uint32_t plane_width = 0;
        std::uint32_t plane_height = 0;
        std::size_t plane_pitch = 0;
    };
    bool publish(std::size_t node, const void* data, std::size_t bytes) noexcept;
    bool publish_plane(std::size_t node, const void* data, std::size_t bytes,
                       std::uint32_t width, std::uint32_t height, std::size_t pitch) noexcept;
    // mutex_ must be held. Writes the owned copy, charges the budget and
    // release-stores version. It performs the budget decision itself so that no
    // publication path can reach the copy without paying for it; extent and
    // geometry remain the caller's decision.
    bool commit_copy_locked(Node& target, const void* data, std::size_t bytes) noexcept;
    // mutex_ must be held, and every worker must already be joined. Drops owned
    // storage for nodes that did not complete, after clearing their payload record
    // and version, and refunds the budget. Only valid once scheduling has stopped:
    // a stopped generation has released no dependent (fail_locked marks them
    // Failed), so nothing it frees was ever handed to a consumer.
    void release_abandoned_locked() noexcept;
    PlaneView input_plane(std::size_t node, std::size_t input, const PlaneIdentity& expected) const;
    // True only for a readable owned BufferVersion of this generation.
    // version == 0 is rejected on its own. A non-zero version that is not a
    // buffer of this generation is a different rejection, so disabling one
    // does not silently keep the other.
    bool published_output(std::size_t index) const;
    void wait_for_input(std::size_t node, std::size_t input,
                        std::chrono::milliseconds timeout) const;
    BufferVersion input(std::size_t node, std::size_t input) const;
    bool input_published(std::size_t node, std::size_t input) const;
    // mutex_ must be held. Empty unless the generation is open and `source`
    // holds one jointly valid owned buffer of this generation.
    BufferVersion admitted_input(const Node& source) const;
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
    // Graph-wide S2 completion rule, captured at construction. Default true.
    bool require_published_output_ = true;
    // Effective T4 ceiling, clamped in the constructor. Spent by
    // commit_copy_locked(), under mutex_, for both publication paths -- not by
    // publish_plane() alone. Refunded by release_abandoned_locked() when a
    // stopped generation drops storage it will never hand to a consumer.
    std::size_t generation_budget_ = kPlaneGenerationBudget;
    std::size_t owned_budget_ = 0;
    GraphError error_ = GraphError::None;
    bool started_ = false, drained_ = true, stop_ = false;
};
} // namespace hypercine::detail
