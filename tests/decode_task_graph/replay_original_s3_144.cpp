// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// Test-only transport of supplied signed coefficients through the existing S3
// attachment. Original authenticity/equality belongs to the Python verifier.
#include "decode_s3_attachment.h"
#include "decode_spatial_scalar.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace hypercine::detail;
namespace {
constexpr Generation generation{144, 292, 1};
void require(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}
unsigned number(const char* text) {
    const std::string s(text);
    require(!s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
        return c >= '0' && c <= '9'; }), "invalid integer");
    const auto value = std::stoul(s);
    require(value <= std::numeric_limits<unsigned>::max(), "integer overflow");
    return static_cast<unsigned>(value);
}
void read_compact(const char* name, MutableSignedPlaneView plane) {
    std::ifstream file(name, std::ios::binary | std::ios::ate);
    require(file && file.tellg() == std::streamoff(std::size_t(plane.width) * plane.height * 2),
            "input byte length");
    file.seekg(0);
    for (unsigned y = 0; y < plane.height; ++y) for (unsigned x = 0; x < plane.width; ++x) {
        unsigned char bytes[2];
        require(bool(file.read(reinterpret_cast<char*>(bytes), 2)), "input read");
        const int value = int(bytes[0]) + 256 * int(bytes[1]);
        plane.data[y * plane.pitch / 2 + x] =
            static_cast<std::int16_t>(value >= 32768 ? value - 65536 : value);
    }
}
int replay(int argc, char** argv) {
    require(argc == 12 && std::string(argv[1]) == "--replay", "arguments");
    const unsigned w = number(argv[2]), h = number(argv[3]);
    const unsigned channel = number(argv[8]), workers = number(argv[10]);
    require(w >= 16 && w <= 480 && w % 8 == 0 && h >= 4 && h <= 270, "geometry");
    require(channel <= 2 && (workers == 1 || workers == 2 || workers == 4 || workers == 16),
            "channel or workers");
    const std::string mode(argv[11]);
    require(mode == "normal" || mode == "wrong-band" || mode == "wrong-channel", "mode");
    std::array<OwnedSignedPlane, 4> inputs{
        OwnedSignedPlane(w,h), OwnedSignedPlane(w,h), OwnedSignedPlane(w,h), OwnedSignedPlane(w,h)};
    OwnedSignedPlane output(w * 2, h * 2);
    for (unsigned b = 0; b < 4; ++b) read_compact(argv[4 + b], inputs[b].view());
    auto consumer = MakeSpatialLowpassTask(generation, channel, w, h, inputs[0].view().pitch);
    if (mode == "wrong-band") std::swap(consumer.inputs[0], consumer.inputs[1]);
    // All foreign identities are legal channels: Y->V, V->U and U->V.
    // Both directions of the chroma swap must reach the consumer guard.
    const unsigned source_channel = mode == "wrong-channel" ? (channel == 0 ? 1 : 3 - channel) : channel;
    if (mode == "wrong-channel") for (auto& key : consumer.inputs) key.channel = source_channel;
    DecodeTaskGraph* running = nullptr;
    unsigned sink_entries = 0;
    std::vector<TaskSpec> tasks;
    for (unsigned b : std::array<unsigned, 4>{{1, 2, 3, 0}}) {
        tasks.push_back({{source_channel,0,5,0,b}, {}, [&, b](const TaskContext& ctx) {
            auto plane = inputs[b].view();
            if (b == 0) require(running->state(4) == TaskState::Pending,
                                "consumer admitted before LL completion");
            require(ctx.publish_plane(plane.data, plane.bytes, w, h, plane.pitch), "input publication");
            // Destroy the caller's visible values after the graph owns its copy.
            // The replay must still use the authenticated original coefficients.
            std::fill_n(plane.data, plane.bytes / 2, static_cast<std::int16_t>(-12345));
        }, [] { return true; }});
    }
    tasks.push_back(std::move(consumer));
    tasks.push_back({{channel,1,0,0,0}, {{channel,0,4,0,0}}, [&](const TaskContext& ctx) {
        ++sink_entries;
        auto target = output.view();
        PlaneIdentity expected{generation,channel,4,0,0,0,target.width,target.height,target.pitch};
        const auto result = ctx.input_plane(0, expected);
        require(result.valid() && result.version == 1 &&
                result.sample == PlaneSample::Signed16LittleEndian, "output identity");
        for (unsigned y = 0; y < target.height; ++y) for (unsigned x = 0; x < target.width; ++x) {
            // Graph-owned storage contains bytes, not live int16_t objects.
            std::memcpy(target.data + y * target.pitch / 2 + x,
                        static_cast<const unsigned char*>(result.data) + y * result.pitch + x * 2, 2);
        }
        const unsigned char token = 1;
        require(ctx.publish(&token, 1), "sink publication");
    }, [] { return true; }});
    DecodeTaskGraph graph(generation, GraphProfile::Synthetic, std::move(tasks));
    running = &graph;
    const bool ok = graph.run(workers);
    require(graph.quiescent(), "graph did not drain");
    if (mode != "normal") {
        require(!ok && sink_entries == 0 && graph.error() == GraphError::Callback,
                "identity control did not reject before output access");
        require(graph.state(5) != TaskState::Ready && graph.state(5) != TaskState::Completed,
                "invalid successor became ready");
        std::cout << "S3_IDENTITY_REJECTED no_output_access=1 drained=1 source_channel="
                  << source_channel << "\n";
        return 2; // A controlled rejection, distinct from an accidental failure.
    }
    require(ok && sink_entries == 1 && graph.state(4) == TaskState::Completed &&
            graph.state(5) == TaskState::Completed, "incomplete graph");
    const std::filesystem::path directory(argv[9]);
    require(std::filesystem::create_directory(directory), "fresh output required");
    std::ofstream stream(directory / "stage.i16le", std::ios::binary);
    const auto plane = output.view();
    for (unsigned y = 0; y < plane.height; ++y) for (unsigned x = 0; x < plane.width; ++x) {
        int value = plane.data[y * plane.pitch / 2 + x];
        if (value < 0) value += 65536;
        const unsigned char bytes[2] = {static_cast<unsigned char>(value % 256),
                                       static_cast<unsigned char>(value / 256)};
        stream.write(reinterpret_cast<const char*>(bytes), 2);
    }
    stream.close(); require(bool(stream), "output write");
    std::cout << "S3_GRAPH_OUTPUT_ONLY drained=1; original equality requires verified harness\n";
    return 0;
}
} // namespace
int main(int argc, char** argv) {
    try { return replay(argc, argv); }
    catch (...) { std::cerr << "S3_REPLAY_FAILURE details_redacted=1\n"; return 1; }
}
