// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#ifndef HYPERCINE_RGB444_QUALITY_H
#define HYPERCINE_RGB444_QUALITY_H

#include <array>
#include <cstdint>

#include "hypercine/quality_state.h"

namespace hypercine {
namespace rgb444 {

// Format-specific p12 4:4:4 quality contract.
//
// This is NOT the YUV 4:2:2 p10 quality-state calculator. The measured original
// RGB444 p12 packets use a different table: every channel is full resolution,
// the three channels do not share one luma/chroma split, and level-1 chroma
// carries twice the green value.
//
// Everything exported here was measured from the pinned F1 corpus and is
// reproduced by a transcription of the original SDK quantizer restricted to the
// RGB444 p12 progressive GOP-1 path. It is a table/report surface, not an
// encoder: no transform, no quantization of coefficients, no packet writer.

// Which of the three full-resolution planes a band belongs to. The measured
// stream order is G, R, B (upstream/Codec/frame.c stores planes G/R/B); the
// public source frame in hypercine/rgb444.h names its planes red, green, blue.
// This enum describes the encoded stream, so the names follow the stream.
enum class Plane { Green = 0, Red = 1, Blue = 2 };

// Wavelet level as numbered in the bitstream: 1 is the finest level, 3 the
// coarsest. A level carries one lowpass and three highpass bands.
inline constexpr int kRgb444Levels = 3;
inline constexpr int kRgb444HighpassPerLevel = 3;
inline constexpr int kRgb444SubbandsPerPlane = 10;  // 1 lowpass + 9 highpass
inline constexpr int kRgb444BandsPerFrame = 30;     // 3 planes x 10 subbands

enum class Orientation { LH = 1, HL = 2, HH = 3 };

// One declared band of one plane. The lowpass band is subband 0 and carries its
// own lowpass quantization tag but no orientation, B_QUANT or B_SCALE tag, so
// those fields are zero for it rather than "declared zero".
struct BandContract {
    Plane plane = Plane::Green;
    bool lowpass = false;
    int level = 0;        // 3, 2, 1 for highpass bands; 0 for the lowpass
    Orientation orientation = Orientation::LH;
    int subband = 0;      // 0..9, the stream's subband index within the plane
    int quantization = 0; // CODEC_TAG_BAND_QUANTIZATION, or lowpass quantization
    int scale = 0;        // CODEC_TAG_BAND_SCALE; never set on a lowpass
};

// Per-band values for one profile, in stream order G then R then B.
struct ProfileContract {
    EncodingQuality quality = EncodingQuality::FilmScan3;
    std::array<BandContract, kRgb444BandsPerFrame> bands{};
};

// Returns the contract for one of the six regular profiles at that profile's
// initial rate limiter, i.e. exactly the table pinned from the corpus.
// Throws std::invalid_argument for any quality outside that set.
ProfileContract MeasuredProfileContract(EncodingQuality quality);

// The RGB444 p12 analogue of QualityState: the band table the SDK would write
// for a given regular profile, GOP-1 progressive, first frame, full-resolution
// chroma. `fs_rate_limiter` mirrors the SDK's own rate-control state; frame 0
// uses the per-profile initial value.
//
// This is NOT interchangeable with CalculateQualityState() from
// hypercine/quality_state.h. That function's p10 branch stops before the p12
// block, keeps a luma/chroma table split and refuses 12-bit input outright
// (`encoded_precision != 10` is an error). Feeding the measured RGB444 values
// through it is impossible by construction, which is why this format-specific
// surface exists.
struct Rgb444QualityInput {
    EncodingQuality quality = EncodingQuality::FilmScan3;
    // 0 is a valid limiter value, so the initial per-profile limiter is
    // selected with `use_initial_limiter`. It is not inferred from 0.
    bool use_initial_limiter = true;
    int fs_rate_limiter = 0;
};

struct Rgb444QualityState {
    bool ok = false;
    const char* error = "invalid RGB444 quality input";
    EncodingQuality quality = EncodingQuality::FilmScan3;
    int fs_rate_limiter = 0;
    // Only the RGB444 p12 path is implemented. These name the reasons a caller
    // must not substitute the YUV 4:2:2 p10 calculator.
    int encoded_precision = 12;
    bool full_resolution_chroma = true;
    int num_channels = 3;
    // Channel-major 3 x (3 levels x 4 slots), slot 0 of each level being the
    // lowpass. `quant` at a lowpass slot is 1; a lowpass band never carries a
    // B_SCALE tag, so `scale` at those slots is 0 and is not a claim.
    std::array<int, 36> quant{};
    std::array<int, 36> scale{};
    int lowpass_quantization = 1;
};

// Computes the p12 table. Returns ok=false with a specific error for a quality
// outside the six regular profiles or a limiter outside 0..20.
Rgb444QualityState CalculateRgb444QualityState(const Rgb444QualityInput& input);

// Band-level view of the same calculation: 30 entries in stream order, matching
// MeasuredProfileContract for the initial limiter.
ProfileContract BandsFromQualityState(const Rgb444QualityState& state);

}  // namespace rgb444
}  // namespace hypercine

#endif  // HYPERCINE_RGB444_QUALITY_H
