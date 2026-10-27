// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/quality_state.h"
#include <charconv>
#include <cstring>
#include <iostream>

namespace {
template<class T> bool ParseInteger(const char* text, T& value) {
    const auto end = text + std::strlen(text);
    const auto parsed = std::from_chars(text, end, value, 10);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}
}

int main(int argc, char** argv) {
    if (argc != 8) {
        std::cerr << "usage: quality_state_probe profile width height frame_index previous_limiter previous_sample_bytes previous_overbitrate\n";
        return 2;
    }
    hypercine::QualityStateInput in;
    int quality = 0;
    if (!ParseInteger(argv[1], quality) ||
        !ParseInteger(argv[2], in.coding_width) ||
        !ParseInteger(argv[3], in.coding_height) ||
        !ParseInteger(argv[4], in.frame_index) ||
        !ParseInteger(argv[5], in.previous_fs_rate_limiter) ||
        !ParseInteger(argv[6], in.previous_completed_sample_bytes) ||
        !ParseInteger(argv[7], in.previous_overbitrate)) {
        std::cerr << "invalid integer argument\n";
        return 2;
    }
    in.quality = static_cast<hypercine::EncodingQuality>(quality);
    const auto state = hypercine::CalculateQualityState(in);
    if (!state.ok) { std::cerr << state.error << '\n'; return 1; }
    std::cout << "{\"limiter\":" << state.fs_rate_limiter << ",\"overbitrate\":" << state.overbitrate << ",\"quant\":[";
    for (std::size_t i=0; i<state.quant.size(); ++i) std::cout << (i ? "," : "") << state.quant[i];
    std::cout << "],\"scale\":[";
    for (std::size_t i=0; i<state.scale.size(); ++i) std::cout << (i ? "," : "") << state.scale[i];
    std::cout << "]}\n";
    return 0;
}
