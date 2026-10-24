// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

// Reuse only the already reviewed offline adapter's aligned file/row helpers.
// Its Phase-A entry point is not called; only the new stages are compared.
#define main Gop2PhaseAComparisonUnusedMain
#include "gop2_candidate_compare.cpp"
#undef main

namespace {
int CompareNewStages(const fs::path& packet_root, const fs::path& stage_root,
                     const fs::path& output, int w, int h) {
    Require(!fs::exists(output)); fs::create_directories(output);
    const auto packet = Read(packet_root / "original_sdk.cfhd");
    std::ofstream report(output / "stage_comparison.json"); Require(bool(report));
    report << "{\"schema\":\"hypercine.gop2.stage_comparison/1\","
        << "\"candidate_sha\":\"7ed4a82ed97598454c2680bd264dedf259e6e7dc\","
        << "\"width\":" << w << ",\"height\":" << h << ",\"comparisons\":[\n";
    int compared = 0, mismatched = 0;
    auto compare = [&](const Bytes& actual, const std::string& reference_id, int frame, int ch,
                       int logical, int wavelet, int band, int width) {
        const auto expected = Read(stage_root / reference_id);
        const auto name = "frame" + std::to_string(frame) + "_" + reference_id;
        Save(output / name, actual);
        const bool equal = actual == expected;
        size_t first = 0, differences = 0;
        const size_t common = (std::min)(expected.size(), actual.size()) / 2;
        const auto* got = reinterpret_cast<const int16_t*>(actual.data());
        const auto* want = reinterpret_cast<const int16_t*>(expected.data());
        while (first < common && got[first] == want[first]) ++first;
        for (size_t i = 0; i < common; ++i) differences += got[i] != want[i];
        if (compared++) report << ",\n";
        mismatched += !equal;
        report << "{\"artifact\":\"" << name << "\",\"reference_id\":\"" << reference_id
            << "\",\"frame_index\":" << frame << ",\"channel\":" << ch
            << ",\"logical_band\":" << logical << ",\"wavelet\":" << wavelet << ",\"band\":" << band
            << ",\"width\":" << width << ",\"equal\":" << (equal ? "true" : "false")
            << ",\"actual_bytes\":" << actual.size() << ",\"expected_bytes\":" << expected.size()
            << ",\"differing_coefficients\":" << differences;
        if (!equal) report << ",\"first_coefficient\":" << first << ",\"row\":" << first/width
            << ",\"column\":" << first%width << ",\"got\":" << (first < actual.size()/2 ? got[first] : 0)
            << ",\"expected\":" << (first < expected.size()/2 ? want[first] : 0);
        report << '}';
    };
    HyperCine::DecoderConfig config;
    config.maximum_width = w; config.maximum_height = h; config.slot_count = 1; config.host_worker_count = 1;
    HyperCine::Decoder decoder(config); Require(decoder.Initialize() == HyperCine::DecodeError::None);
    constexpr int owners[17] = {5,5,5,5,4,4,4,3,3,3,3,1,1,1,0,0,0};
    constexpr int bands[17] = {0,1,2,3,1,2,3,0,1,2,3,1,2,3,1,2,3};
    constexpr int candidate_plane_for_sdk_channel[3] = {0,2,1};
    for (int f = 0; f < 2; ++f) {
        HyperCine::DecodeRequest request;
        request.frame_index = f; request.format = HyperCine::DecodePixelFormat::PlanarYUV422Signed16;
        request.orientation = HyperCine::DecodeOrientation::TopDown;
        const auto ticket = decoder.QueueDecode(packet.data(), packet.size(), request); Require(bool(ticket));
        Require(decoder.WaitForDecode(ticket).state == HyperCine::DecodeState::Ready);
        HyperCine::DeviceOutput view;
        Require(decoder.GetDeviceOutput(ticket, &view) && view.width == w && view.height == h && view.plane_count == 3 &&
            view.orientation == HyperCine::DecodeOrientation::TopDown);
        for (int ch = 0; ch < 3; ++ch) {
            for (int logical = 0; logical < 17; ++logical) {
                // Explicitly compare each frame's materialized input bands; all
                // 51 unique original bands are covered across the two requests.
                if ((f == 0 && logical >= 11 && logical <= 13) || (f == 1 && logical >= 14)) continue;
                std::vector<int16_t> values; int width = 0, height = 0, pitch = 0;
                Require(HyperCine::DecoderInspector::DownloadFieldPlusBand(decoder, ticket, ch, logical,
                    values, &width, &height, &pitch));
                compare(Compact(values,width,height,pitch), "pre_ch" + std::to_string(ch) + "_band" +
                    std::to_string(logical) + ".i16le", f,ch,logical,owners[logical],bands[logical],width);
            }
            const int width = ch == 0 ? w : w/2;
            const auto& plane = view.planes[candidate_plane_for_sdk_channel[ch]];
            Require(plane.width == width && plane.height == h);
            Bytes values(size_t(width) * h * 2);
            Require(cudaMemcpy2D(values.data(),width*2,plane.device_pointer,plane.pitch_bytes,
                width*2,h,cudaMemcpyDeviceToHost) == cudaSuccess);
            // No clipping, shifting, packing or recomputation of final values.
            compare(values,"signed_frame" + std::to_string(f) + "_ch" + std::to_string(ch) + ".i16le",
                f,ch,-1,-1,-1,width);
        }
        Require(decoder.ReleaseOutput(ticket));
    }
    report << "\n],\"compared\":" << compared << ",\"mismatched\":" << mismatched
        << ",\"unique_pre_inverse_band_oracles\":51,\"pre_inverse_band_comparisons\":84,\"signed_plane_comparisons\":6,"
        << "\"release_gate\":\"OWNER_REVIEW_REQUIRED\",\"candidate_capture\":\"unmodified_entropy_destinations_and_direct_signed_device_planes\","
        << "\"not_materialized_per_channel\":{\"frame0_logical_bands\":[11,12,13],\"frame1_logical_bands\":[14,15,16]}}\n";
    report.close(); Require(bool(report) && compared == 90);
    std::cout << "new original stages " << w << 'x' << h << " compared=" << compared << " mismatched=" << mismatched << '\n';
    return mismatched ? 2 : 0;
}
}
int main(int argc, char** argv) {
    try {
        Require(argc == 6);
        const int w = std::stoi(argv[4]), h = std::stoi(argv[5]);
        Require((w == 1920 && h == 1080) || (w == 3840 && h == 2160));
        return CompareNewStages(argv[1],argv[2],argv[3],w,h);
    } catch (const std::exception&) {
        std::cerr << "new-stage comparison failed; preserve partial report; gate BLOCKED\n";
        return 1;
    }
}
