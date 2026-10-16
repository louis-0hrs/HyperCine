// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/quality_state.h"

#include <cstdlib>
#include <iostream>

namespace {
void Require(bool ok, const char* message) { if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); } }
}

int main() {
    using namespace hypercine;
    const int ids[] = {1,2,3,4,5,6};
    const int expected_limiters[] = {0,0,0,0,8,4};
    const int first_y[][9] = {
        {128,128,192,16,16,12,64,64,48},
        {64,64,96,12,12,8,48,48,32},
        {32,32,48,8,8,6,32,32,24},
        {24,24,36,6,6,3,24,24,12},
        {8,8,12,4,4,2,16,16,8},
        {6,6,9,4,4,2,16,16,8}
    };
    for (int p = 0; p < 6; ++p) {
        QualityStateInput in;
        in.quality = static_cast<EncodingQuality>(ids[p]);
        const auto out = CalculateQualityState(in);
        Require(out.ok, "valid profile should calculate");
        Require(out.fs_rate_limiter == expected_limiters[p], "frame-zero limiter");
        for (int k = 0; k < 9; ++k) {
            const int level = k / 3, band = 1 + k % 3;
            Require(out.quant[level * 4 + band] == first_y[p][k], "frame-zero luma quantizer");
        }
        Require(out.quant[0] == 1 && out.scale[0] == 4 && out.scale[11] == 16,
                "low-pass and transform scales");
        Require(IsValidEncodingQuality(in.quality), "SDK quality enum values remain valid");
    }

    QualityStateInput fs2;
    fs2.quality = EncodingQuality::FilmScan2;
    fs2.frame_index = 1;
    fs2.previous_fs_rate_limiter = 8;
    fs2.previous_completed_sample_bytes = 250000;
    const auto fs2_next = CalculateQualityState(fs2);
    Require(fs2_next.ok && fs2_next.fs_rate_limiter == 4, "FilmScan2 independent limiter transition");

    QualityStateInput fs3;
    fs3.quality = EncodingQuality::FilmScan3;
    fs3.frame_index = 1;
    fs3.previous_fs_rate_limiter = 4;
    fs3.previous_completed_sample_bytes = 250000;
    const auto fs3_next = CalculateQualityState(fs3);
    Require(fs3_next.ok && fs3_next.fs_rate_limiter == 0, "FilmScan3 independent limiter transition");

    QualityStateInput medium_overrate;
    medium_overrate.quality = EncodingQuality::Medium;
    medium_overrate.frame_index = 7;
    medium_overrate.previous_completed_sample_bytes = 2117476;
    const auto medium_limited = CalculateQualityState(medium_overrate);
    Require(medium_limited.ok && medium_limited.overbitrate == 1 && medium_limited.quant[1] == 96,
            "Medium fixed-quality overrate table");

    QualityStateInput unsupported;
    unsupported.encoded_precision = 8;
    Require(!CalculateQualityState(unsupported).ok, "unsupported precision fails closed");
    unsupported = {};
    unsupported.quality = static_cast<EncodingQuality>(7);
    Require(!CalculateQualityState(unsupported).ok, "unsupported profile fails closed");
    std::cout << "quality-state deterministic unit vectors PASS\n";
    return 0;
}
