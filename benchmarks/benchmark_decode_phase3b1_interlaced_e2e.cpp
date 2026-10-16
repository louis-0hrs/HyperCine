// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "../tests/decode_phase3a_rc_support.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace ProductionDecodeTest;

namespace {
int AcStatus() {
#ifdef _WIN32
    SYSTEM_POWER_STATUS power{};
    Require(GetSystemPowerStatus(&power) != 0, "read AC power status");
    Require(power.ACLineStatus == 1, "interlaced benchmark requires AC power");
    return int(power.ACLineStatus);
#else
    return -1;
#endif
}
double Percentile95(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[size_t(std::ceil(0.95 * values.size())) - 1];
}
}
int main(int argc, char** argv) {
    try {
        Require(argc == 2, "output JSON path required");
        auto input = HyperCineTest::GenerateTestVectorYUY2(
            HyperCineTest::PATTERN_PSEUDO_RANDOM_NOISE, 1920, 1080);
        auto sample = DecodeRC::EncodeInput(input, 1920, 1080, CFHD_PIXEL_FORMAT_YUY2,
            CFHD_ENCODING_QUALITY_FILMSCAN3, 15, CFHD_ENCODING_FLAGS_YUV_INTERLACED);
        const uint64_t folded = (14695981039346656037ULL ^
            HyperCineTest::ComputeFNV1a64(input.data(), input.size())) * 1099511628211ULL;
        std::ostringstream source_hash_stream;
        source_hash_stream << std::hex << std::setfill('0') << std::setw(16) << folded;
        const std::string source_hash = source_hash_stream.str();
        Require(source_hash == "2d12a276f3594b09", "frozen source hash");
        Require(Hash(sample.compressed.data(), sample.compressed.size()) == "356ad5c204104b7d",
            "frozen compressed sample hash");

        DecoderConfig config;
        config.maximum_width = 1920;
        config.maximum_height = 1080;
        config.slot_count = 2;
        Decoder decoder(config);
        Require(decoder.Initialize() == DecodeError::None, "decoder init");
        DecodeRC::ReferenceDecoder reference(sample, CFHD_PIXEL_FORMAT_BGRA);
        reference.Decode(sample);
        const std::string expected_output_hash = Hash(reference.output.data(), reference.output.size());
        std::vector<uint8_t> downloaded(size_t(1920) * 1080 * 4);
        const DiagnosticDecodeStrategy strategies[] = {
            DiagnosticDecodeStrategy::Lightweight,
            DiagnosticDecodeStrategy::Hybrid,
            DiagnosticDecodeStrategy::Automatic
        };
        const char* names[] = {"Lightweight", "Hybrid", "Auto"};
        std::ofstream out(argv[1]);
        Require(bool(out), "open output JSON");
        out << "{\"license\":\"MIT OR Apache-2.0\",\"source_hash\":\"" << source_hash
            << "\",\"sample_hash\":\"356ad5c204104b7d\",\"output_hash\":\"" << expected_output_hash
            << "\",\"geometry\":\"1920x1080\",\"interlaced\":true,\"quality\":\"FilmScan3\","
            << "\"pixel_format\":\"YUY2\",\"timestamp_scope\":\"QueueDecode->WaitForDecode->D2H Download->ReleaseOutput\","
            << "\"frames_per_strategy\":100,\"cases\":[";
        for (size_t k = 0; k < std::size(strategies); ++k) {
            DecodeRequest request;
            request.diagnostic_strategy = strategies[k];
            for (int i = 0; i < 12; ++i) {
                auto ticket = Begin(decoder, sample, request);
                Download(decoder, ticket, downloaded.data(), size_t(1920) * 4);
                Require(decoder.ReleaseOutput(ticket), "warmup release");
            }
            std::vector<double> latencies;
            latencies.reserve(100);
            const int ac_before = AcStatus();
            for (int i = 0; i < 100; ++i) {
                const auto start = Clock::now();
                auto ticket = Begin(decoder, sample, request);
                Download(decoder, ticket, downloaded.data(), size_t(1920) * 4);
                Require(decoder.ReleaseOutput(ticket), "timed release");
                const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
                Require(Hash(downloaded.data(), downloaded.size()) == expected_output_hash, "exact output hash");
                latencies.push_back(ms);
            }
            const int ac_after = AcStatus();
            const double mean = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
            double variance = 0;
            for (double value : latencies) variance += (value - mean) * (value - mean);
            variance /= latencies.size() - 1;
            const double median = [&] {
                auto sorted = latencies;
                std::sort(sorted.begin(), sorted.end());
                return (sorted[49] + sorted[50]) * 0.5;
            }();
            if (k) out << ',';
            out << "{\"strategy\":\"" << names[k] << "\",\"frames\":100,\"ac_before\":" << ac_before
                << ",\"ac_after\":" << ac_after << ",\"mean_ms\":" << mean << ",\"median_ms\":" << median
                << ",\"p95_ms\":" << Percentile95(latencies) << ",\"variance_ms2\":" << variance
                << ",\"min_ms\":" << *std::min_element(latencies.begin(), latencies.end())
                << ",\"max_ms\":" << *std::max_element(latencies.begin(), latencies.end())
                << ",\"latencies_ms\":[";
            for (size_t i = 0; i < latencies.size(); ++i) {
                if (i) out << ',';
                out << std::setprecision(9) << latencies[i];
            }
            out << "]}";
            std::cout << names[k] << " mean=" << mean << " median=" << median
                << " p95=" << Percentile95(latencies) << " ms\n" << std::flush;
        }
        out << "]}\n";
        Require(bool(out), "write output JSON");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
