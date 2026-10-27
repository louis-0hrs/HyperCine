// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Synthetic T4 plane dependency (issue #144). Not a decoder, not a wavelet
// kernel, and not a claim that the original SDK races in #195 are fixed.
//
// Guards, and the rule that goes red when that guard is removed:
//   T4-EXTENT    publish_plane rejects a short pitched span   -> T4-extent
//   T4-BUDGET    the generation refuses the plane that does   -> T4-budget
//                not fit
//   T4-KEY       a foreign channel or level is not a plane    -> T4-key
//   T4-STALE     a read named for another generation is empty -> T4-stale
//   T4-IDENTITY  V is not U, and band 1 is not band 0         -> T4-identity
//   T4-GEOMETRY  a wider claim is not this plane              -> T4-geometry
//   T4-OWNED-ALIGN the retained plane copy is 32-byte aligned -> T4-owned-align

#include "decode_task_graph.h"
#include "decode_s3_plane.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#if defined(_WIN32)
#include <malloc.h>
#endif
#include <cstring>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace hypercine::detail {
struct DecodeTaskGraphTestAccess {
    static bool published_output(const DecodeTaskGraph& graph, std::size_t index) {
        return graph.published_output(index);
    }
};
}  // namespace hypercine::detail

namespace {

using namespace hypercine::detail;

const Generation kGen{144, 4, 1};
constexpr std::uint32_t kWidth = 16;
constexpr std::uint32_t kHeight = 4;
constexpr std::size_t kPitch = 32;
constexpr std::size_t kBytes = kPitch * kHeight;
constexpr std::size_t kSamples = kBytes / sizeof(std::int16_t);

int failures = 0;
int holds = 0;

void required(const char* rule, bool ok, const std::string& detail) {
    if (ok) {
        ++holds;
        std::printf("RULE %s HOLDS %s\n", rule, detail.c_str());
        return;
    }
    ++failures;
    std::printf("RULE %s BROKEN %s\n", rule, detail.c_str());
}

std::int16_t sample_at(std::uint32_t band, std::size_t index) {
    return static_cast<std::int16_t>(1000 + band * 17 + static_cast<int>(index));
}

TaskKey plane_key(std::uint32_t channel, std::uint32_t level, std::uint32_t band,
                  std::uint32_t frame = 0) {
    return TaskKey{channel, /*transform=*/0, level, frame, band};
}

PlaneIdentity identity(std::uint32_t channel, std::uint32_t wavelet, std::uint32_t band,
                       std::uint32_t frame = 0) {
    PlaneIdentity id;
    id.generation = kGen;
    id.channel = channel;
    id.wavelet = wavelet;
    id.band = band;
    id.transform = 0;
    id.frame = frame;
    id.width = kWidth;
    id.height = kHeight;
    id.pitch = kPitch;
    return id;
}

namespace {
// MSVC has no posix_memalign, and this test is built by the Windows CI jobs. One
// helper keeps the platform difference in one place instead of two call sites.
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
} // namespace

struct HeapPlane {
    std::int16_t* data = nullptr;
    HeapPlane() {
        void* memory = AllocateAligned(kBytes, 32);
        if (!memory) throw std::bad_alloc();
        data = static_cast<std::int16_t*>(memory);
        std::memset(data, 0, kBytes);
    }
    HeapPlane(const HeapPlane&) = delete;
    HeapPlane& operator=(const HeapPlane&) = delete;
    ~HeapPlane() { FreeAligned(data); }
    void fill(std::uint32_t band) {
        for (std::size_t i = 0; i < kSamples; ++i) data[i] = sample_at(band, i);
    }
};

bool matches(const PlaneView& view, std::uint32_t band) {
    if (!view.valid() || view.bytes != kBytes || view.data == nullptr) return false;
    const auto* samples = static_cast<const std::int16_t*>(view.data);
    for (std::size_t i = 0; i < kSamples; ++i)
        if (samples[i] != sample_at(band, i)) return false;
    return true;
}

bool publish_witness(const TaskContext& context) {
    HeapPlane witness;
    return context.publish_plane(witness.data, kBytes, kWidth, kHeight, kPitch);
}

// Four wavelet5 bands, a probe that may run as soon as band 3 exists, and a
// lowpass that must not run until all four edges are published. Each read is
// the band that edge names, including when band 3 is the first task in the
// ready queue.
void rule_dependency_order(std::size_t workers) {
    std::atomic<int> publishes{0};
    std::atomic<int> probe_runs{0};
    std::atomic<int> inverse_runs{0};
    std::atomic<int> bad{0};
    std::atomic<int> crossed{0};
    const std::vector<TaskKey> bands = {plane_key(0, 5, 0), plane_key(0, 5, 1), plane_key(0, 5, 2),
                                        plane_key(0, 5, 3)};
    std::vector<TaskSpec> tasks;
    // Band 3 is listed first so a single worker publishes it before the others.
    const std::uint32_t order[4] = {3, 1, 2, 0};
    for (std::uint32_t band : order) {
        tasks.push_back(TaskSpec{bands[band], {}, [&, band](const TaskContext& context) {
                                     HeapPlane plane;
                                     plane.fill(band);
                                     if (!context.publish_plane(plane.data, kBytes, kWidth, kHeight, kPitch))
                                         bad.fetch_add(1);
                                     else
                                         publishes.fetch_add(1);
                                 }});
    }
    tasks.push_back(TaskSpec{
        plane_key(0, 5, 0, /*frame=*/1),
        {bands[3]},
        [&](const TaskContext& context) {
            probe_runs.fetch_add(1);
            const PlaneView own = context.input_plane(0, identity(0, 5, 3));
            const PlaneView as_band0 = context.input_plane(0, identity(0, 5, 0));
            if (!matches(own, 3) || as_band0.valid() || own.data == nullptr) bad.fetch_add(1);
            if (!publish_witness(context)) bad.fetch_add(1);
        }});
    tasks.push_back(TaskSpec{
        plane_key(0, 4, 0),
        {bands[0], bands[1], bands[2], bands[3]},
        [&](const TaskContext& context) {
            inverse_runs.fetch_add(1);
            if (publishes.load() != 4) bad.fetch_add(1);
            for (std::uint32_t band = 0; band < 4; ++band) {
                const PlaneView view = context.input_plane(band, identity(0, 5, band));
                if (!matches(view, band)) bad.fetch_add(1);
            }
            // Index 0 is band 0 even though band 3 was queued first.
            if (context.input_plane(0, identity(0, 5, 3)).valid()) crossed.fetch_add(1);
            if (!publish_witness(context)) bad.fetch_add(1);
        }});

    DecodeTaskGraph graph(kGen, GraphProfile::Synthetic, std::move(tasks));
    const bool ran = graph.run(workers);
    const bool ok = ran && graph.error() == GraphError::None && graph.quiescent() &&
                    publishes.load() == 4 && probe_runs.load() == 1 && inverse_runs.load() == 1 &&
                    bad.load() == 0 && crossed.load() == 0 &&
                    graph.owned_publication_bytes() == 6 * kBytes;
    char detail[256];
    std::snprintf(detail, sizeof detail,
                  "run=%d error=%d publishes=%d probe=%d inverse=%d bad=%d crossed=%d owned=%zu",
                  ran ? 1 : 0, static_cast<int>(graph.error()), publishes.load(), probe_runs.load(),
                  inverse_runs.load(), bad.load(), crossed.load(), graph.owned_publication_bytes());
    required("T4-order", ok, ok ? "each edge kept its own band; the lowpass waited for all four"
                                : std::string(detail));
}

// The consumer is not Ready while the producer is still inside its callback,
// before publish_plane(). Releasing the producer then hands out the copy.
void rule_premature_access(std::size_t workers) {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
    std::atomic<int> consumer_runs{0};
    std::atomic<int> bad{0};
    DecodeTaskGraph graph(kGen, GraphProfile::Synthetic,
                          {TaskSpec{plane_key(0, 5, 0),
                                    {},
                                    [&](const TaskContext& context) {
                                        {
                                            std::lock_guard<std::mutex> lock(mutex);
                                            entered = true;
                                        }
                                        changed.notify_all();
                                        {
                                            std::unique_lock<std::mutex> lock(mutex);
                                            changed.wait(lock, [&] { return release; });
                                        }
                                        HeapPlane plane;
                                        plane.fill(0);
                                        if (!context.publish_plane(plane.data, kBytes, kWidth, kHeight,
                                                                   kPitch))
                                            bad.fetch_add(1);
                                    }},
                           TaskSpec{plane_key(0, 4, 0),
                                    {plane_key(0, 5, 0)},
                                    [&](const TaskContext& context) {
                                        consumer_runs.fetch_add(1);
                                        if (!matches(context.input_plane(0, identity(0, 5, 0)), 0))
                                            bad.fetch_add(1);
                                        if (!publish_witness(context)) bad.fetch_add(1);
                                    }}});
    bool ran = false;
    std::thread runner([&] { ran = graph.run(workers); });
    bool started = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        started = changed.wait_for(lock, std::chrono::seconds(2), [&] { return entered; });
    }
    const int early = consumer_runs.load();
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    changed.notify_all();
    runner.join();
    const bool ok = started && early == 0 && ran && graph.error() == GraphError::None &&
                    consumer_runs.load() == 1 && bad.load() == 0 && graph.quiescent();
    char detail[192];
    std::snprintf(detail, sizeof detail, "started=%d early=%d run=%d runs=%d bad=%d error=%d",
                  started ? 1 : 0, early, ran ? 1 : 0, consumer_runs.load(), bad.load(),
                  static_cast<int>(graph.error()));
    required("T4-premature", ok,
             ok ? "the lowpass did not run before the plane was published" : std::string(detail));
}

// The caller frees and poisons its buffer before either dependent runs. Both
// read the graph-owned copy, not the freed address.
void rule_premature_free(std::size_t workers) {
    const void* freed = nullptr;
    std::atomic<int> reads{0};
    std::atomic<int> bad{0};
    auto consume = [&](const TaskContext& context) {
        const PlaneView view = context.input_plane(0, identity(0, 5, 0));
        if (view.data == freed || !matches(view, 0)) {
            bad.fetch_add(1);
            return;
        }
        reads.fetch_add(1);
        if (!publish_witness(context)) bad.fetch_add(1);
    };
    DecodeTaskGraph graph(
        kGen, GraphProfile::Synthetic,
        {TaskSpec{plane_key(0, 5, 0),
                  {},
                  [&](const TaskContext& context) {
                      void* memory = AllocateAligned(kBytes, 32);
                      if (!memory) throw std::bad_alloc();
                      auto* samples = static_cast<std::int16_t*>(memory);
                      for (std::size_t i = 0; i < kSamples; ++i) samples[i] = sample_at(0, i);
                      const bool published =
                          context.publish_plane(samples, kBytes, kWidth, kHeight, kPitch);
                      freed = samples;
                      std::memset(samples, 0x5A, kBytes);
                      FreeAligned(samples);
                      if (!published) throw std::runtime_error("plane publish failed");
                  }},
         TaskSpec{plane_key(0, 4, 0), {plane_key(0, 5, 0)}, consume},
         TaskSpec{plane_key(1, 4, 0), {plane_key(0, 5, 0)}, consume}});
    const bool ran = graph.run(workers);
    const bool ok = ran && graph.error() == GraphError::None && reads.load() == 2 && bad.load() == 0 &&
                    graph.quiescent() && graph.owned_publication_bytes() == 3 * kBytes;
    char detail[192];
    std::snprintf(detail, sizeof detail, "run=%d error=%d reads=%d bad=%d owned=%zu", ran ? 1 : 0,
                  static_cast<int>(graph.error()), reads.load(), bad.load(),
                  graph.owned_publication_bytes());
    required("T4-premature-free", ok,
             ok ? "both dependents read the copy after the caller buffer was freed"
                : std::string(detail));
}

// Guard (T4-EXTENT): bytes shorter than pitch*height are not a publication.
// A later full publish of the same task is still admitted.
void rule_extent(std::size_t workers) {
    std::atomic<int> rejected{0};
    std::atomic<int> accepted{0};
    std::atomic<int> bad{0};
    DecodeTaskGraph graph(
        kGen, GraphProfile::Synthetic,
        {TaskSpec{plane_key(0, 5, 0),
                  {},
                  [&](const TaskContext& context) {
                      HeapPlane plane;
                      plane.fill(0);
                      if (!context.publish_plane(plane.data, kBytes - 2, kWidth, kHeight, kPitch))
                          rejected.fetch_add(1);
                      if (context.publish_plane(plane.data, kBytes, kWidth, kHeight, kPitch))
                          accepted.fetch_add(1);
                  }},
         TaskSpec{plane_key(0, 4, 0), {plane_key(0, 5, 0)}, [&](const TaskContext& context) {
                      const PlaneView view = context.input_plane(0, identity(0, 5, 0));
                      if (!matches(view, 0)) bad.fetch_add(1);
                      if (!publish_witness(context)) bad.fetch_add(1);
                  }}});
    const bool ran = graph.run(workers);
    const bool producer_published = DecodeTaskGraphTestAccess::published_output(graph, 0);
    const bool ok = ran && graph.error() == GraphError::None && rejected.load() == 1 &&
                    accepted.load() == 1 && bad.load() == 0 && producer_published &&
                    graph.owned_publication_bytes() == 2 * kBytes && graph.quiescent();
    char detail[224];
    std::snprintf(detail, sizeof detail,
                  "run=%d error=%d rejected=%d accepted=%d bad=%d published=%d owned=%zu", ran ? 1 : 0,
                  static_cast<int>(graph.error()), rejected.load(), accepted.load(), bad.load(),
                  producer_published ? 1 : 0, graph.owned_publication_bytes());
    required("T4-extent", ok,
             ok ? "a short span stayed unpublished; the full plane was admitted afterwards"
                : std::string(detail));
}

// Guard (T4-BUDGET): the second retained plane does not fit. The lowpass is
// not readied, and the rejected call adds no bytes.
void rule_budget(std::size_t workers) {
    std::atomic<int> published{0};
    std::atomic<int> refused{0};
    std::atomic<int> consumer_runs{0};
    GraphLimits limits;
    limits.generation_budget_bytes = kBytes;
    const std::size_t ready_before_slot = 2;
    DecodeTaskGraph graph(
        kGen, GraphProfile::Synthetic,
        {TaskSpec{plane_key(0, 5, 0),
                  {},
                  [&](const TaskContext& context) {
                      HeapPlane plane;
                      plane.fill(0);
                      if (context.publish_plane(plane.data, kBytes, kWidth, kHeight, kPitch))
                          published.fetch_add(1);
                      else
                          refused.fetch_add(1);
                  }},
         TaskSpec{plane_key(0, 5, 1),
                  {},
                  [&](const TaskContext& context) {
                      HeapPlane plane;
                      plane.fill(1);
                      if (context.publish_plane(plane.data, kBytes, kWidth, kHeight, kPitch))
                          published.fetch_add(1);
                      else
                          refused.fetch_add(1);
                  }},
         TaskSpec{plane_key(0, 4, 0),
                  {plane_key(0, 5, 0), plane_key(0, 5, 1)},
                  [&](const TaskContext&) { consumer_runs.fetch_add(1); }}},
        limits);
    const std::size_t ready_before = graph.ready_transitions();
    const bool ran = graph.run(workers);
    const bool consumer_held = graph.state(2) != TaskState::Ready &&
                               graph.state(2) != TaskState::Running &&
                               graph.state(2) != TaskState::Completed;
    // The budget is charged when publish_plane() succeeds, but a failed graph
    // releases every publication whose producer did not reach Completed. Under
    // parallel workers the accepted producer can race the other producer's
    // OutputNotPublished failure: it either completed first (the copy remains)
    // or is canceled while still Running (release_abandoned_locked refunds it).
    // A fixed post-run kBytes expectation incorrectly rejects that valid cleanup.
    const TaskState band0_state = graph.state(0);
    const TaskState band1_state = graph.state(1);
    const std::size_t completed_producers =
        (band0_state == TaskState::Completed ? 1u : 0u) +
        (band1_state == TaskState::Completed ? 1u : 0u);
    const std::size_t expected_retained_bytes = completed_producers * kBytes;
    const bool publication_matches_completion =
        DecodeTaskGraphTestAccess::published_output(graph, 0) ==
            (band0_state == TaskState::Completed) &&
        DecodeTaskGraphTestAccess::published_output(graph, 1) ==
            (band1_state == TaskState::Completed);
    const bool budget_postdrain = completed_producers <= 1 &&
                                  graph.owned_publication_bytes() == expected_retained_bytes &&
                                  publication_matches_completion;
    const bool ok = !ran && graph.error() == GraphError::OutputNotPublished && graph.quiescent() &&
                    published.load() == 1 && refused.load() == 1 && consumer_runs.load() == 0 &&
                    consumer_held && budget_postdrain &&
                    graph.ready_transitions() == ready_before && ready_before == ready_before_slot;
    char detail[320];
    std::snprintf(detail, sizeof detail,
                  "run=%d error=%d published=%d refused=%d consumer=%d owned=%zu expected_owned=%zu "
                  "producer_states=%d,%d publication_matches=%d ready %zu->%zu state=%d",
                  ran ? 1 : 0, static_cast<int>(graph.error()), published.load(), refused.load(),
                  consumer_runs.load(), graph.owned_publication_bytes(), expected_retained_bytes,
                  static_cast<int>(band0_state), static_cast<int>(band1_state),
                  publication_matches_completion ? 1 : 0, ready_before,
                  graph.ready_transitions(), static_cast<int>(graph.state(2)));
    required("T4-budget", ok,
             ok ? "one plane was accepted, one refused; completed copies retained, canceled copies refunded"
                : std::string(detail));
}

// Guard (T4-KEY): channel 3, wavelet level 3, and a wavelet4 highpass are not
// planes. The version stays 0 and nothing is retained.
void rule_key(std::size_t workers) {
    auto attempt = [&](TaskKey key, int* refused, int* error, int* owned_planes) {
        std::atomic<int> local_refused{0};
        DecodeTaskGraph graph(kGen, GraphProfile::Synthetic,
                              {TaskSpec{key, {}, [&](const TaskContext& context) {
                                            HeapPlane plane;
                                            plane.fill(0);
                                            if (!context.publish_plane(plane.data, kBytes, kWidth, kHeight,
                                                                       kPitch))
                                                local_refused.fetch_add(1);
                                        }}});
        const bool ran = graph.run(workers);
        *refused = local_refused.load();
        *error = static_cast<int>(graph.error());
        *owned_planes = graph.owned_publication_bytes() == 0 ? 1 : 0;
        return !ran && graph.error() == GraphError::OutputNotPublished && local_refused.load() == 1 &&
               graph.owned_publication_bytes() == 0 && graph.quiescent() &&
               !DecodeTaskGraphTestAccess::published_output(graph, 0);
    };
    int channel_refused = 0, channel_error = -1, channel_empty = 0;
    int level_refused = 0, level_error = -1, level_empty = 0;
    int highpass_refused = 0, highpass_error = -1, highpass_empty = 0;
    const bool channel = attempt(TaskKey{3, 0, 5, 0, 0}, &channel_refused, &channel_error, &channel_empty);
    const bool level = attempt(TaskKey{0, 0, 3, 0, 0}, &level_refused, &level_error, &level_empty);
    const bool highpass =
        attempt(TaskKey{0, 0, 4, 0, 1}, &highpass_refused, &highpass_error, &highpass_empty);
    const bool ok = channel && level && highpass;
    char detail[256];
    std::snprintf(detail, sizeof detail,
                  "channel refused=%d error=%d; level refused=%d error=%d; highpass refused=%d error=%d",
                  channel_refused, channel_error, level_refused, level_error, highpass_refused,
                  highpass_error);
    required("T4-key", ok,
             ok ? "channel 3, level 3 and wavelet4 band 1 were not published" : std::string(detail));
}

// Guard (T4-IDENTITY): the bytes may exist and still be the wrong edge.
void rule_identity(std::size_t workers) {
    std::atomic<int> bad{0};
    std::atomic<int> untyped{0};
    DecodeTaskGraph graph(
        kGen, GraphProfile::Synthetic,
        {TaskSpec{plane_key(1, 5, 0),
                  {},
                  [&](const TaskContext& context) {
                      HeapPlane plane;
                      plane.fill(1);
                      if (!context.publish_plane(plane.data, kBytes, kWidth, kHeight, kPitch))
                          bad.fetch_add(1);
                  }},
         TaskSpec{plane_key(1, 4, 0), {plane_key(1, 5, 0)}, [&](const TaskContext& context) {
                      const PlaneView live = context.input_plane(0, identity(1, 5, 0));
                      const PlaneView as_u = context.input_plane(0, identity(2, 5, 0));
                      const PlaneView as_band1 = context.input_plane(0, identity(1, 5, 1));
                      PlaneIdentity other_frame = identity(1, 5, 0);
                      other_frame.frame = 1;
                      const PlaneView as_frame = context.input_plane(0, other_frame);
                      const PlaneView as_lowpass = context.input_plane(0, identity(1, 4, 0));
                      const BufferVersion raw = context.input(0);
                      if (raw.valid() && raw.bytes == kBytes) untyped.fetch_add(1);
                      if (!matches(live, 1) || as_u.valid() || as_band1.valid() || as_frame.valid() ||
                          as_lowpass.valid())
                          bad.fetch_add(1);
                      if (!publish_witness(context)) bad.fetch_add(1);
                  }}});
    const bool ran = graph.run(workers);
    const bool ok = ran && graph.error() == GraphError::None && bad.load() == 0 && untyped.load() == 1 &&
                    graph.quiescent();
    char detail[128];
    std::snprintf(detail, sizeof detail, "run=%d error=%d bad=%d untyped=%d", ran ? 1 : 0,
                  static_cast<int>(graph.error()), bad.load(), untyped.load());
    required("T4-identity", ok,
             ok ? "V/band0 matched; U, band 1, frame 1 and wavelet4 did not" : std::string(detail));
}

// Guard (T4-STALE): naming the next generation must not reveal this one's bytes.
void rule_stale(std::size_t workers) {
    std::atomic<int> bad{0};
    DecodeTaskGraph graph(
        kGen, GraphProfile::Synthetic,
        {TaskSpec{plane_key(0, 5, 0),
                  {},
                  [&](const TaskContext& context) {
                      HeapPlane plane;
                      plane.fill(0);
                      if (!context.publish_plane(plane.data, kBytes, kWidth, kHeight, kPitch))
                          bad.fetch_add(1);
                  }},
         TaskSpec{plane_key(0, 4, 0), {plane_key(0, 5, 0)}, [&](const TaskContext& context) {
                      const PlaneView live = context.input_plane(0, identity(0, 5, 0));
                      PlaneIdentity stale = identity(0, 5, 0);
                      stale.generation.value += 1;
                      const PlaneView dead = context.input_plane(0, stale);
                      if (!matches(live, 0) || dead.valid()) bad.fetch_add(1);
                      if (!publish_witness(context)) bad.fetch_add(1);
                  }}});
    const bool ran = graph.run(workers);
    const bool ok = ran && graph.error() == GraphError::None && bad.load() == 0 && graph.quiescent();
    char detail[96];
    std::snprintf(detail, sizeof detail, "run=%d error=%d bad=%d", ran ? 1 : 0,
                  static_cast<int>(graph.error()), bad.load());
    required("T4-stale", ok,
             ok ? "the next generation observed an empty view" : std::string(detail));
}

// Guard (T4-GEOMETRY): same bytes, wider claim. pitch*height still covers the
// buffer, so only the stored extent rejects it.
void rule_geometry(std::size_t workers) {
    std::atomic<int> bad{0};
    DecodeTaskGraph graph(
        kGen, GraphProfile::Synthetic,
        {TaskSpec{plane_key(0, 5, 0),
                  {},
                  [&](const TaskContext& context) {
                      HeapPlane plane;
                      plane.fill(0);
                      if (!context.publish_plane(plane.data, kBytes, kWidth, kHeight, kPitch))
                          bad.fetch_add(1);
                  }},
         TaskSpec{plane_key(0, 4, 0), {plane_key(0, 5, 0)}, [&](const TaskContext& context) {
                      PlaneIdentity wider = identity(0, 5, 0);
                      wider.width = 24;
                      const PlaneView wrong = context.input_plane(0, wider);
                      const PlaneView live = context.input_plane(0, identity(0, 5, 0));
                      if (wrong.valid() || !matches(live, 0)) bad.fetch_add(1);
                      if (!publish_witness(context)) bad.fetch_add(1);
                  }}});
    const bool ran = graph.run(workers);
    const bool ok = ran && graph.error() == GraphError::None && bad.load() == 0 && graph.quiescent();
    char detail[96];
    std::snprintf(detail, sizeof detail, "run=%d error=%d bad=%d", ran ? 1 : 0,
                  static_cast<int>(graph.error()), bad.load());
    required("T4-geometry", ok,
             ok ? "a 24-wide claim over the 16-wide plane was empty" : std::string(detail));
}

// No plane, no spin, no invented samples. The lowpass is not readied.
void rule_missing(std::size_t workers) {
    std::atomic<int> consumer_runs{0};
    const auto started = std::chrono::steady_clock::now();
    DecodeTaskGraph graph(kGen, GraphProfile::Synthetic,
                          {TaskSpec{plane_key(0, 5, 0), {}, [](const TaskContext&) {}},
                           TaskSpec{plane_key(0, 4, 0),
                                    {plane_key(0, 5, 0)},
                                    [&](const TaskContext&) { consumer_runs.fetch_add(1); }}});
    const bool ran = graph.run(workers);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const bool prompt = elapsed < std::chrono::seconds(2);
    const bool ok = !ran && graph.error() == GraphError::OutputNotPublished && graph.quiescent() &&
                    consumer_runs.load() == 0 && graph.owned_publication_bytes() == 0 && prompt &&
                    graph.state(1) == TaskState::Failed &&
                    !DecodeTaskGraphTestAccess::published_output(graph, 0);
    char detail[192];
    std::snprintf(detail, sizeof detail, "run=%d error=%d consumer=%d owned=%zu prompt=%d state=%d",
                  ran ? 1 : 0, static_cast<int>(graph.error()), consumer_runs.load(),
                  graph.owned_publication_bytes(), prompt ? 1 : 0, static_cast<int>(graph.state(1)));
    required("T4-missing", ok,
             ok ? "a missing plane failed closed without blocking or inventing samples"
                : std::string(detail));
}

// Lax producer, so the consumer actually waits. The deadline is terminal and
// the plane view stays empty: no zero-filled stand-in.
void rule_timeout(std::size_t workers) {
    std::atomic<int> returned{0};
    std::atomic<int> lied{0};
    GraphLimits limits;
    limits.require_published_output = false;
    const auto started = std::chrono::steady_clock::now();
    DecodeTaskGraph graph(
        kGen, GraphProfile::Synthetic,
        {TaskSpec{plane_key(0, 5, 0), {}, [](const TaskContext&) {}},
         TaskSpec{plane_key(0, 4, 0), {plane_key(0, 5, 0)}, [&](const TaskContext& context) {
                      try {
                          context.wait_for_input(0, std::chrono::milliseconds(80));
                          returned.fetch_add(1);
                      } catch (const PublicationTimeout&) {
                          if (context.input_plane(0, identity(0, 5, 0)).valid()) lied.fetch_add(1);
                          throw;
                      }
                  }}},
        limits);
    const bool ran = graph.run(workers);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const bool prompt = elapsed < std::chrono::seconds(2);
    const bool ok = !ran && graph.error() == GraphError::Timeout && graph.quiescent() &&
                    returned.load() == 0 && lied.load() == 0 && prompt &&
                    graph.owned_publication_bytes() == 0 &&
                    !DecodeTaskGraphTestAccess::published_output(graph, 1);
    char detail[192];
    std::snprintf(detail, sizeof detail, "run=%d error=%d returned=%d lied=%d prompt=%d", ran ? 1 : 0,
                  static_cast<int>(graph.error()), returned.load(), lied.load(), prompt ? 1 : 0);
    required("T4-timeout", ok,
             ok ? "the deadline ended the wait and no plane was invented" : std::string(detail));
}

// One aborted producer. The lowpass does not run. The error is the exception,
// not a later success.
void rule_abort(std::size_t workers) {
    std::atomic<int> consumer_runs{0};
    DecodeTaskGraph graph(kGen, GraphProfile::Synthetic,
                          {TaskSpec{plane_key(0, 5, 0),
                                    {},
                                    [](const TaskContext&) { throw std::runtime_error("producer aborted"); }},
                           TaskSpec{plane_key(0, 4, 0),
                                    {plane_key(0, 5, 0)},
                                    [&](const TaskContext&) { consumer_runs.fetch_add(1); }}});
    const bool ran = graph.run(workers);
    const bool ok = !ran && graph.error() == GraphError::Callback && graph.quiescent() &&
                    consumer_runs.load() == 0 && graph.state(0) == TaskState::Failed &&
                    graph.state(1) == TaskState::Failed && graph.owned_publication_bytes() == 0;
    char detail[160];
    std::snprintf(detail, sizeof detail, "run=%d error=%d consumer=%d producer=%d lowpass=%d", ran ? 1 : 0,
                  static_cast<int>(graph.error()), consumer_runs.load(), static_cast<int>(graph.state(0)),
                  static_cast<int>(graph.state(1)));
    required("T4-abort", ok, ok ? "the aborted producer failed the generation; the lowpass never ran"
                                : std::string(detail));
}

void rule_cancel(std::size_t workers) {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
    std::atomic<int> consumer_runs{0};
    std::atomic<int> published{0};
    DecodeTaskGraph graph(kGen, GraphProfile::Synthetic,
                          {TaskSpec{plane_key(0, 5, 0),
                                    {},
                                    [&](const TaskContext& context) {
                                        {
                                            std::lock_guard<std::mutex> lock(mutex);
                                            entered = true;
                                        }
                                        changed.notify_all();
                                        {
                                            std::unique_lock<std::mutex> lock(mutex);
                                            changed.wait(lock, [&] { return release; });
                                        }
                                        if (context.cancellation_requested()) return;
                                        HeapPlane plane;
                                        if (context.publish_plane(plane.data, kBytes, kWidth, kHeight, kPitch))
                                            published.fetch_add(1);
                                    }},
                           TaskSpec{plane_key(0, 4, 0),
                                    {plane_key(0, 5, 0)},
                                    [&](const TaskContext&) { consumer_runs.fetch_add(1); }}});
    bool ran = true;
    std::thread runner([&] { ran = graph.run(workers); });
    bool started = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        started = changed.wait_for(lock, std::chrono::seconds(2), [&] { return entered; });
    }
    const bool canceled = started && graph.cancel(kGen);
    const bool second = graph.cancel(kGen);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    changed.notify_all();
    runner.join();
    const bool ok = started && canceled && !second && !ran && graph.error() == GraphError::Canceled &&
                    graph.quiescent() && consumer_runs.load() == 0 && published.load() == 0 &&
                    graph.owned_publication_bytes() == 0 && graph.state(1) == TaskState::Canceled;
    char detail[192];
    std::snprintf(detail, sizeof detail,
                  "started=%d cancel=%d second=%d run=%d error=%d consumer=%d published=%d state=%d",
                  started ? 1 : 0, canceled ? 1 : 0, second ? 1 : 0, ran ? 1 : 0,
                  static_cast<int>(graph.error()), consumer_runs.load(), published.load(),
                  static_cast<int>(graph.state(1)));
    required("T4-cancel", ok,
             ok ? "the matching generation canceled once; no plane was published" : std::string(detail));
}

// A stale generation does not cancel this one, and the plane still completes.
void rule_cancel_stale(std::size_t workers) {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
    std::atomic<int> bad{0};
    DecodeTaskGraph graph(
        kGen, GraphProfile::Synthetic,
        {TaskSpec{plane_key(0, 5, 0),
                  {},
                  [&](const TaskContext& context) {
                      {
                          std::lock_guard<std::mutex> lock(mutex);
                          entered = true;
                      }
                      changed.notify_all();
                      {
                          std::unique_lock<std::mutex> lock(mutex);
                          changed.wait(lock, [&] { return release; });
                      }
                      HeapPlane plane;
                      plane.fill(0);
                      if (!context.publish_plane(plane.data, kBytes, kWidth, kHeight, kPitch))
                          bad.fetch_add(1);
                  }},
         TaskSpec{plane_key(0, 4, 0), {plane_key(0, 5, 0)}, [&](const TaskContext& context) {
                      if (!matches(context.input_plane(0, identity(0, 5, 0)), 0)) bad.fetch_add(1);
                      if (!publish_witness(context)) bad.fetch_add(1);
                  }}});
    bool ran = false;
    std::thread runner([&] { ran = graph.run(workers); });
    bool started = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        started = changed.wait_for(lock, std::chrono::seconds(2), [&] { return entered; });
    }
    Generation stale = kGen;
    stale.value += 1;
    const bool canceled = graph.cancel(stale);
    const GraphError mid = graph.error();
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    changed.notify_all();
    runner.join();
    const bool ok = started && !canceled && mid == GraphError::None && ran &&
                    graph.error() == GraphError::None && bad.load() == 0 && graph.quiescent();
    char detail[160];
    std::snprintf(detail, sizeof detail, "started=%d stale_cancel=%d mid=%d run=%d error=%d bad=%d",
                  started ? 1 : 0, canceled ? 1 : 0, static_cast<int>(mid), ran ? 1 : 0,
                  static_cast<int>(graph.error()), bad.load());
    required("T4-cancel-stale", ok,
             ok ? "the other generation did not cancel this one" : std::string(detail));
}

// Several generations, each destroyed after drain. The retained copies must
// not outlive the graph; AddressSanitizer is the leak check.
// Guard (T4-OWNED-ALIGN): publish_plane copies into graph storage. That
// address is what input_plane() returns. Sixteen frames, one legal plane
// each, so a single accidentally aligned allocation cannot pass.
void rule_owned_alignment(std::size_t workers) {
    constexpr std::uint32_t kFrames = 16;
    std::atomic<int> bad{0};
    std::atomic<int> seen{0};
    std::vector<TaskSpec> tasks;
    tasks.reserve(kFrames * 2);
    for (std::uint32_t frame = 0; frame < kFrames; ++frame) {
        tasks.push_back(TaskSpec{
            plane_key(0, 5, 0, frame), {},
            [frame, &bad](const TaskContext& context) {
                HeapPlane plane;
                plane.fill(0);
                if (!context.publish_plane(plane.data, kBytes, kWidth, kHeight, kPitch))
                    bad.fetch_add(1);
            }});
        tasks.push_back(TaskSpec{
            plane_key(0, 4, 0, frame), {plane_key(0, 5, 0, frame)},
            [frame, &bad, &seen](const TaskContext& context) {
                const PlaneView view = context.input_plane(0, identity(0, 5, 0, frame));
                seen.fetch_add(1);
                const auto address = reinterpret_cast<std::uintptr_t>(view.data);
                if (!view.valid() || view.bytes != kBytes || (address % 32) != 0)
                    bad.fetch_add(1);
            }});
    }
    GraphLimits limits;
    limits.require_published_output = false;
    DecodeTaskGraph graph(kGen, GraphProfile::Synthetic, std::move(tasks), limits);
    const bool ran = graph.run(workers);
    const bool ok = ran && graph.error() == GraphError::None &&
                    seen.load() == static_cast<int>(kFrames) && bad.load() == 0;
    char detail[160];
    std::snprintf(detail, sizeof detail, "run=%d error=%d seen=%d bad=%d",
                  ran ? 1 : 0, static_cast<int>(graph.error()), seen.load(), bad.load());
    required("T4-owned-align", ok,
             ok ? "sixteen graph-owned plane copies were 32-byte aligned"
                : std::string(detail));
}

void rule_release(std::size_t workers) {
    bool ok = true;
    for (std::uint64_t value = 1; value <= 4; ++value) {
        Generation generation{144, 4, value};
        std::atomic<int> bad{0};
        {
            DecodeTaskGraph graph(generation, GraphProfile::Synthetic,
                                  {TaskSpec{plane_key(0, 5, 0), {}, [&](const TaskContext& context) {
                                                HeapPlane plane;
                                                plane.fill(0);
                                                if (!context.publish_plane(plane.data, kBytes, kWidth, kHeight,
                                                                           kPitch))
                                                    bad.fetch_add(1);
                                            }}});
            if (!graph.run(workers) || graph.owned_publication_bytes() != kBytes || bad.load() != 0)
                ok = false;
        }
    }
    required("T4-release", ok, ok ? "four generations retained one plane each and were destroyed after drain"
                                  : "a generation leaked its charge or failed to publish");
}

// BUDGET-ALL: the ceiling covers untyped publication too. Before this rule the
// budget was charged only by publish_plane(), so a plain publish() could retain
// storage that no counter accounted for -- the graph could hold unbounded bytes
// while owned_publication_bytes() reported zero.
void rule_budget_all_paths(std::size_t workers) {
    GraphLimits limits;
    limits.generation_budget_bytes = kBytes;
    limits.require_published_output = false;
    std::atomic<int> accepted{0};
    std::atomic<int> refused{0};
    DecodeTaskGraph graph(kGen, GraphProfile::Synthetic,
                          {TaskSpec{plane_key(0, 5, 0), {}, [&](const TaskContext& context) {
                               HeapPlane plane;
                               plane.fill(0);
                               if (context.publish(plane.data, kBytes)) accepted.fetch_add(1);
                               else refused.fetch_add(1);
                           }},
                           TaskSpec{plane_key(0, 5, 1), {}, [&](const TaskContext& context) {
                               HeapPlane plane;
                               plane.fill(1);
                               if (context.publish(plane.data, kBytes)) accepted.fetch_add(1);
                               else refused.fetch_add(1);
                           }}},
                          limits);
    const bool ran = graph.run(workers);
    // The first untyped publication fits the ceiling, the second does not.
    const std::size_t owned = graph.owned_publication_bytes();
    const bool ok = ran && accepted.load() == 1 && refused.load() == 1 && owned == kBytes;
    char detail[192];
    std::snprintf(detail, sizeof detail, "run=%d accepted=%d refused=%d owned=%zu budget=%zu",
                  ran ? 1 : 0, accepted.load(), refused.load(), owned, kBytes);
    required("T4-budget-all", ok,
             ok ? "the second untyped publication was refused by the same ceiling; owned bytes match"
                : std::string(detail));
}

// RELEASE-ABANDONED: a stopped generation retains nothing. A node that published
// and then failed never completed, so fail_locked() marked its dependents Failed and
// no consumer was ever handed the storage. After run() joins its workers the bytes
// are released and the charge is refunded, instead of living until destruction.
void rule_release_abandoned(std::size_t workers) {
    GraphLimits limits;
    limits.require_published_output = false;
    std::atomic<int> published{0};
    DecodeTaskGraph graph(
        kGen, GraphProfile::Synthetic,
        {TaskSpec{plane_key(0, 5, 0), {}, [&](const TaskContext& context) {
             HeapPlane plane;
             plane.fill(0);
             if (context.publish_plane(plane.data, kBytes, kWidth, kHeight, kPitch))
                 published.fetch_add(1);
             // A publisher that dies after publishing: its output exists but the
             // node never completes, so nothing may consume it.
             throw std::runtime_error("synthetic post-publication failure");
         }},
         TaskSpec{plane_key(0, 4, 0), {plane_key(0, 5, 0)}, [&](const TaskContext&) {
             throw std::runtime_error("consumer must never run");
         }}},
        limits);
    const bool ran = graph.run(workers);
    const std::size_t owned = graph.owned_publication_bytes();
    const bool consumer_held = graph.state(1) != TaskState::Completed;
    const bool ok = !ran && published.load() == 1 && consumer_held && owned == 0 &&
                    graph.error() != GraphError::None;
    char detail[192];
    std::snprintf(detail, sizeof detail, "run=%d published=%d owned=%zu error=%d consumer_state=%d",
                  ran ? 1 : 0, published.load(), owned, static_cast<int>(graph.error()),
                  static_cast<int>(graph.state(1)));
    required("T4-release-abandoned", ok,
             ok ? "a completed-then-failed publication was released and its charge refunded"
                : std::string(detail));
}

// S3-ALIGN-ADMIT: typed admission requires the alignment instead of assuming it.
// A hand-built BufferVersion naming an unaligned address is rejected at admission,
// not admitted into a view the consumer would later refuse to read.
void rule_admission_alignment() {
    HeapPlane storage;
    storage.fill(0);
    PlaneEdge claimed;
    claimed.generation = kGen;
    claimed.channel = 0;
    claimed.wavelet = 5;
    claimed.band = 0;
    claimed.plane.data = storage.data;
    claimed.plane.width = kWidth;
    claimed.plane.height = kHeight;
    claimed.plane.pitch = kPitch;
    claimed.plane.bytes = kBytes;

    BufferVersion aligned;
    aligned.generation = kGen;
    aligned.version = 1;
    aligned.data = storage.data;
    aligned.bytes = kBytes;
    const AdmittedPlane accepted = admit_owned_plane(aligned, claimed, plane_key(0, 5, 0), kGen);

    BufferVersion shifted;
    shifted.generation = kGen;
    shifted.version = 1;
    // One sample off: still inside the buffer, no longer 32-byte aligned.
    shifted.data = reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(storage.data) +
                                                 sizeof(std::int16_t));
    shifted.bytes = kBytes;
    const AdmittedPlane rejected = admit_owned_plane(shifted, claimed, plane_key(0, 5, 0), kGen);

    const bool ok = accepted.ok && !rejected.ok && rejected.plane.data == nullptr;
    char detail[160];
    std::snprintf(detail, sizeof detail, "aligned_ok=%d unaligned_ok=%d",
                  accepted.ok ? 1 : 0, rejected.ok ? 1 : 0);
    required("S3-align-admit", ok,
             ok ? "an aligned owned span is admitted; an unaligned one is rejected"
                : std::string(detail));
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t workers = 1;
    if (argc > 1) workers = static_cast<std::size_t>(std::strtoul(argv[1], nullptr, 10));
    if (workers == 0 || workers > 16) {
        std::printf("usage: %s [workers 1..16]\n", argv[0]);
        return 2;
    }
    std::printf("INFO workers=%zu synthetic T4 plane dependency only; issue 195 races are not in this gate\n",
                workers);
    rule_dependency_order(workers);
    rule_premature_access(workers);
    rule_premature_free(workers);
    rule_extent(workers);
    rule_budget(workers);
    rule_key(workers);
    rule_identity(workers);
    rule_stale(workers);
    rule_geometry(workers);
    rule_missing(workers);
    rule_timeout(workers);
    rule_abort(workers);
    rule_cancel(workers);
    rule_cancel_stale(workers);
    rule_owned_alignment(workers);
    rule_release(workers);
    rule_budget_all_paths(workers);
    rule_release_abandoned(workers);
    rule_admission_alignment();
    if (failures != 0) {
        std::printf("SCHEDULER-T4 FAIL workers=%zu holds=%d failures=%d\n", workers, holds, failures);
        return 1;
    }
    std::printf("SCHEDULER-T4 PASS workers=%zu holds=%d failures=0\n", workers, holds);
    return 0;
}
