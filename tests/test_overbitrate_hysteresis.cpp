// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Issue #46. Drive the original-SDK overbitrate window 0,2,3,2,1,1,0 through
// CalculateQualityState and check, at every step, which table is selected.
//
// The packet-only hysteresis manifest does not record internal state: all 145
// frames store sdk_overbitrate as not observable. Replaying that manifest's
// hold-ramp sizes through the ladder never produces 0,2,3,2,1,1,0 (Low, Medium
// and High climb to the cap of 16). The rows below are the directly read SDK
// fields q.overbitrate and the preceding packet size, from the separate
// seven-frame state capture. Both transfer indices were checked (177/177 and
// 35/35) before these numbers were copied. Tag 53 on those same packets is the
// table column. This is not a VC-5 or SMPTE conformance claim.
#include "hypercine/quality_state.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
void Require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

// Same float order as quality_state.cpp: int bits, float32 rate, truncating cast.
int CurrentBitrate(std::uint64_t previous_bytes) {
    const auto bits = static_cast<std::int32_t>(previous_bytes * 8);
    const float bit_count = static_cast<float>(bits);
    const float bitrate = bit_count * (30.0f / 1.0f);
    return static_cast<int>(bitrate);
}

std::int64_t Ceiling(hypercine::EncodingQuality quality) {
    return 130000000 + (static_cast<std::int64_t>(quality) - 2) * 20000000;
}

// Nine high-pass entries, level-major, bands 1..3. Channel 0 is luma.
struct Table9 {
    int v[9];
};

bool SameTable(const Table9& a, const Table9& b) {
    for (int i = 0; i < 9; ++i) if (a.v[i] != b.v[i]) return false;
    return true;
}

void ExpectChannel(const hypercine::QualityState& state, int channel, const Table9& table,
                   const std::string& where) {
    for (int k = 0; k < 9; ++k) {
        const int level = k / 3;
        const int band = 1 + (k % 3);
        const int at = channel * 12 + level * 4 + band;
        Require(state.quant[at] == table.v[k],
                where + " quant[" + std::to_string(at) + "] expected " +
                    std::to_string(table.v[k]) + " got " + std::to_string(state.quant[at]));
    }
}

struct Step {
    std::uint64_t previous_bytes;
    int sdk_overbitrate;
};

struct ProfileTrace {
    const char* name;
    hypercine::EncodingQuality quality;
    Step steps[7];
    Table9 luma_base;
    Table9 luma_over;
    Table9 chroma_base;
    Table9 chroma_over;
    // Low's midpoint of itself is Low, so tag 53 cannot show the hold.
    bool quant_discriminates;
};

const ProfileTrace kTraces[] = {
    {"low", hypercine::EncodingQuality::Low,
     {{0, 0}, {1655372, 2}, {1655372, 3}, {440980, 2}, {440980, 1}, {440980, 1}, {163452, 0}},
     {{128, 128, 192, 16, 16, 12, 64, 64, 48}},
     {{128, 128, 192, 16, 16, 12, 64, 64, 48}},
     {{128, 128, 192, 16, 16, 12, 64, 64, 48}},
     {{128, 128, 192, 16, 16, 12, 64, 64, 48}},
     false},
    {"medium", hypercine::EncodingQuality::Medium,
     {{0, 0}, {2117644, 2}, {1834008, 3}, {520400, 2}, {520400, 1}, {520400, 1}, {165024, 0}},
     {{64, 64, 96, 12, 12, 8, 48, 48, 32}},
     {{96, 96, 144, 14, 14, 10, 56, 56, 40}},
     {{64, 64, 128, 12, 12, 8, 48, 48, 32}},
     {{96, 96, 160, 14, 14, 10, 56, 56, 40}},
     true},
    {"high", hypercine::EncodingQuality::High,
     {{0, 0}, {2712844, 2}, {2336132, 3}, {525552, 2}, {525552, 1}, {525552, 1}, {168408, 0}},
     {{32, 32, 48, 8, 8, 6, 32, 32, 24}},
     {{48, 48, 72, 10, 10, 7, 40, 40, 28}},
     {{32, 32, 64, 12, 12, 8, 48, 48, 32}},
     {{48, 48, 96, 12, 12, 8, 48, 48, 32}},
     true},
};

const int kExpectedOver[7] = {0, 2, 3, 2, 1, 1, 0};
}  // namespace

int main() {
    using hypercine::CalculateQualityState;
    using hypercine::QualityStateInput;

    int profiles = 0;
    for (const auto& trace : kTraces) {
        ++profiles;
        Require(SameTable(trace.luma_base, trace.luma_over) != trace.quant_discriminates,
                std::string(trace.name) + " table discriminant flag does not match the pinned tables");
        int carried = 0;
        int band_run = 0;
        int band_run_max = 0;
        for (int frame = 0; frame < 7; ++frame) {
            const Step& step = trace.steps[frame];
            Require(step.sdk_overbitrate == kExpectedOver[frame],
                    std::string(trace.name) + " pinned SDK counter left the window 0,2,3,2,1,1,0");
            QualityStateInput in;
            in.quality = trace.quality;
            in.frame_index = static_cast<std::uint32_t>(frame);
            in.previous_overbitrate = carried;
            in.previous_completed_sample_bytes = step.previous_bytes;
            const auto state = CalculateQualityState(in);
            const std::string where = std::string(trace.name) + " frame " + std::to_string(frame);
            Require(state.ok, where + " input rejected");
            Require(state.overbitrate == step.sdk_overbitrate,
                    where + " overbitrate expected " + std::to_string(step.sdk_overbitrate) +
                        " got " + std::to_string(state.overbitrate));

            const bool overrate = state.overbitrate > 0;
            const Table9& luma = overrate ? trace.luma_over : trace.luma_base;
            const Table9& chroma = overrate ? trace.chroma_over : trace.chroma_base;
            ExpectChannel(state, 0, luma, where + " luma");
            ExpectChannel(state, 1, chroma, where + " chroma");
            ExpectChannel(state, 2, chroma, where + " chroma2");

            const bool in_band = step.previous_bytes != 0 && [&] {
                const auto ceiling = Ceiling(trace.quality);
                const auto bitrate = static_cast<std::int64_t>(CurrentBitrate(step.previous_bytes));
                // Strictly inside (80 %, 100 %) of the ceiling. The exit rule uses
                // the same integer 80 % cut (upper * 8 / 10).
                return bitrate > ceiling * 8 / 10 && bitrate < ceiling;
            }();
            // Frames 3 and 4 are the two consecutive in-band holds: the counter
            // steps 3→2 then 2→1, and the overrate table stays selected because
            // the counter is still > 0. Frame 5 is the extra stall at 1.
            if (frame == 3 || frame == 4 || frame == 5) {
                Require(in_band, where + " preceding bitrate is not inside 80..100 %");
                Require(state.overbitrate > 0, where + " hold frame cleared the counter");
                Require(overrate, where + " hold frame dropped the overrate table");
            } else {
                Require(!in_band, where + " unexpectedly inside the 80..100 % band");
            }
            if (in_band && state.overbitrate > 0) {
                ++band_run;
                if (band_run > band_run_max) band_run_max = band_run;
            } else {
                band_run = 0;
            }
            carried = state.overbitrate;
        }
        Require(band_run_max >= 2,
                std::string(trace.name) + " did not keep the overrate table for two consecutive 80..100 % frames");
        Require(carried == 0, std::string(trace.name) + " did not leave hold on the final frame");
    }
    Require(profiles == 3, "Low, Medium and High were not all driven");
    std::cout << "overbitrate hysteresis 0,2,3,2,1,1,0 PASS\n";
    return 0;
}
