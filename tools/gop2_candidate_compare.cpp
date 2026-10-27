// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

// Offline comparison against already pinned original-SDK bytes. This process
// links only the candidate decoder and never generates its own reference.
#include "hypercine/decoder.h"
#include "hypercine/research/decoder_diagnostics.h"
#include "decode_fieldplus.h"
#include "test_vectors.h"
#include <cuda_runtime.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
namespace fs = std::filesystem;
namespace {
using Bytes = HyperCineTest::AlignedVector<uint8_t>;
void Require(bool ok) { if (!ok) throw std::runtime_error("comparison operation failed"); }
Bytes Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    Require(bool(in)); auto size = in.tellg(); Require(size > 0 && size < 256 * 1024 * 1024);
    Bytes data(static_cast<size_t>(size));
    in.seekg(0); in.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
    Require(bool(in)); return data;
}
void Save(const fs::path& path, const Bytes& data) {
    Require(!fs::exists(path)); std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    out.close(); Require(bool(out));
}
Bytes Compact(const std::vector<int16_t>& input, int width, int height, int pitch) {
    Require(width > 0 && height > 0 && pitch >= width * 2 && pitch % 2 == 0);
    Require(input.size() >= size_t(pitch / 2) * height);
    Bytes result(size_t(width) * height * 2);
    for (int y = 0; y < height; ++y)
        std::memcpy(result.data() + size_t(y) * width * 2,
            input.data() + size_t(y) * (pitch / 2), size_t(width) * 2);
    return result;
}
struct Reporter {
    fs::path output; std::ofstream json; bool first = true; int mismatches = 0, compared = 0;
    Reporter(const fs::path& root, int w, int h) : output(root), json(root / "comparison.json") {
        Require(bool(json));
        json << "{\"schema\":\"hypercine.gop2.comparison/1\",\"candidate_sha\":\"7ed4a82ed97598454c2680bd264dedf259e6e7dc\","
            << "\"width\":" << w << ",\"height\":" << h << ",\"comparisons\":[\n";
    }
    void Compare(const Bytes& expected, const Bytes& actual, const std::string& id,
                 int frame, int channel, int wavelet, int band, int width, int element_bytes) {
        const std::string artifact = "frame" + std::to_string(frame) + "_" + id + ".bin";
        Save(output / artifact, actual);
        size_t first_difference = 0, differences = 0;
        const size_t common = (std::min)(expected.size(), actual.size());
        while (first_difference < common && expected[first_difference] == actual[first_difference]) ++first_difference;
        for (size_t i = 0; i < common; ++i) differences += expected[i] != actual[i];
        const bool equal = expected == actual;
        mismatches += !equal; ++compared;
        if (!first) json << ",\n"; first = false;
        json << "{\"id\":\"" << id << "\",\"artifact\":\"" << artifact << "\",\"frame_index\":" << frame
            << ",\"channel\":" << channel << ",\"wavelet\":" << wavelet << ",\"band\":" << band
            << ",\"equal\":" << (equal ? "true" : "false") << ",\"expected_bytes\":" << expected.size()
            << ",\"actual_bytes\":" << actual.size() << ",\"differing_bytes\":" << differences;
        if (!equal) json << ",\"first_byte\":" << first_difference << ",\"row\":" << first_difference / (width * element_bytes)
            << ",\"column\":" << (first_difference / element_bytes) % width;
        json << '}';
    }
    int Finish() {
        json << "\n],\"compared\":" << compared << ",\"mismatched\":" << mismatches
            << ",\"topology\":{\"parser\":\"candidate_strict_FIELDPLUS_parser\",\"chunks\":54,\"coefficient_bands\":51,\"control_records\":3},"
            << "\"not_materialized_per_channel\":{\"frame0_logical_bands\":[11,12,13],\"frame1_logical_bands\":[14,15,16]},"
            << "\"release_gate\":\"BLOCKED\",\"missing\":[\"pre_inverse_entropy_oracle\",\"final_signed_planar_oracle\"]}\n";
        json.close(); Require(bool(json));
        std::cout << "compared=" << compared << " mismatched=" << mismatches << " gate=BLOCKED\n";
        return mismatches ? 2 : 0;
    }
};
int Run(const fs::path& references, const fs::path& output, int w, int h) {
    Require(!fs::exists(output)); fs::create_directories(output);
    const auto packet = Read(references / "original_sdk.cfhd");
    HyperCine::FieldPlusSampleDescriptor topology;
    std::string error;
    Require(HyperCine::ParseProgressiveFieldPlusSampleValidated(packet.data(), packet.size(), 0, topology, &error));
    Reporter report(output, w, h);
    HyperCine::DecoderConfig config;
    config.maximum_width = w; config.maximum_height = h; config.slot_count = 1; config.host_worker_count = 1;
    HyperCine::Decoder decoder(config);
    Require(decoder.Initialize() == HyperCine::DecodeError::None);
    for (int frame = 0; frame < 2; ++frame) for (int fmt = 0; fmt < 2; ++fmt) {
        HyperCine::DecodeRequest request;
        request.frame_index = frame;
        request.orientation = HyperCine::DecodeOrientation::BottomUp; // SDK BGRA storage.
        request.format = fmt == 0 ? HyperCine::DecodePixelFormat::DeviceBGRA8 : HyperCine::DecodePixelFormat::PlanarYUV422Signed16;
        auto ticket = decoder.QueueDecode(packet.data(), packet.size(), request);
        Require(bool(ticket));
        Require(decoder.WaitForDecode(ticket).state == HyperCine::DecodeState::Ready);
        HyperCine::DeviceOutput view;
        Require(decoder.GetDeviceOutput(ticket, &view) && view.width == w && view.height == h);
        if (fmt == 0) {
            Require(view.plane_count == 1 && view.orientation == HyperCine::DecodeOrientation::BottomUp);
            Bytes actual(size_t(w) * h * 4);
            Require(cudaMemcpy2D(actual.data(), w * 4, view.planes[0].device_pointer, view.planes[0].pitch_bytes,
                w * 4, h, cudaMemcpyDeviceToHost) == cudaSuccess);
            report.Compare(Read(references / ("BGRA_call0_frame" + std::to_string(frame) + ".raw")), actual,
                "BGRA", frame, -1, -1, -1, w, 4);
        } else {
            Require(view.plane_count == 3 && view.orientation == HyperCine::DecodeOrientation::TopDown);
            std::array<std::vector<int16_t>, 3> planes;
            for (int ch = 0; ch < 3; ++ch) {
                const int width = ch == 0 ? w : w / 2;
                planes[ch].resize(size_t(width) * h);
                Require(cudaMemcpy2D(planes[ch].data(), width * 2, view.planes[ch].device_pointer,
                    view.planes[ch].pitch_bytes, width * 2, h, cudaMemcpyDeviceToHost) == cudaSuccess);
            }
            // Explicitly compare clipped packed YU64, not an alleged signed-plane oracle.
            Bytes packed(size_t(w) * h * 4);
            auto* words = reinterpret_cast<uint16_t*>(packed.data());
            auto pack = [](int16_t v) { return uint16_t((std::max)(0, (std::min)(1023, int(v))) * 64); };
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; x += 2) {
                const size_t i = (size_t(y) * w + x) * 2;
                words[i] = pack(planes[0][size_t(y)*w+x]); words[i+2] = pack(planes[0][size_t(y)*w+x+1]);
                words[i+1] = pack(planes[2][size_t(y)*w/2+x/2]); words[i+3] = pack(planes[1][size_t(y)*w/2+x/2]);
            }
            report.Compare(Read(references / ("YU64_call0_frame" + std::to_string(frame) + ".raw")), packed,
                "YU64_clipped", frame, -1, -1, -1, w, 4);
            constexpr int owners[17] = {5,5,5,5,4,4,4,3,3,3,3,1,1,1,0,0,0};
            constexpr int bands[17] = {0,1,2,3,1,2,3,0,1,2,3,1,2,3,1,2,3};
            for (int ch = 0; ch < 3; ++ch) {
                for (int b = 0; b < 17; ++b) {
                    // The other frame's final highpass bands are not materialized by this request.
                    if ((frame == 0 && b >= 11 && b <= 13) || (frame == 1 && b >= 14 && b <= 16)) continue;
                    int width = 0, height = 0, pitch = 0; std::vector<int16_t> values;
                    Require(HyperCine::DecoderInspector::DownloadFieldPlusBand(decoder, ticket, ch, b,
                        values, &width, &height, &pitch));
                    const std::string id = "ch" + std::to_string(ch) + "_resident_band" + std::to_string(b);
                    report.Compare(Read(references / "stages" / (id + ".i16le")), Compact(values,width,height,pitch),
                        id, frame, ch, owners[b], bands[b], width, 2);
                }
                std::vector<int16_t> low, high, ll, base; int width = 0, height = 0;
                Require(HyperCine::DecoderInspector::DownloadFieldPlusTemporalStages(decoder, ticket, ch,
                    low, high, ll, base, &width, &height));
                const std::array<std::pair<std::string, const std::vector<int16_t>*>, 4> stages{{
                    {"wavelet4_ll", &ll}, {"temporal_low", &low}, {"temporal_high", &high},
                    {"frame" + std::to_string(frame) + "_base_ll", &base}}};
                for (const auto& stage : stages) {
                    const std::string id = "ch" + std::to_string(ch) + "_" + stage.first;
                    const auto* bytes = reinterpret_cast<const uint8_t*>(stage.second->data());
                    report.Compare(Read(references / "stages" / (id + ".i16le")), Bytes(bytes,bytes+stage.second->size()*2),
                        id, frame, ch, -1, -1, stage.first == "wavelet4_ll" ? width/2 : width, 2);
                }
            }
        }
        Require(decoder.ReleaseOutput(ticket));
    }
    return report.Finish();
}
}
int main(int argc, char** argv) {
    try {
        Require(argc == 5);
        const int w = std::stoi(argv[3]), h = std::stoi(argv[4]);
        Require((w == 1920 && h == 1080) || (w == 3840 && h == 2160));
        return Run(argv[1], argv[2], w, h);
    } catch (const std::exception&) {
        std::cerr << "candidate comparison failed; preserve partial report; gate BLOCKED\n";
        return 1;
    }
}
