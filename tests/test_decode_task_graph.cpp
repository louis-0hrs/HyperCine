// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "decode_task_graph.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>

// Real allocation failures, restricted to the controlling thread. Workers use
// their normal allocator; callback bad_alloc is tested separately below.
thread_local long allocation_budget = -1;
void* operator new(std::size_t size) {
    if (allocation_budget == 0) throw std::bad_alloc();
    if (allocation_budget > 0) --allocation_budget;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace hypercine::detail {
// Only the synchronous worker owns completion in production. Test the private
// publication guard directly to cover stale, duplicate and impossible messages.
struct DecodeTaskGraphTestAccess {
    static bool complete(DecodeTaskGraph& g, Generation id, std::size_t node) {
        return g.complete(id, node, GraphError::None);
    }
    static void corrupt_count(DecodeTaskGraph& g, std::size_t node) {
        std::lock_guard<std::mutex> lock(g.mutex_);
        g.nodes_.at(node).remaining = 0;
    }
};
}
using namespace hypercine::detail;
namespace {
const Generation generation{1, 2, 1};
TaskKey key(std::uint32_t band, std::uint32_t channel = 0) { return {channel, 0, 0, 0, band}; }
void check(bool value) { if (!value) throw std::runtime_error("Contract assertion failed"); }
TaskSpec task(std::uint32_t band, std::vector<TaskKey> inputs = {},
              std::function<void(const TaskContext&)> fn = [](const TaskContext&) {}) {
    // An empty callback stays empty so construction still rejects it. A real
    // callback publishes one owned byte after its own work; a throw publishes nothing.
    if (!fn) return {key(band), std::move(inputs), {}};
    return {key(band), std::move(inputs), [fn = std::move(fn)](const TaskContext& context) {
        fn(context);
        static constexpr unsigned char token = 1;
        (void)context.publish(&token, 1);
    }};
}
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false;
    void hold() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        if (!changed.wait_for(lock, std::chrono::seconds(3), [&] { return released; }))
            throw std::runtime_error("Producer gate timed out");
    }
    bool await_entry() {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(3), [&] { return entered; });
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        changed.notify_all();
    }
};
struct Runner {
    DecodeTaskGraph& graph;
    bool result = false;
    std::thread thread;
    Runner(DecodeTaskGraph& g, std::size_t workers) : graph(g), thread([&, workers] { result = g.run(workers); }) {}
    ~Runner() { if (thread.joinable()) { graph.cancel(generation); thread.join(); } }
    void join() { thread.join(); }
};
struct Budget {
    explicit Budget(long n) { allocation_budget = n; }
    ~Budget() { allocation_budget = -1; }
};
int cases = 0;
template<class F> void scenario(const char* name, F fn) {
    fn();
    ++cases;
    std::cout << "PASS " << name << '\n';
}
template<class F> void invalid(F fn) {
    bool rejected = false;
    try { fn(); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected);
}
void tests(std::size_t workers) {
    scenario("held producer / pending consumer / publication", [&] {
        Gate gate;
        bool output_valid = false; // deliberately non-atomic: DAG must publish it
        int value = 0;
        std::atomic<int> consumed{0};
        DecodeTaskGraph graph(generation, GraphProfile::Synthetic, {
            task(1, {}, [&](const TaskContext&) { gate.hold(); value = 147; output_valid = true; }),
            task(2, {key(1)}, [&](const TaskContext&) { check(output_valid && value == 147); ++consumed; })});
        Runner runner(graph, workers);
        const bool entered = gate.await_entry();
        // The producer is held before its write. The consumer must still be
        // pending, regardless of how many other workers are available.
        const bool pending = graph.state(1) == TaskState::Pending;
        const bool untouched = !output_valid && consumed.load() == 0;
        const bool stale = !DecodeTaskGraphTestAccess::complete(graph, {1, 2, 2}, 0) &&
                           graph.state(0) == TaskState::Running;
        gate.release();
        runner.join();
        check(entered && pending && untouched && stale && runner.result && consumed == 1);
        check(graph.state(0) == TaskState::Completed && graph.state(1) == TaskState::Completed);
        check(graph.quiescent() && graph.error() == GraphError::None);
    });
    scenario("reversed arrival / highpass only / multiple channels", [&] {
        int published[3][4]{};
        std::vector<TaskSpec> tasks;
        // Bands 1,2,3 only; no implicit LL/bit-zero dependency.
        const auto retain = [](const TaskContext& context) {
            static constexpr unsigned char token = 1;
            check(context.publish(&token, 1));
        };
        tasks.push_back({key(9), {key(8, 0), key(8, 1), key(8, 2)}, [&](const TaskContext& context) {
            for (auto& channel : published) check(channel[0] == 6);
            retain(context);
        }});
        for (std::uint32_t c = 0; c < 3; ++c) {
            tasks.push_back({key(8, c), {key(1, c), key(2, c), key(3, c)}, [&, c](const TaskContext& context) {
                published[c][0] = published[c][1] + published[c][2] + published[c][3];
                retain(context);
            }});
            for (std::uint32_t band = 3; band > 0; --band)
                tasks.push_back({key(band, c), {}, [&, c, band](const TaskContext& context) {
                    published[c][band] = int(band);
                    retain(context);
                }});
        }
        DecodeTaskGraph graph(generation, GraphProfile::Synthetic, std::move(tasks));
        check(graph.run(workers));
        check(!graph.run(workers)); // duplicate submission never executes again
        check(!DecodeTaskGraphTestAccess::complete(graph, generation, 0));
        check(!graph.cancel(generation)); // too late
    });
    scenario("generation reuse / foreign and duplicate completions", [&] {
        for (std::uint64_t version = 2; version < 10; ++version) {
            Generation current{generation.session, generation.slot, version};
            int writes = 0;
            DecodeTaskGraph graph(current, GraphProfile::Synthetic,
                {task(1, {}, [&](const TaskContext&) { ++writes; })});
            check(!DecodeTaskGraphTestAccess::complete(graph, generation, 0));
            check(!graph.cancel(generation));
            check(!graph.cancel({2, current.slot, version}));
            check(!graph.cancel({current.session, 3, version}));
            check(!DecodeTaskGraphTestAccess::complete(graph, current, 0)); // not running
            check(!DecodeTaskGraphTestAccess::complete(graph, current, 9)); // nonexistent
            check(graph.run(workers) && writes == 1);
            check(!DecodeTaskGraphTestAccess::complete(graph, current, 0));
        }
    });
    scenario("missing producer rejected", [&] { invalid([&] {
        DecodeTaskGraph g(generation, GraphProfile::Synthetic, {task(2, {key(1)})});
    }); });
    scenario("duplicate node rejected", [&] { invalid([&] {
        DecodeTaskGraph g(generation, GraphProfile::Synthetic, {task(1), task(1)});
    }); });
    scenario("duplicate predecessor rejected", [&] { invalid([&] {
        DecodeTaskGraph g(generation, GraphProfile::Synthetic, {task(1), task(2, {key(1), key(1)})});
    }); });
    scenario("cycle rejected", [&] { invalid([&] {
        DecodeTaskGraph g(generation, GraphProfile::Synthetic, {task(1, {key(2)}), task(2, {key(1)})});
    }); });
    scenario("self cycle rejected", [&] { invalid([&] {
        DecodeTaskGraph g(generation, GraphProfile::Synthetic, {task(1, {key(1)})});
    }); });
    scenario("profile / identity / topology limits rejected", [&] {
        invalid([&] { DecodeTaskGraph g(generation, GraphProfile::Unsupported, {task(1)}); });
        invalid([&] { DecodeTaskGraph g({0, 0, 1}, GraphProfile::Synthetic, {task(1)}); });
        invalid([&] { DecodeTaskGraph g({1, 0, 0}, GraphProfile::Synthetic, {task(1)}); });
        invalid([&] { DecodeTaskGraph g(generation, GraphProfile::Synthetic, {}); });
        invalid([&] { DecodeTaskGraph g(generation, GraphProfile::Synthetic, {task(1)}, {0, 1}); });
        invalid([&] { DecodeTaskGraph g(generation, GraphProfile::Synthetic, {task(1), task(2, {key(1)})}, {2, 0}); });
        invalid([&] { DecodeTaskGraph g(generation, GraphProfile::Synthetic, {task(1)}, {1, 65537}); });
        std::vector<TaskSpec> too_many;
        for (std::uint32_t i = 0; i < 4097; ++i) too_many.push_back(task(i));
        invalid([&] { DecodeTaskGraph g(generation, GraphProfile::Synthetic, std::move(too_many), {4097, 65536}); });
        invalid([&] { DecodeTaskGraph g(generation, GraphProfile::Synthetic, {task(1, {}, {})}); });
    });
    scenario("impossible predecessor count fails closed", [&] {
        DecodeTaskGraph graph(generation, GraphProfile::Synthetic, {task(1), task(2, {key(1)})});
        DecodeTaskGraphTestAccess::corrupt_count(graph, 1);
        check(!graph.run(workers));
        check(graph.error() == GraphError::InvalidGraph && graph.state(1) == TaskState::Failed);
    });
    scenario("producer exception / dependent failure", [&] {
        int reads = 0;
        DecodeTaskGraph graph(generation, GraphProfile::Synthetic, {
            task(1, {}, [](const TaskContext&) { throw std::runtime_error("synthetic"); }),
            task(2, {key(1)}, [&](const TaskContext&) { ++reads; }), task(3, {key(2)})});
        check(!graph.run(workers));
        check(graph.error() == GraphError::Callback && reads == 0 && graph.quiescent());
        check(graph.state(0) == TaskState::Failed && graph.state(1) == TaskState::Failed && graph.state(2) == TaskState::Failed);
    });
    scenario("allocation callback failure", [&] {
        DecodeTaskGraph graph(generation, GraphProfile::Synthetic,
            {task(1, {}, [](const TaskContext&) { throw std::bad_alloc(); })});
        check(!graph.run(workers) && graph.error() == GraphError::Allocation && graph.quiescent());
    });
    scenario("first producer failure / cancel queued work / drain peers", [&] {
        Gate failure, peer;
        int reads = 0;
        DecodeTaskGraph graph(generation, GraphProfile::Synthetic, {
            task(1, {}, [&](const TaskContext&) { failure.hold(); throw std::runtime_error("first"); }),
            task(2, {}, [&](const TaskContext&) { peer.hold(); throw std::bad_alloc(); }),
            task(3, {key(1)}, [&](const TaskContext&) { ++reads; })});
        Runner runner(graph, workers);
        const bool entered = failure.await_entry();
        const bool peer_entered = workers == 1 || peer.await_entry();
        failure.release();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (graph.error() == GraphError::None && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        const bool first = graph.error() == GraphError::Callback;
        const bool draining = workers == 1 || !graph.quiescent();
        peer.release();
        runner.join();
        check(entered && peer_entered && first && draining && !runner.result && reads == 0);
        check(graph.error() == GraphError::Callback && graph.quiescent());
        check(graph.state(2) == TaskState::Failed);
        if (workers == 1) check(graph.state(1) == TaskState::Canceled);
        else check(graph.state(1) == TaskState::Failed);
    });
    scenario("constructor allocation failures unwind", [&] {
        bool reached_success = false;
        int rejected = 0;
        for (long budget = 0; budget < 64; ++budget) {
            auto specs = std::vector<TaskSpec>{task(1), task(2, {key(1)}), task(3, {key(2)})};
            try {
                Budget fault(budget);
                DecodeTaskGraph graph(generation, GraphProfile::Synthetic, std::move(specs));
                reached_success = true;
            } catch (const std::bad_alloc&) { ++rejected; }
            if (reached_success) break;
        }
        check(reached_success && rejected >= 5);
    });
    scenario("worker storage allocation failure", [&] {
        DecodeTaskGraph graph(generation, GraphProfile::Synthetic, {task(1)});
        bool result;
        { Budget fault(0); result = graph.run(workers); }
        check(!result && graph.error() == GraphError::Allocation && graph.quiescent());
        check(graph.state(0) == TaskState::Canceled);
    });
    scenario("cancel before run / first causal failure retained", [&] {
        int runs = 0;
        DecodeTaskGraph graph(generation, GraphProfile::Synthetic,
            {task(1, {}, [&](const TaskContext&) { ++runs; })});
        check(graph.cancel(generation));
        check(!graph.cancel(generation));
        check(!graph.run(workers) && runs == 0 && graph.quiescent());
        check(graph.error() == GraphError::Canceled && graph.state(0) == TaskState::Canceled);
    });
    scenario("cancel running / drain before buffer release / no publication", [&] {
        Gate gate;
        auto buffer = std::make_shared<int>(0);
        std::weak_ptr<int> lifetime = buffer;
        std::atomic<bool> saw_cancel{false};
        auto graph = std::make_unique<DecodeTaskGraph>(generation, GraphProfile::Synthetic, std::vector<TaskSpec>{
            task(1, {}, [&, owned = buffer](const TaskContext& context) {
                gate.hold();
                saw_cancel = context.cancellation_requested();
                *owned = 147;
                throw std::runtime_error("later error must not replace cancellation");
            }), task(2, {key(1)}, [](const TaskContext&) { throw std::runtime_error("must never read"); })});
        buffer.reset();
        Runner runner(*graph, workers);
        const bool entered = gate.await_entry();
        const bool canceled = graph->cancel(generation);
        const bool draining = !graph->quiescent() && !lifetime.expired();
        gate.release();
        runner.join();
        check(entered && canceled && draining && !runner.result && graph->quiescent() && saw_cancel);
        check(graph->error() == GraphError::Canceled && graph->state(1) == TaskState::Canceled);
        check(!DecodeTaskGraphTestAccess::complete(*graph, generation, 0));
        graph.reset();
        check(lifetime.expired());
    });
    scenario("cancel successful running callback discards completion", [&] {
        Gate gate;
        DecodeTaskGraph graph(generation, GraphProfile::Synthetic, {
            task(1, {}, [&](const TaskContext&) { gate.hold(); }), task(2, {key(1)})});
        Runner runner(graph, workers);
        const bool entered = gate.await_entry();
        const bool canceled = graph.cancel(generation);
        gate.release();
        runner.join();
        check(entered && canceled && !runner.result);
        check(graph.state(0) == TaskState::Canceled && graph.state(1) == TaskState::Canceled);
    });
    scenario("callback reentrancy outside graph lock", [&] {
        DecodeTaskGraph* ptr = nullptr;
        DecodeTaskGraph graph(generation, GraphProfile::Synthetic, {
            task(1, {}, [&](const TaskContext& context) {
                check(!context.cancellation_requested() && !ptr->quiescent());
                check(ptr->state(0) == TaskState::Running);
                check(!ptr->run(1));
                check(ptr->cancel(generation));
            }), task(2, {key(1)})});
        ptr = &graph;
        check(!graph.run(workers) && graph.error() == GraphError::Canceled && graph.quiescent());
    });
    scenario("invalid worker counts rejected / retry", [&] {
        DecodeTaskGraph graph(generation, GraphProfile::Synthetic, {task(1)});
        check(!graph.run(0) && !graph.run(17));
        check(graph.run(workers));
    });
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("Expected worker count");
        const auto workers = std::stoul(argv[1]);
        check(workers == 1 || workers == 2 || workers == 4 || workers == 16);
        tests(workers);
        std::cout << cases << " scenarios passed; workers=" << workers << "; synthetic only\n";
        return 0;
    } catch (...) {
        std::cerr << "FAIL synthetic graph contract after " << cases << " completed scenarios\n";
        return 1;
    }
}
