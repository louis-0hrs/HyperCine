// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#ifndef HYPERCINE_QUALITY_STATE_H
#define HYPERCINE_QUALITY_STATE_H

#include <array>
#include <cstdint>

namespace hypercine {

// SDK-identical regular CFHD quality values. Keying and uncompressed modes
// are intentionally outside this portable API.
enum class EncodingQuality : int {
    Low = 1, Medium = 2, High = 3, FilmScan1 = 4, FilmScan2 = 5, FilmScan3 = 6
};

const char* EncodingQualityName(EncodingQuality quality) noexcept;
const char* EncodingQualitySdkName(EncodingQuality quality) noexcept;
bool IsValidEncodingQuality(EncodingQuality quality) noexcept;

struct QualityStateInput {
    EncodingQuality quality = EncodingQuality::FilmScan3;
    int coding_width = 1920;
    int coding_height = 1080;
    int encoded_precision = 10;
    int frame_rate_numerator = 30;
    int frame_rate_denominator = 1;
    int gop_length = 1;
    bool full_resolution_chroma = false;
    std::uint32_t frame_index = 0;
    int previous_fs_rate_limiter = 0;
    int previous_overbitrate = 0;
    // Bytes in the previously completed CFHD sample. This is the byte count
    // corresponding to lastgopbitcount >> 3 for the supported GOP-1 profile.
    std::uint64_t previous_completed_sample_bytes = 0;
    std::array<int, 3> prescale{};
};

struct QualityState {
    bool ok = false;
    const char* error = "invalid quality-state input";
    EncodingQuality quality = EncodingQuality::FilmScan3;
    std::uint32_t frame_index = 0;
    int coding_width = 0;
    int coding_height = 0;
    int encoded_precision = 0;
    int frame_rate_numerator = 0;
    int frame_rate_denominator = 0;
    int gop_length = 0;
    std::array<int, 3> prescale{};
    int fs_rate_limiter = 0;
    int overbitrate = 0;
    std::uint64_t previous_completed_sample_bytes = 0;
    std::array<int, 36> quant{};
    std::array<int, 36> scale{};
};

// Portable counterpart of the regular-profile branch in GoPro quantize.c.
// P1 supports progressive YUV 4:2:2, precision 10, GOP 1 and dimensions whose
// source-byte arithmetic fits signed 32-bit, matching the verified corpus.
QualityState CalculateQualityState(const QualityStateInput& input) noexcept;

}  // namespace hypercine
#endif
