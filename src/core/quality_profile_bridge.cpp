// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "quality_profile_bridge.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace hypercine { namespace standalone {
namespace {

// The only profile whose emitted packet this SDK-free writer has qualified
// against original SDK bytes (see docs/FILMSCAN3_RATE_STATE_TIMEBASE.md and
// docs/RGB444_ENCODER.md for the families that are explicitly not qualified).
bool IsFilmScan3(EncodingQuality quality) noexcept {
    return quality == EncodingQuality::FilmScan3;
}

}  // namespace

bool IsQualifiedSessionProfile(EncodingQuality quality) noexcept {
    return IsFilmScan3(quality);
}

const char* SessionProfileRefusalReason(EncodingQuality quality) noexcept {
    if (!IsValidEncodingQuality(quality)) return "quality profile is not one of the six regular profiles";
    if (IsFilmScan3(quality)) return "";
    return "only FilmScan3 is qualified on the SDK-free YUV422 p10 GOP1 writer; "
           "the other five profiles need original SDK per-profile packet gates "
           "(issue #46) and are refused rather than encoded with FilmScan3 bytes";
}

void RequireTablesMatchFrozen(const std::array<int, 36>& quant,
                              const std::array<int, 36>& scale,
                              int fs_rate_limiter) {
    // Independent oracle, kept from the pre-#46 writer, is only compared
    // against. It must never be the value the writer receives.
    const FilmScan3Tables frozen = ComputeFilmScan3Tables(fs_rate_limiter);
    for (unsigned i = 0; i < 36; ++i) {
        if (quant[i] < 0 || scale[i] < 0)
            throw std::logic_error(
                "shared quality state produced a negative table entry at slot "
                + std::to_string(i));
        if (static_cast<unsigned long long>(quant[i]) >
                std::numeric_limits<unsigned>::max() ||
            static_cast<unsigned long long>(scale[i]) >
                std::numeric_limits<unsigned>::max()) {
            throw std::logic_error(
                "shared quality state entry does not fit the writer's table type at slot "
                + std::to_string(i));
        }
        if (static_cast<unsigned>(quant[i]) != frozen.quant[i] ||
            static_cast<unsigned>(scale[i]) != frozen.scale[i]) {
            throw std::logic_error(
                "shared quality state and frozen FilmScan3 table disagree at slot "
                + std::to_string(i));
        }
    }
}

ProfileTables ComputeProfileTables(const ProfileTableRequest& request) {
    const char* refusal = SessionProfileRefusalReason(request.quality);
    if (*refusal) throw std::invalid_argument(refusal);
    if (request.fs_rate_limiter < 0 || request.fs_rate_limiter > 20)
        throw std::invalid_argument("invalid FilmScan3 limiter");

    // The shared calculator owns the profile mathematics and is the single
    // producer of the frame tables: the 36 quant/scale entries below are copied
    // out of its state, never rebuilt from a second local table set.
    QualityStateInput input;
    input.quality = request.quality;
    input.coding_width = request.width;
    input.coding_height = request.height;
    input.encoded_precision = 10;
    input.frame_rate_numerator = 30;
    input.frame_rate_denominator = 1;
    input.gop_length = 1;
    input.full_resolution_chroma = false;
    input.frame_index = request.frame_index;
    input.previous_fs_rate_limiter = request.fs_rate_limiter;
    input.previous_overbitrate = 0;
    input.previous_completed_sample_bytes = request.previous_completed_sample_bytes;
    input.prescale = {{0, 0, 0}};

    const QualityState state = CalculateQualityState(input);
    if (!state.ok) throw std::logic_error(state.error);

    // The frozen helper stays as an independent cross-check, never as the
    // source of the bytes the writer emits. A drift is a defect, so it throws
    // naming the slot instead of silently shifting output.
    RequireTablesMatchFrozen(state.quant, state.scale, std::clamp(state.fs_rate_limiter, 0, 20));

    ProfileTables out;
    out.fs_rate_limiter = std::clamp(state.fs_rate_limiter, 0, 20);
    for (unsigned i = 0; i < 36; ++i) {
        // Checked conversions: the ranges were validated by the guard above.
        out.tables.quant[i] = static_cast<unsigned>(state.quant[i]);
        out.tables.scale[i] = static_cast<unsigned>(state.scale[i]);
    }
    return out;
}

}}  // namespace hypercine::standalone
