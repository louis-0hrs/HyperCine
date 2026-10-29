// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "quality_profile_bridge.h"
#include <algorithm>
#include <stdexcept>

namespace hypercine { namespace standalone {
namespace {

// The only profile whose emitted packet this SDK-free writer has qualified
// against original SDK bytes (see docs/FILMSCAN3_RATE_STATE_TIMEBASE.md and
// docs/RGB444_ENCODER.md for the families that are explicitly not qualified).
bool IsFilmScan3(EncodingQuality quality) noexcept {
    return quality == EncodingQuality::FilmScan3;
}

int InitialLimiter(EncodingQuality quality) noexcept {
    // Frame 0 has no prior sample, so the shared calculator keeps the seed the
    // profile starts from: FS2 = 8, FS3 = 4. Both are upstream constants.
    switch (quality) {
    case EncodingQuality::FilmScan2: return 8;
    case EncodingQuality::FilmScan3: return 4;
    default: return 0;
    }
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

ProfileTables ComputeProfileTables(const ProfileTableRequest& request) {
    const char* refusal = SessionProfileRefusalReason(request.quality);
    if (*refusal) throw std::invalid_argument(refusal);
    if (request.fs_rate_limiter < 0 || request.fs_rate_limiter > 20)
        throw std::invalid_argument("invalid FilmScan3 limiter");

    // The shared calculator owns the profile mathematics. Feed it exactly the
    // state the session holds, so the limiter it returns is the one the frozen
    // FilmScan3 tables are then built from.
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

    ProfileTables out;
    out.fs_rate_limiter = std::clamp(state.fs_rate_limiter, 0, 20);

    // Keep one frozen table source for FilmScan3: the existing helper is the
    // writer's only quant/scale producer, and the shared calculator must agree
    // with it exactly. A drift between the two is a defect, not a preference.
    out.tables = ComputeFilmScan3Tables(out.fs_rate_limiter);
    for (unsigned i = 0; i < 36; ++i) {
        if (static_cast<unsigned>(state.quant[i]) != out.tables.quant[i] ||
            static_cast<unsigned>(state.scale[i]) != out.tables.scale[i]) {
            throw std::logic_error(
                "shared quality state and frozen FilmScan3 table disagree at slot "
                + std::to_string(i));
        }
    }
    return out;
}

}}  // namespace hypercine::standalone
