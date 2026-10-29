// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include "hypercine/filmscan3.h"
#include "hypercine/quality_state.h"
#include <cstdint>

namespace hypercine { namespace standalone {

// Session-scoped quality profile. The encoder writes exactly one CFHD family
// (progressive YUV422 p10, GOP1), so a profile is only admissible when that
// family has been qualified against original SDK bytes for it.
//
// Issue #46 asks where the six shared profile tables enter FilmScan3Encoder.
// They enter here, through the single SDK-free calculator from #29, so no
// second quantization table can drift away from the shared one. What this
// bridge does NOT do is certify the five non-FilmScan3 profiles: the writer
// path for them is unqualified, so they are refused instead of being encoded
// with FilmScan3 bytes under a different label.
struct ProfileTableRequest {
    EncodingQuality quality = EncodingQuality::FilmScan3;
    unsigned frame_index = 0;
    int fs_rate_limiter = 4;
    std::uint64_t previous_completed_sample_bytes = 0;
    int width = 1920;
    int height = 1080;
};

struct ProfileTables {
    FilmScan3Tables tables;
    // Limiter that applies to the frame being encoded, after rate feedback.
    int fs_rate_limiter = 4;
};

// Returns the 36-entry quant/scale pair for this session profile and the rate
// limiter state for the current frame. Throws std::invalid_argument for a
// profile whose bitstream is not qualified on this path and std::logic_error
// if the shared calculator and the frozen FilmScan3 tables ever disagree.
ProfileTables ComputeProfileTables(const ProfileTableRequest& request);

// True only for profiles whose emitted packet is qualified on this writer.
bool IsQualifiedSessionProfile(EncodingQuality quality) noexcept;

// Human-readable reason a profile is refused on the SDK-free YUV422 writer.
const char* SessionProfileRefusalReason(EncodingQuality quality) noexcept;

}}  // namespace hypercine::standalone
