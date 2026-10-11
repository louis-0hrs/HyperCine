// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// Reuse the frozen D fixture recipes verbatim. The historical measurement main
// is never called; this executable only materializes and verifies pixels.
#define main Phase3adHistoricalMainNotCalled
#include "benchmark_decode_phase3ad.cpp"
#undef main

int main(int argc, char** argv) {
    try {
        Require(argc == 4, "usage: audit_decode_phase3ae_corpus ROOT EXPECTED_TSV OUTPUT_JSON");
        const std::filesystem::path root = argv[1];
        std::ifstream input(argv[2]); Require(bool(input), "original fixture fingerprints");
        std::map<std::string, std::string> expected;
        std::string id, hash;
        while (input >> id >> hash) expected[id] = hash;
        auto cases = BuildDevVal(root); auto holdout = BuildHoldout(root);
        cases.insert(cases.end(), holdout.begin(), holdout.end());
        std::map<int, std::vector<Sample>> cache;
        std::ofstream output(argv[3]); Require(bool(output), "corpus output");
        output << "{\"license\":\"MIT OR Apache-2.0\",\"purpose\":\"correctness only\",\"cases\":[";
        bool comma = false; size_t passed = 0, unsupported = 0, failed = 0;
        for (const auto& spec : cases) {
            if (comma) output << ','; comma = true;
            output << "{\"frame\":\"" << Escape(spec.id) << '"';
            try {
                auto sample = Materialize(root, spec, cache);
                const auto fingerprint = Hash(sample.compressed.data(), sample.compressed.size());
                Require(expected.count(spec.id) && expected[spec.id] == fingerprint, "D frozen sample fingerprint");
                output << ",\"sample_fnv1a64\":\"" << fingerprint << '"';
                std::vector<GpuEntropyBandDescriptor> bands; std::string reason;
                if (sample.compressed.size() > 32u * 1024 * 1024 ||
                    !ParseCineFormSampleValidated(sample.compressed.data(), sample.compressed.size(), 24, bands, &reason)) {
                    ++unsupported; output << ",\"supported\":false}";
                    std::cout << spec.id << " unsupported control\n" << std::flush;
                    continue;
                }
                DecoderConfig config; config.maximum_width = sample.width; config.maximum_height = sample.height;
                Decoder decoder(config); Require(decoder.Initialize() == DecodeError::None, "corpus decoder init");
                const auto bgra_hash = Exact(sample, decoder);
                ++passed;
                output << ",\"supported\":true,\"four_strategies_all_stages_exact\":true,\"bgra_fnv1a64\":\"" << bgra_hash << "\"}";
                std::cout << spec.id << " all four strategies exact\n" << std::flush;
            } catch (const std::exception& e) {
                ++failed; output << ",\"error\":\"" << Escape(e.what()) << "\"}";
                std::cout << spec.id << " FAILED " << e.what() << '\n' << std::flush;
            }
        }
        output << "],\"passed\":" << passed << ",\"unsupported\":" << unsupported << ",\"failed\":" << failed << "}\n";
        return failed ? 1 : 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
