// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "../tests/decode_phase3a_rc_support.h"
#include "hypercine/research/decode_phase2e.h"
#include <fstream>
using namespace DecodeRC;
int main(int argc, char** argv) {
    try {
        Require(argc == 2, "stage output JSON path required");
        auto b = HyperCineTest::GenerateTestVectorYUY2(HyperCineTest::PATTERN_PSEUDO_RANDOM_NOISE, 1920, 1080);
        auto s = EncodeInput(b, 1920, 1080, CFHD_PIXEL_FORMAT_YUY2, CFHD_ENCODING_QUALITY_FILMSCAN3, 15, CFHD_ENCODING_FLAGS_YUV_INTERLACED);
        Require(Hash(s.compressed.data(), s.compressed.size()) == "356ad5c204104b7d", "frozen stage sample");
        ReferenceDecoder reference(s, CFHD_PIXEL_FORMAT_BGRA); reference.Decode(s);
        Require(Hash(reference.output.data(), reference.output.size()) == "41a4e58b09555e3e", "frozen upstream output");
        CudaDecoderPipelinePhase2E pipe; Require(pipe.Initialize(1920, 1080, 2, 10, 8, 4), "stage pipeline init");
        pipe.PrepareProductionScratch(); pipe.SetPhase2FCSelectorPolicy(Phase2FCSelectorPolicy::AdaptiveDynamic);
        std::vector<GpuEntropyBandDescriptor> parsed; FrameReconstruction reconstruction;
        Require(ParseCineFormSampleValidated(s.compressed.data(), s.compressed.size(), 24, parsed, nullptr, nullptr, &reconstruction)
            && reconstruction == FrameReconstruction::InterlacedGop1, "stage profile");
        DecodeFrameInfo info; const auto automatic = ClassifyDecodeFrame(parsed, info);
        std::ofstream out(argv[1]); out << "{\"license\":\"MIT OR Apache-2.0\",\"sample_hash\":\"356ad5c204104b7d\",\"field_timing\":\"sum of three channel kernels\",\"cases\":[";
        const char* names[] = {"Lightweight", "Hybrid", "Auto"};
        for (int k = 0; k < 3; ++k) {
            const auto strategy = k == 2 ? automatic : k == 0 ? DecodeStrategy::Lightweight : DecodeStrategy::Hybrid;
            pipe.SetPipelineMode(strategy == DecodeStrategy::Hybrid ? Phase2EPipelineMode::Phase2FCHybridFullFrame : Phase2EPipelineMode::Phase2DFrameBarrier);
            double field = 0, color = 0, critical = 0; constexpr int count = 100; int ac_status = -1;
            for (int r = -12; r < count; ++r) {
#ifdef _WIN32
                SYSTEM_POWER_STATUS power{}; Require(GetSystemPowerStatus(&power) && power.ACLineStatus == 1, "stage benchmark requires AC power");
                ac_status = int(power.ACLineStatus);
#endif
                auto t = pipe.QueueDecodeParsed(s.compressed.data(), s.compressed.size(), parsed, DeviceSurfaceType::BGRA8,
                    ColorMatrix::BT709, ColorRange::CG, RowOrientation::BottomUp, true, 0, true);
                Require(t && pipe.WaitForDecode(t), "stage decode ready");
                if (r == -1) { Bytes actual(reference.output.size()); Require(pipe.DownloadBgraToHost(t, actual.data(), 1920 * 4) && actual == reference.output, "stage output exact"); }
                if (r >= 0) { Phase2eSlotStageTimings timings; Require(pipe.GetStageTimings(t, &timings), "stage timings");
                    field += timings.field_combine_ms; color += timings.color_convert_ms; critical += timings.total_critical_path_ms; }
                pipe.ReleaseOutput(t);
            }
            if (k) out << ',';
            out << "{\"strategy\":\"" << names[k] << "\",\"actual_strategy\":\"" << StrategyName(strategy) << "\",\"frames\":" << count
                << ",\"field_combine_avg_ms\":" << field / count << ",\"color_convert_avg_ms\":" << color / count
                << ",\"pipeline_critical_path_avg_ms\":" << critical / count << ",\"ac_line_status\":" << ac_status << ",\"bgra_exact\":true}";
            std::cout << names[k] << " field=" << field / count << " color=" << color / count << " ms\n" << std::flush;
        }
        out << "]}\n"; Require(bool(out), "stage evidence write"); return 0;
    } catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }
}
