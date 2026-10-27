// Copyright (c) 2026 Hypercine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// T5 gate: publication after the decision. Issues #144 / #195, after #275.
//
// What this measures, and why it is not already covered:
//
// T1/T2/S2/T4 already prove that a *published* buffer is not readable once the
// graph stopped or recorded an error (admitted_input, wait_for_input), and that
// a version word from a foreign generation is not an input. None of them
// measures the other half: whether the *producer* may still install a
// publication after that decision exists. rule_cancel_does_not_ready() calls
// publish() after cancel() and deliberately discards the return value, so a
// build in which publish() ignored stop_/error_ would pass every existing rule
// and simply retain bytes nobody may read.
//
// The contract under test:
//
//   T5-A  publish() after cancel()              is refused, retains nothing
//   T5-B  publish() after a recorded error      is refused, retains nothing
//   T5-C  publish_plane() after cancel()        is refused, retains nothing
//   T5-D  positive control: the same call on the same key with no decision is
//         ACCEPTED and retains bytes -- so a refusal above cannot be explained
//         by an invalid key, geometry or budget
//
// Every rule measures the return value, the retained-byte counter, the version
// word and the stored-output predicate. A refusal that returned false but kept
// the bytes would fail here; so would a refusal that kept them invisible to
// admitted_input() but charged the generation budget.
//
// Negative control (mutation), verified by hand on this branch:
//   remove `stop_ || error_ != GraphError::None` from DecodeTaskGraph::publish()
//   and publish_plane() -> T5-A, T5-B, T5-C report BROKEN and the exit code is 1.
//   T5-D stays green, which is the point: it is the control that proves the
//   refusal measured above came from the decision and not from the fixture.
//
// Synthetic only. No SDK, no upstream/, no decoder, no bitstream.
#include "decode_task_graph.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace hypercine::detail {

struct DecodeTaskGraphTestAccess {
    static bool published_output(const DecodeTaskGraph& g, std::size_t node) {
        return g.published_output(node);
    }
    static unsigned long long version(const DecodeTaskGraph& g, std::size_t node) {
        std::lock_guard<std::mutex> lock(g.mutex_);
        return g.nodes_.at(node).version.load(std::memory_order_relaxed);
    }
    static BufferVersion input(const DecodeTaskGraph& g, std::size_t node, std::size_t source) {
        return g.input(node, source);
    }
    static bool input_published(const DecodeTaskGraph& g, std::size_t node, std::size_t source) {
        return g.input_published(node, source);
    }
};

} // namespace hypercine::detail

namespace {

using namespace hypercine::detail;

const Generation kGeneration{7, 3, 41};
constexpr std::size_t kPayload = 128;
// A legal S3 plane extent: width 16..480 in steps of 8, height 4..270, pitch a
// multiple of 32 (kS3PlaneAlign) and at least width*2. Using a *valid* extent is
// what makes T5-A..C attributable to the stop/error decision rather than to the
// fixture, and T5-D is the control that proves this extent is accepted.
constexpr std::uint32_t kWidth = 16;
constexpr std::uint32_t kHeight = 4;
constexpr std::size_t kPitch = 32;
constexpr std::size_t kPlaneBytes = kPitch * kHeight;

// The graph rejects an unaligned caller buffer (PlaneStatus::Misaligned), and
// MSVC has no posix_memalign, so the platform difference lives in one helper.
void* AllocateAligned(std::size_t bytes, std::size_t alignment) {
#if defined(_WIN32)
    return _aligned_malloc(bytes, alignment);
#else
    void* memory = nullptr;
    return posix_memalign(&memory, alignment, bytes) == 0 ? memory : nullptr;
#endif
}
void FreeAligned(void* memory) {
#if defined(_WIN32)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

// 32-byte aligned signed plane storage for one publication attempt.
struct PlaneBuffer {
    std::int16_t* data = nullptr;
    PlaneBuffer() {
        void* memory = AllocateAligned(kPlaneBytes, 32);
        if (!memory) throw std::bad_alloc();
        data = static_cast<std::int16_t*>(memory);
        for (std::size_t i = 0; i < kPlaneBytes / sizeof(std::int16_t); ++i) data[i] = 7;
    }
    PlaneBuffer(const PlaneBuffer&) = delete;
    PlaneBuffer& operator=(const PlaneBuffer&) = delete;
    ~PlaneBuffer() { FreeAligned(data); }
};

// A wavelet5 band-0 producer key: the plane form is admitted for
// level 4 (lowpass) or level 5 bands 0..3 only. Using a valid key is what makes
// the refusals in T5-A..C attributable to the stop/error decision.
TaskKey plane_producer() { return TaskKey{0, /*transform=*/0, /*level=*/5, /*frame=*/0, /*band=*/0}; }
TaskKey untyped_producer() { return TaskKey{0, /*transform=*/0, /*level=*/0, /*frame=*/0, /*band=*/1}; }
TaskKey consumer_key() { return TaskKey{0, /*transform=*/0, /*level=*/4, /*frame=*/0, /*band=*/0}; }

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

// A producer that blocks inside its callback until released, so the decision is
// recorded by another thread while this frame is still live. That is the only
// ordering in which "publish after stop" is a real event and not a scheduling
// accident.
struct Gate {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    void hold() {
        entered.store(true);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!release.load() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bool await_entry() {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!entered.load() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return entered.load();
    }
};

// ---------------------------------------------------------------------------
// T5-A (untyped) and T5-C (plane): cancel() while the producer frame is live.
// ---------------------------------------------------------------------------
void rule_cancel_refuses_late_publication(std::size_t workers) {
    const std::size_t width = workers < 2 ? 2 : workers;
    Gate gate;
    std::atomic<int> untyped_accepted{0}, untyped_refused{0};
    std::atomic<int> typed_accepted{0}, typed_refused{0};
    std::atomic<std::size_t> owned_after_cancel{0};
    std::atomic<int> consumer_runs{0};
    std::vector<unsigned char> payload(kPayload, 0x5A);
    PlaneBuffer plane;

    DecodeTaskGraph graph(kGeneration, GraphProfile::Synthetic,
                          {{untyped_producer(), {},
                            [&](const TaskContext& context) {
                                gate.hold();
                                if (context.publish(payload.data(), payload.size()))
                                    ++untyped_accepted;
                                else
                                    ++untyped_refused;
                                if (context.publish_plane(plane.data, kPlaneBytes, kWidth, kHeight,
                                                          kPitch))
                                    ++typed_accepted;
                                else
                                    ++typed_refused;
                            },
                            {[] { return true; }}},
                           {consumer_key(), {untyped_producer()},
                            [&](const TaskContext&) { ++consumer_runs; }, {[] { return true; }}}});

    std::thread runner([&] { graph.run(width); });
    const bool saw = gate.await_entry();
    const bool canceled = saw && graph.cancel(kGeneration);
    owned_after_cancel.store(graph.owned_publication_bytes());
    gate.release.store(true);
    runner.join();

    const bool not_published = !DecodeTaskGraphTestAccess::published_output(graph, 0);
    const auto version = DecodeTaskGraphTestAccess::version(graph, 0);
    char detail[512];
    std::snprintf(detail, sizeof detail,
                  "width=%zu entered=%d canceled=%d untyped=%d/%d typed=%d/%d "
                  "owned_after_cancel=%zu owned_final=%zu version=%llu published=%d "
                  "error=%d consumer=%d",
                  width, static_cast<int>(saw), static_cast<int>(canceled),
                  untyped_accepted.load(), untyped_refused.load(), typed_accepted.load(),
                  typed_refused.load(), owned_after_cancel.load(),
                  graph.owned_publication_bytes(), version,
                  static_cast<int>(!not_published), static_cast<int>(graph.error()),
                  consumer_runs.load());
    const bool untyped_ok = untyped_accepted.load() == 0 && untyped_refused.load() == 1;
    const bool typed_ok = typed_accepted.load() == 0 && typed_refused.load() == 1;
    const bool ok = saw && canceled && untyped_ok && typed_ok && not_published && version == 0 &&
                    owned_after_cancel.load() == 0 && graph.owned_publication_bytes() == 0 &&
                    graph.error() == GraphError::Canceled && consumer_runs.load() == 0 &&
                    graph.quiescent();
    required("T5-A-cancel-refuses-late-publish-untyped", ok, detail);
    required("T5-C-cancel-refuses-late-publish-plane", ok, detail);
}

// ---------------------------------------------------------------------------
// T5-B: the same refusal after a recorded error rather than a cancel. P1 blocks;
// P2 records Callback; only then is P1 released and attempts twice. The second
// attempt proves the refusal is the decision, not "the first one already won".
// ---------------------------------------------------------------------------
void rule_error_refuses_late_publication(std::size_t workers) {
    const std::size_t width = workers < 2 ? 2 : workers;
    Gate gate;
    std::atomic<int> accepted{0}, refused{0};
    std::atomic<std::size_t> owned_after_error{0};
    std::vector<unsigned char> payload(kPayload, 0x33);
    const TaskKey failing{1, /*transform=*/0, /*level=*/0, /*frame=*/0, /*band=*/2};

    DecodeTaskGraph graph(kGeneration, GraphProfile::Synthetic,
                          {{untyped_producer(), {},
                            [&](const TaskContext& context) {
                                gate.hold();
                                for (int attempt = 0; attempt < 2; ++attempt) {
                                    if (context.publish(payload.data(), payload.size()))
                                        ++accepted;
                                    else
                                        ++refused;
                                }
                            },
                            {[] { return true; }}},
                           {failing, {},
                            [&](const TaskContext&) {
                                while (!gate.entered.load())
                                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                throw std::runtime_error("second producer records Callback");
                            },
                            {[] { return true; }}}});

    std::thread runner([&] { graph.run(width); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (graph.error() == GraphError::None && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool errored = graph.error() == GraphError::Callback;
    owned_after_error.store(graph.owned_publication_bytes());
    gate.release.store(true);
    runner.join();

    const bool not_published = !DecodeTaskGraphTestAccess::published_output(graph, 0);
    const auto version = DecodeTaskGraphTestAccess::version(graph, 0);
    char detail[512];
    std::snprintf(detail, sizeof detail,
                  "width=%zu error=%d (expected %d Callback) accepted=%d refused=%d "
                  "owned_after_error=%zu owned_final=%zu version=%llu published=%d",
                  width, static_cast<int>(graph.error()), static_cast<int>(GraphError::Callback),
                  accepted.load(), refused.load(), owned_after_error.load(),
                  graph.owned_publication_bytes(), version, static_cast<int>(!not_published));
    const bool ok = errored && accepted.load() == 0 && refused.load() == 2 && not_published &&
                    version == 0 && owned_after_error.load() == 0 &&
                    graph.owned_publication_bytes() == 0 &&
                    graph.error() == GraphError::Callback && graph.quiescent();
    required("T5-B-error-refuses-late-publish", ok, detail);
}

// ---------------------------------------------------------------------------
// T5-D: positive control. Same producer key, same plane geometry, same payload,
// NO cancel and NO error. The plane publication must be ACCEPTED and the bytes
// retained. Without this rule a refusal in T5-A..C could be produced by a bad
// fixture and the gate would be measuring nothing.
// ---------------------------------------------------------------------------
void rule_valid_publication_is_accepted(std::size_t workers) {
    std::atomic<int> accepted{0}, refused{0};
    std::atomic<int> consumer_view_bad{0};
    std::atomic<int> consumer_published{0};
    PlaneBuffer plane;
    PlaneBuffer lowpass;

    DecodeTaskGraph graph(kGeneration, GraphProfile::Synthetic,
                          {{plane_producer(), {},
                            [&](const TaskContext& context) {
                                if (context.publish_plane(plane.data, kPlaneBytes, kWidth, kHeight,
                                                          kPitch))
                                    ++accepted;
                                else
                                    ++refused;
                            },
                            {[] { return true; }}},
                           {consumer_key(), {plane_producer()},
                            [&](const TaskContext& context) {
                                PlaneIdentity expected;
                                expected.generation = kGeneration;
                                expected.channel = 0;
                                expected.wavelet = 5;
                                expected.band = 0;
                                expected.transform = 0;
                                expected.frame = 0;
                                expected.width = kWidth;
                                expected.height = kHeight;
                                expected.pitch = kPitch;
                                const PlaneView live = context.input_plane(0, expected);
                                if (!live.valid() || live.width != kWidth || live.height != kHeight ||
                                    live.pitch != kPitch || live.bytes < kPlaneBytes)
                                    ++consumer_view_bad;
                                // The lowpass is the consumer's own output: level 4, band 0.
                                // Publishing it keeps the graph profile satisfied; the rule is
                                // still about the producer's plane being admitted.
                                if (context.publish_plane(lowpass.data, kPlaneBytes, kWidth, kHeight,
                                                          kPitch))
                                    ++consumer_published;
                            },
                            {[] { return true; }}}});

    const bool ran = graph.run(workers);
    const bool not_unpublished = DecodeTaskGraphTestAccess::published_output(graph, 0);
    char detail[384];
    std::snprintf(detail, sizeof detail,
                  "run=%d error=%d accepted=%d refused=%d consumer_published=%d owned=%zu "
                  "retained=%d consumer_bad=%d",
                  static_cast<int>(ran), static_cast<int>(graph.error()), accepted.load(),
                  refused.load(), consumer_published.load(), graph.owned_publication_bytes(),
                  static_cast<int>(not_unpublished), consumer_view_bad.load());
    const bool ok = ran && graph.error() == GraphError::None && accepted.load() == 1 &&
                    refused.load() == 0 && consumer_published.load() == 1 &&
                    graph.owned_publication_bytes() == 2 * kPlaneBytes && not_unpublished &&
                    consumer_view_bad.load() == 0 && graph.quiescent();
    required("T5-D-valid-publication-is-accepted", ok, detail);
}

} // namespace

int main(int argc, char** argv) {
    std::size_t workers = 1;
    if (argc > 1) workers = static_cast<std::size_t>(std::strtoul(argv[1], nullptr, 10));
    if (workers == 0 || workers > 16) {
        std::printf("usage: %s [workers 1..16]\n", argv[0]);
        return 2;
    }
    std::printf("INFO workers=%zu synthetic T5 publication-after-decision only\n", workers);
    rule_cancel_refuses_late_publication(workers);
    rule_error_refuses_late_publication(workers);
    rule_valid_publication_is_accepted(workers);
    std::printf("SUMMARY %d rule(s) hold, %d broken\n", holds, failures);
    if (failures) {
        std::printf("SCHEDULER-T5 FAIL\n");
        return 1;
    }
    std::printf("SCHEDULER-T5 PASS\n");
    return 0;
}
