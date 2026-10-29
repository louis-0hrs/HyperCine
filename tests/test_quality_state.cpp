// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "hypercine/quality_state.h"

#include <cstdlib>
#include <climits>
#include <limits>
#include <iostream>
#include <string>

namespace {
void Require(bool ok, const char* message) { if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); } }
// Require takes a C string, so a composed message needs storage that outlives the
// call. This keeps the existing signature that every other assertion relies on.
const char* Detail(const std::string& text) {
    static std::string buffer;
    buffer = text;
    return buffer.c_str();
}
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
    // 2 117 476 bytes at 30/1 is 508.2 Mbps against Medium's 130 Mbps ceiling,
    // i.e. 391 % — above 120 %. The frozen ladder (upstream/Codec/quantize.c:
    // 3046-3063) sets the counter to 1 and then increments it, so a single large
    // frame lands on 2, not 1. The earlier expectation of 1 encoded the flat-gate
    // reading that this change corrects; the selected *table* is identical either
    // way, which real SDK packets confirm (level-1 luma 96/96/144).
    Require(medium_limited.ok && medium_limited.overbitrate == 2 && medium_limited.quant[1] == 96,
            "Medium overrate ladder enters at 2 for a frame above 120 %");

    QualityStateInput unsupported;
    unsupported.encoded_precision = 8;
    Require(!CalculateQualityState(unsupported).ok, "unsupported precision fails closed");
    unsupported = {};
    unsupported.quality = static_cast<EncodingQuality>(7);
    Require(!CalculateQualityState(unsupported).ok, "unsupported profile fails closed");
    // Exercise the actual public integer limits, without allocating frame buffers.
    QualityStateInput boundary;
    boundary.coding_width = INT_MAX - 1;
    boundary.coding_height = INT_MAX;
    Require(!CalculateQualityState(boundary).ok, "huge geometry rejected before product overflow");
    boundary.coding_width = 2;
    boundary.coding_height = 286331153;
    Require(CalculateQualityState(boundary).ok, "source byte INT_MAX after truncation accepted");
    ++boundary.coding_height;
    Require(!CalculateQualityState(boundary).ok, "first excessive source byte count rejected");
    boundary = {};
    boundary.previous_completed_sample_bytes = std::numeric_limits<std::uint64_t>::max();
    Require(!CalculateQualityState(boundary).ok, "oversized prior sample rejected");

    for (const auto quality : {EncodingQuality::Low, EncodingQuality::Medium, EncodingQuality::High}) {
        boundary = {};
        boundary.quality = quality;
        boundary.frame_index = 1;
        boundary.frame_rate_numerator = 1;
        boundary.previous_completed_sample_bytes = 268435440;
        Require(CalculateQualityState(boundary).ok, "largest float bitrate below signed limit accepted");
        boundary.previous_completed_sample_bytes = 268435455;
        Require(!CalculateQualityState(boundary).ok, "bit count rounding to 2^31 rejected before cast");
        boundary.previous_completed_sample_bytes = 268435456;
        Require(!CalculateQualityState(boundary).ok, "prior integer bit count overflow rejected");
        boundary.previous_completed_sample_bytes = 1;
        boundary.frame_rate_numerator = INT_MAX;
        Require(!CalculateQualityState(boundary).ok, "extreme frame rate rejected");
        boundary.frame_rate_denominator = INT_MAX;
        Require(CalculateQualityState(boundary).ok, "large rational components with safe bitrate accepted");
        boundary.frame_rate_numerator = 1;
        Require(CalculateQualityState(boundary).ok, "tiny positive frame rate accepted");
    }
    for (const auto quality : {EncodingQuality::FilmScan2, EncodingQuality::FilmScan3}) {
        boundary = {};
        boundary.quality = quality;
        boundary.frame_index = 1;
        boundary.previous_completed_sample_bytes = INT_MAX;
        boundary.previous_fs_rate_limiter = INT_MAX;
        const auto upper = CalculateQualityState(boundary);
        Require(upper.ok && upper.fs_rate_limiter == 20, "large prior limiter saturates without overflow");
        boundary.previous_completed_sample_bytes = 1;
        boundary.previous_fs_rate_limiter = INT_MIN;
        const auto lower = CalculateQualityState(boundary);
        Require(lower.ok && lower.fs_rate_limiter == 0, "negative prior limiter saturates without overflow");
    }

    // Issue #46: the overbitrate hysteresis must be the frozen five-branch
    // *ladder*, not a single gate. The expectation table below is not derived
    // from this implementation: every row is the state recovered from real
    // original-SDK packets of a 120-frame 1080p Medium sequence, where each
    // frame's selected table (tag 53, level-1 luma) is visible in the bytes.
    // The observed base table is 64/64/96 and the observed hold table 96/96/144,
    // so "hold" in the table below means "over > 0".
    {
        // Frame N selects its table from the bitrate of frame N-1, so the
        // "prev_bytes" column is the *preceding* frame's packet size and the "over"
        // column is the counter this implementation must report for N. The rows are
        // deliberately contiguous (7..20) because the counter is carried: a table
        // that skipped frames would be testing arithmetic over gaps that never
        // happened. Every prev_bytes value is a real packet size from the corpus,
        // and every "hold" value is the table actually observed in that frame's
        // tag 53.
        struct Step { unsigned frame; std::size_t prev_bytes; int over; bool hold; };
        static constexpr Step kMedium[] = {
            {7,  176048,  0, false},   // 32 %  - below ceiling, no hold
            {8, 2117644,  2, true},    // 391 % - enter at 2, not 1
            {9, 1229176,  3, true},    // 227 %
            {10, 1834296, 4, true},    // 339 %
            {11, 1228932, 5, true},
            {12, 1834452, 6, true},
            {13, 1229212, 7, true},
            {14, 1550640, 8, true},
            {15, 554444,  8, true},    // 102 % - inside the ceiling, ladder holds
            {16, 741452,  9, true},
            {17, 1550308, 10, true},
            {18, 165088,  9, true},    // 30 %  - single step down, no exit
            {19, 165116,  8, true},
            {20, 165768,  7, true},
        };
        int carried = 0;
        for (const auto& step : kMedium) {
            QualityStateInput in;
            in.quality = EncodingQuality::Medium;
            in.frame_index = step.frame;
            in.previous_completed_sample_bytes = step.prev_bytes;
            in.previous_overbitrate = carried;
            const auto state = CalculateQualityState(in);
            Require(state.ok, "Medium ladder input must be valid");
            Require(state.overbitrate == step.over,
                    Detail("Medium ladder counter must match real SDK bytes at frame "
                           + std::to_string(step.frame)
                           + " expected " + std::to_string(step.over)
                           + " got " + std::to_string(state.overbitrate)));
            // The hold table is selected exactly when the counter is non-zero; the
            // base level-1 luma is 64 and the hold table's is 96. Reading the
            // selected value proves the counter is actually wired to the table,
            // not merely tracked alongside it.
            Require((state.quant[1] == 96) == step.hold,
                    Detail("Medium selected table must follow the counter at frame "
                           + std::to_string(step.frame)));
            carried = state.overbitrate;
        }
        // Anti-vacuity: the ladder must actually leave the hold state. If every
        // row above were `hold`, a stuck counter would pass the whole table.
        Require(carried > 0, "the pinned window must end inside the hold state");
        QualityStateInput relax;
        relax.quality = EncodingQuality::Medium;
        relax.previous_overbitrate = 1;
        relax.previous_completed_sample_bytes = 165000;   // 39.6 Mbps, 30 % of ceiling
        const auto left = CalculateQualityState(relax);
        Require(left.ok && left.overbitrate == 0 && left.quant[1] == 64,
                "a counter of 1 must leave hold below 80 % of the ceiling");
        // The corpus happens to contain no frame whose preceding bitrate lands in
        // the 80..100 % window — measured, not assumed: scanning all 120 frames of
        // both Medium and High finds none. So the last rule difference cannot be
        // pinned from packets, and is pinned to the frozen source instead:
        //   quantize.c:3060  if(q->overbitrate > 1 && currentbitrate < upperlimit)
        // i.e. a counter above 1 steps down anywhere below 100 %, not below 80 %.
        // Without this case, an implementation that applied the 80 % floor to the
        // whole ladder instead of only to the counter==1 exit would pass every
        // corpus-derived row above.
        {
            QualityStateInput in_band;
            in_band.quality = EncodingQuality::Medium;
            in_band.previous_overbitrate = 5;
            // 130 Mbps ceiling; 0.9 * ceiling / (8 * 30) = 487 500 bytes -> 90 %.
            in_band.previous_completed_sample_bytes = 487500;
            const auto band = CalculateQualityState(in_band);
            Require(band.ok && band.overbitrate == 4 && band.quant[1] == 96,
                    "a counter above 1 must step down inside the 80-100 % window "
                    "(frozen quantize.c:3060), not only below 80 %");
        }
        // ... and one step above must NOT leave at the same bitrate: that is the
        // exact property the single-gate implementation destroyed.
        relax.previous_overbitrate = 2;
        const auto held = CalculateQualityState(relax);
        Require(held.ok && held.overbitrate == 1 && held.quant[1] == 96,
                "a counter above 1 must step down, not exit, below 80 % of the ceiling");
    }
    std::cout << "quality-state deterministic unit vectors PASS\n";
    return 0;
}
