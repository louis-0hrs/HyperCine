// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/quality_state.h"
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 8) {
        std::cerr << "usage: quality_state_probe profile width height frame_index previous_limiter previous_sample_bytes previous_overbitrate\n";
        return 2;
    }
    hypercine::QualityStateInput in;
    in.quality = static_cast<hypercine::EncodingQuality>(std::atoi(argv[1]));
    in.coding_width = std::atoi(argv[2]);
    in.coding_height = std::atoi(argv[3]);
    in.frame_index = static_cast<std::uint32_t>(std::strtoul(argv[4], nullptr, 10));
    in.previous_fs_rate_limiter = std::atoi(argv[5]);
    in.previous_completed_sample_bytes = std::strtoull(argv[6], nullptr, 10);
    in.previous_overbitrate = std::atoi(argv[7]);
    const auto state = hypercine::CalculateQualityState(in);
    if (!state.ok) { std::cerr << state.error << '\n'; return 1; }
    std::cout << "{\"limiter\":" << state.fs_rate_limiter << ",\"overbitrate\":" << state.overbitrate << ",\"quant\":[";
    for (std::size_t i=0; i<state.quant.size(); ++i) std::cout << (i ? "," : "") << state.quant[i];
    std::cout << "],\"scale\":[";
    for (std::size_t i=0; i<state.scale.size(); ++i) std::cout << (i ? "," : "") << state.scale[i];
    std::cout << "]}\n";
    return 0;
}
