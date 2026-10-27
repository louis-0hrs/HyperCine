// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Synthetic S3 attachment (issues #144 / #195). Not a decoder and not a
// pixel oracle. Rules, in order:
//   1-3  one positive buffer/stride plane, one short-extent rejection
//   4    channel/band mapping (V is not U; band 1 is not band 0)
//   5    bounded inverse-wavelet consumer, reject and pre-cancel
//   6    generation, ownership and cancellation via the S2 scheduler

#include "decode_s3_consumer.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
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

const Generation kGen{195, 1, 1};
constexpr std::uint32_t kWidth = 16;
constexpr std::uint32_t kHeight = 4;
constexpr std::size_t kPitch = 64;
constexpr std::size_t kBytes = kPitch * kHeight;
constexpr std::size_t kSamples = kBytes / sizeof(std::int16_t);
constexpr std::uint32_t kOutWidth = 32;
constexpr std::uint32_t kOutHeight = 8;
constexpr std::size_t kOutPitch = 64;
constexpr std::size_t kOutBytes = kOutPitch * kOutHeight;
constexpr std::size_t kOutSamples = kOutBytes / sizeof(std::int16_t);

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

struct alignas(32) HostPlane {
    std::int16_t samples[kSamples]{};
};
struct alignas(32) HostOutput {
    std::int16_t samples[kOutSamples]{};
};

SignedPlaneDesc desc_of(const HostPlane& plane, std::size_t bytes = kBytes) {
    return SignedPlaneDesc{plane.samples, kWidth, kHeight, kPitch, bytes};
}

PlaneEdge edge_of(const HostPlane& plane, std::uint32_t band, std::uint32_t channel = 0,
                  std::size_t bytes = kBytes) {
    return PlaneEdge{kGen, channel, /*wavelet=*/5, band, desc_of(plane, bytes)};
}

TaskKey input_key(std::uint32_t channel, std::uint32_t band) {
    return TaskKey{channel, /*transform=*/0, /*level=*/5, /*frame=*/0, band};
}

void rule_positive_buffer_stride() {
    HostPlane storage;
    const SignedPlaneDesc plane = desc_of(storage);
    const PlaneStatus status = check_signed_plane(plane);
    const std::size_t packed = std::size_t(kWidth) * sizeof(std::int16_t);
    const std::size_t last =
        (std::size_t(kHeight) - 1) * (kPitch / sizeof(std::int16_t)) + (kWidth - 1);
    storage.samples[last] = 0x5A5A;
    const bool indexed_inside = last < kSamples && storage.samples[last] == static_cast<std::int16_t>(0x5A5A);
    required("S3-buffer-stride",
             status == PlaneStatus::Ok && kPitch > packed && kPitch % 32 == 0 &&
                 kBytes == kPitch * kHeight && indexed_inside,
             "16x4 pitch 64 bytes (padded, 32-aligned) covering 256 bytes is one plane");
}

void rule_negative_short_extent() {
    HostPlane storage;
    const SignedPlaneDesc plane = desc_of(storage, kBytes - sizeof(std::int16_t));
    required("S3-extent-negative",
             check_signed_plane(plane) == PlaneStatus::BadExtent,
             "bytes 254 < pitch*height 256 is BadExtent and not a plane");
}

void rule_channel_band() {
    HostPlane storage;
    const PlaneEdge y0 = edge_of(storage, /*band=*/0, /*channel=*/0);
    const bool match = match_plane_edge(y0, input_key(0, 0), kGen) == EdgeStatus::Match;
    PlaneEdge band1 = y0;
    band1.band = 1;
    const bool band_miss = match_plane_edge(band1, input_key(0, 0), kGen) == EdgeStatus::Band;
    PlaneEdge chroma_v = y0;
    chroma_v.channel = 1;
    const bool channel_miss =
        match_plane_edge(chroma_v, input_key(2, 0), kGen) == EdgeStatus::Channel;
    required("S3-channel-band",
             match && band_miss && channel_miss,
             "Y/band0 matches; band 1 does not satisfy band 0; V does not satisfy U");
}

std::array<PlaneEdge, 4> four_edges(const HostPlane bands[4], std::size_t short_band = 4) {
    std::array<PlaneEdge, 4> edges{};
    for (std::uint32_t band = 0; band < 4; ++band) {
        const std::size_t bytes = band == short_band ? kBytes - sizeof(std::int16_t) : kBytes;
        edges[band] = edge_of(bands[band], band, 0, bytes);
    }
    return edges;
}

OutputEdge output_edge(HostOutput& output) {
    return OutputEdge{kGen, 0, 4, 0,
                      MutablePlaneDesc{output.samples, kOutWidth, kOutHeight, kOutPitch, kOutBytes}};
}

bool sentinel_intact(const HostOutput& output) {
    for (std::int16_t sample : output.samples)
        if (sample != 0x1111) return false;
    return true;
}

void rule_consumer_complete() {
    HostPlane bands[4]{};
    HostOutput output;
    std::memset(output.samples, 0x11, sizeof output.samples);
    OutputEdge lowpass = output_edge(output);
    const ConsumeStatus status = consume_wavelet5_lowpass(four_edges(bands), lowpass);
    bool cleared = true;
    for (std::int16_t sample : output.samples)
        if (sample != 0) cleared = false;
    required("S3-consumer-complete",
             status == ConsumeStatus::Complete && cleared,
             "four zero wavelet5 planes produce a cleared wavelet4 lowpass");
}

void rule_consumer_reject_short() {
    HostPlane bands[4]{};
    HostOutput output;
    std::memset(output.samples, 0x11, sizeof output.samples);
    OutputEdge output_plane = output_edge(output);
    const ConsumeStatus status =
        consume_wavelet5_lowpass(four_edges(bands, /*short_band=*/2), output_plane);
    required("S3-consumer-reject-short",
             status == ConsumeStatus::Rejected && sentinel_intact(output),
             "a short band extent is rejected before any output store");
}

void rule_consumer_cancel() {
    HostPlane bands[4]{};
    HostOutput output;
    std::memset(output.samples, 0x11, sizeof output.samples);
    OutputEdge output_plane = output_edge(output);
    const ConsumeStatus status =
        consume_wavelet5_lowpass(four_edges(bands), output_plane, [] { return true; });
    required("S3-consumer-cancel",
             status == ConsumeStatus::Canceled && sentinel_intact(output),
             "cancel before the first write leaves the output sentinel unchanged");
}

void rule_generation_admission() {
    HostPlane storage;
    storage.samples[0] = 42;
    PlaneEdge claimed = edge_of(storage, 0);
    BufferVersion foreign;
    foreign.generation = Generation{195, 1, 2};
    foreign.version = 1;
    foreign.data = storage.samples;
    foreign.bytes = kBytes;
    const AdmittedPlane missed = admit_owned_plane(foreign, claimed, input_key(0, 0), kGen);

    BufferVersion short_span;
    short_span.generation = kGen;
    short_span.version = 1;
    short_span.data = storage.samples;
    short_span.bytes = kBytes - sizeof(std::int16_t);
    const AdmittedPlane short_view = admit_owned_plane(short_span, claimed, input_key(0, 0), kGen);

    BufferVersion owned;
    owned.generation = kGen;
    owned.version = 1;
    owned.data = storage.samples;
    owned.bytes = kBytes;
    const AdmittedPlane admitted = admit_owned_plane(owned, claimed, input_key(0, 0), kGen);
    const bool read_ok = admitted.ok && admitted.plane.data[0] == 42;
    required("S3-generation-admission",
             !missed.ok && missed.plane.data == nullptr && !short_view.ok && read_ok,
             "foreign generation and a short span are empty; a matching owned span is admitted");
}

std::int16_t pattern(std::uint32_t band, std::uint32_t x, std::uint32_t y) {
    return static_cast<std::int16_t>(band * 1000 + y * kWidth + x);
}

void fill_pattern(HostPlane& plane, std::uint32_t band) {
    const std::size_t stride = kPitch / sizeof(std::int16_t);
    for (std::uint32_t y = 0; y < kHeight; ++y)
        for (std::uint32_t x = 0; x < kWidth; ++x)
            plane.samples[y * stride + x] = pattern(band, x, y);
}

void rule_ownership_through_scheduler() {
    HostPlane caller[4];
    for (std::uint32_t band = 0; band < 4; ++band) fill_pattern(caller[band], band);
    HostOutput output;
    std::memset(output.samples, 0x11, sizeof output.samples);
    std::atomic<int> saw_owned{0};
    std::atomic<int> saw_caller{0};
    std::atomic<bool> published{false};
    const std::function<bool()> no_predicate;

    std::vector<TaskSpec> tasks;
    for (std::uint32_t band = 0; band < 4; ++band) {
        tasks.push_back(TaskSpec{
            input_key(0, band),
            {},
            [&, band](const TaskContext& context) {
                const bool ok = context.publish(caller[band].samples, kBytes);
                std::memset(caller[band].samples, 0x7F, sizeof caller[band].samples);
                if (!ok) saw_caller.fetch_add(1);
            },
            no_predicate});
    }
    tasks.push_back(TaskSpec{
        TaskKey{0, 0, 4, 0, 0},
        {input_key(0, 0), input_key(0, 1), input_key(0, 2), input_key(0, 3)},
        [&](const TaskContext& context) {
            HostPlane aligned[4];
            std::array<PlaneEdge, 4> bands{};
            for (std::uint32_t band = 0; band < 4; ++band) {
                context.wait_for_input(band, std::chrono::milliseconds(1000));
                const BufferVersion view = context.input(band);
                const PlaneEdge claimed = edge_of(caller[band], band);
                const AdmittedPlane admitted =
                    admit_owned_plane(view, claimed, input_key(0, band), kGen);
                if (!admitted.ok || admitted.plane.data == caller[band].samples ||
                    admitted.plane.bytes != kBytes) {
                    saw_caller.fetch_add(1);
                    return;
                }
                const std::size_t stride = admitted.plane.pitch / sizeof(std::int16_t);
                bool intact = true;
                for (std::uint32_t y = 0; y < kHeight && intact; ++y)
                    for (std::uint32_t x = 0; x < kWidth; ++x)
                        if (admitted.plane.data[y * stride + x] != pattern(band, x, y)) intact = false;
                if (!intact) {
                    saw_caller.fetch_add(1);
                    return;
                }
                // Research inverse requires 32-byte alignment. S2's owned
                // vector does not. Copy after the ownership check, then run.
                std::memcpy(aligned[band].samples, admitted.plane.data, kBytes);
                bands[band] = edge_of(aligned[band], band);
                saw_owned.fetch_add(1);
            }
            OutputEdge lowpass = output_edge(output);
            if (consume_wavelet5_lowpass(bands, lowpass,
                                         [&] { return context.cancellation_requested(); }) !=
                ConsumeStatus::Complete)
                return;
            published.store(context.publish(output.samples, kOutBytes));
        },
        no_predicate});

    DecodeTaskGraph graph(kGen, GraphProfile::Synthetic, std::move(tasks));
    const bool strict = graph.requires_published_output();
    const std::size_t uncontracted = graph.uncontracted_tasks();
    const bool ran = graph.run(2);
    bool all_completed = ran;
    bool all_published = ran;
    for (std::size_t node = 0; node < 5; ++node) {
        all_completed = all_completed && graph.state(node) == TaskState::Completed;
        all_published = all_published && DecodeTaskGraphTestAccess::published_output(graph, node);
    }
    const bool ok = strict && uncontracted == 5 && ran && graph.error() == GraphError::None &&
                   all_completed && all_published && saw_owned.load() == 4 && saw_caller.load() == 0 &&
                   published.load();
    char detail[256];
    std::snprintf(detail, sizeof detail,
                  "strict=%d uncontracted=%zu ran=%d error=%d completed=%d published_nodes=%d "
                  "owned=%d caller=%d publish=%d",
                  strict ? 1 : 0, uncontracted, ran ? 1 : 0, static_cast<int>(graph.error()),
                  all_completed ? 1 : 0, all_published ? 1 : 0, saw_owned.load(), saw_caller.load(),
                  published.load() ? 1 : 0);
    required("S3-ownership", ok,
             ok ? std::string("empty predicates complete only via graph-owned copies; caller scribble "
                              "is ignored")
                : std::string(detail));
}

void rule_empty_predicate_is_not_a_buffer() {
    const std::function<bool()> no_predicate;
    DecodeTaskGraph graph(kGen, GraphProfile::Synthetic,
                          {TaskSpec{input_key(0, 0), {}, [](const TaskContext&) {}, no_predicate}});
    const bool ran = graph.run(1);
    required("S3-empty-predicate-no-buffer",
             graph.requires_published_output() && graph.uncontracted_tasks() == 1 && !ran &&
                 graph.error() == GraphError::OutputNotPublished &&
                 !DecodeTaskGraphTestAccess::published_output(graph, 0) &&
                 graph.state(0) == TaskState::Failed,
             "an empty predicate does not complete a task that published no buffer");
}

void rule_cancel_reads_no_payload() {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
    std::atomic<int> consumer_ran{0};
    std::atomic<int> producer_published{0};
    const std::function<bool()> no_predicate;
    std::vector<TaskSpec> tasks;
    tasks.push_back(TaskSpec{
        input_key(0, 0),
        {},
        [&](const TaskContext& context) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                entered = true;
            }
            changed.notify_one();
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [&] { return release; });
            }
            if (context.cancellation_requested()) return;
            HostPlane local{};
            if (context.publish(local.samples, kBytes)) producer_published.fetch_add(1);
        },
        no_predicate});
    tasks.push_back(TaskSpec{
        TaskKey{0, 0, 4, 0, 0},
        {input_key(0, 0)},
        [&](const TaskContext&) { consumer_ran.fetch_add(1); },
        no_predicate});

    DecodeTaskGraph graph(kGen, GraphProfile::Synthetic, std::move(tasks));
    bool ran = true;
    std::thread worker([&] { ran = graph.run(1); });
    bool started = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        started = changed.wait_for(lock, std::chrono::seconds(2), [&] { return entered; });
    }
    const bool canceled = started && graph.cancel(kGen);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    changed.notify_all();
    worker.join();
    required("S3-cancel-no-payload",
             started && canceled && !ran && graph.error() == GraphError::Canceled &&
                 consumer_ran.load() == 0 && producer_published.load() == 0 &&
                 !DecodeTaskGraphTestAccess::published_output(graph, 0) &&
                 !DecodeTaskGraphTestAccess::published_output(graph, 1) &&
                 graph.state(1) != TaskState::Completed && graph.state(1) != TaskState::Ready,
             "cancel before publish runs no consumer and retains no plane");
}

}  // namespace

int main() {
    rule_positive_buffer_stride();
    rule_negative_short_extent();
    rule_channel_band();
    rule_consumer_complete();
    rule_consumer_reject_short();
    rule_consumer_cancel();
    rule_generation_admission();
    rule_ownership_through_scheduler();
    rule_empty_predicate_is_not_a_buffer();
    rule_cancel_reads_no_payload();
    if (failures != 0) {
        std::printf("DECODE-S3 FAIL holds=%d failures=%d\n", holds, failures);
        return 1;
    }
    std::printf("DECODE-S3 PASS holds=%d failures=0\n", holds);
    return 0;
}
