// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Format-specific RGB444 p12 quality tables for issue #37.
//
// Provenance, in order of strength:
//
//  1. MEASURED. Every value produced for a profile's initial rate limiter was
//     read from the pinned F1 corpus: 16 packets of each of the six profiles at
//     128x128 plus 8 filmscan1 and 8 filmscan3 packets at 1920x1080. That is
//     112 packets and exactly six distinct band tables, one per profile, with
//     identical values across all three planes at levels 2 and 3. Both corpora
//     were verified against their own TRANSFER_SHA256.tsv before any packet was
//     read (64/64 and 888/888 entries, 0 problems).
//
//  2. TRANSCRIBED, NOT MEASURED. The arithmetic below is a transcription of the
//     original SDK quantizer, upstream/Codec/quantize.c
//     QuantizationSetQuality(), restricted to the branches these packets
//     exercise: progressive, GOP 1, frame 0, full-resolution chroma, precision
//     12, rgb_quality 2. Structure: p10 scale/limiter block, then the
//     `precision == CODEC_PRECISION_12BIT` block, then the `goplength == 1`
//     copies that move subband slots 11..13 onto 7..9.
//
//     The corpus contains only first frames, so the limiter-dependent part is
//     validated only at each profile's initial limiter. Other limiter values
//     are the transcribed formula, not measured data, and are labelled as such.
//
//  3. NOT MODELLED. The signed pre-quant stage, the transform, the prescale
//     path and the band framing/payload writer: PENDING_SDK_INTERNAL_STAGE.
//     No packet is written and no coefficient is produced here.
//
// Why a separate table is required: the shared p10 YUV calculator stops at the
// p10 block and carries a luma/chroma split with subsampled chroma. The measured
// RGB444 stream is full resolution in all three planes, and its two non-green
// planes receive the p12 `chromagain` factor at level 1 only.
#include "hypercine/rgb444_quality.h"

#include <algorithm>
#include <stdexcept>

namespace hypercine {
namespace rgb444 {
namespace {

// upstream/Codec/quantize.h tables. The three regular profiles use their own
// base table; the three film-scan profiles all collapse to HIGH (`factor > 3`
// becomes 3) and then diverge through the low-frequency quant and scale terms.
constexpr int kLow[17] = {4, 8, 8, 12, 8, 8, 12, 9, 12, 12, 16, 32, 32, 48, 32, 32, 48};
constexpr int kMedium[17] = {4, 6, 6, 8, 6, 6, 8, 5, 8, 8, 12, 16, 16, 24, 16, 16, 24};
constexpr int kHigh[17] = {4, 4, 4, 6, 4, 4, 6, 5, 8, 8, 8, 8, 8, 12, 8, 8, 12};

// B_SCALE as declared in the stream, by bitstream level. Level 1 is the finest
// level and is numbered last; it declares 2/2/1 but its B_QUANT entries equal
// the unscaled subband values, so no scale multiplication is applied there.
constexpr int kDeclaredScale[4][4] = {
    {0, 0, 0, 0},
    {0, 2, 2, 1},   // finest, stream level 1
    {0, 8, 8, 4},   // middle, stream level 2
    {0, 32, 32, 16} // coarsest, stream level 3
};

// SDK initial rate limiter per profile: filmscan2 starts at 8, filmscan3 at 4,
// everything else at 0 (upstream/Codec/quantize.c `newQuality >= 5 && frame == 0`).
constexpr int kInitialLimiter[6] = {0, 0, 0, 0, 8, 4};

int ProfileIndex(EncodingQuality quality) {
    switch (quality) {
    case EncodingQuality::Low: return 0;
    case EncodingQuality::Medium: return 1;
    case EncodingQuality::High: return 2;
    case EncodingQuality::FilmScan1: return 3;
    case EncodingQuality::FilmScan2: return 4;
    case EncodingQuality::FilmScan3: return 5;
    }
    return -1;
}

// upstream/Codec/quantize.c p12 branch `chromagain`, with rgb_quality fixed at
// the pinned packets' value (2 -> 4, then the level-1 loop applies it on top of
// the `*4` that all three planes already received, giving 8 for the two
// non-green planes and 4 for green).
int ChromaGain(int limiter) {
    int gain = 8;
    if (limiter > 16) {
        gain += limiter - 16;
        if (gain > 8) gain = 8;
    }
    return gain;
}

struct Tables {
    int luma[17] = {};
    int chroma[17] = {};
};

Tables BuildTables(EncodingQuality quality, int limiter) {
    const int profile = ProfileIndex(quality);
    Tables t;
    const int* base = kHigh;
    if (profile == 0) base = kLow;
    else if (profile == 1) base = kMedium;
    for (int i = 0; i < 17; ++i) { t.luma[i] = base[i]; t.chroma[i] = base[i]; }

    const int clamped = std::min(limiter, 16);
    const bool film_scan = ProfileIndex(quality) >= 3;
    int lowfreqquant = 4;
    int scale = 4 * 16;
    if (quality == EncodingQuality::FilmScan1) {
        lowfreqquant = 3;
        scale = 3 * 16;
    } else if (quality == EncodingQuality::FilmScan2 ||
               quality == EncodingQuality::FilmScan3) {
        lowfreqquant = 2;
        scale = 1 * 16 + clamped * 2;
        if (scale >= 4) scale >>= 1;
    }

    if (film_scan) {
        for (int i = 1; i < 7; ++i) { t.luma[i] = lowfreqquant; t.chroma[i] = lowfreqquant; }
    }
    for (int i = 8; i < 17; ++i) {
        t.luma[i] = std::max(2, (t.luma[i] * scale) >> 4);
        t.chroma[i] = std::max(2, (t.chroma[i] * scale) >> 4);
    }
    t.luma[7] = 4;
    t.chroma[7] = 4;

    // p12 block.
    const int chromagain = ChromaGain(limiter);
    if (film_scan) {
        for (int i = 1; i < 7; ++i) { t.luma[i] = lowfreqquant; t.chroma[i] = lowfreqquant; }
    }
    for (int i = 4; i < 7; ++i) {
        t.luma[i] *= 4;
        t.chroma[i] *= 4;
    }
    for (int i = 11; i < 17; ++i) {
        t.luma[i] *= 4;
        t.chroma[i] *= chromagain;
    }

    // `if (goplength == 1)`: the GOP-1 stream numbers the finest level with the
    // slots 11..13, so 7..9 are copied from there unscaled.
    for (int i = 7; i <= 9; ++i) {
        t.luma[i] = t.luma[i + 4];
        t.chroma[i] = t.chroma[i + 4];
    }
    return t;
}

// The coarsest and middle levels are written as table x declared-scale / 4.
// The finest level keeps its unscaled subband value.
int BandQuantization(const int table[17], int level, int band) {
    const int subband = (3 - level) * 3 + band;
    if (level == 1) return table[subband];
    return (table[subband] * kDeclaredScale[level][band]) >> 2;
}

void FillChannel(const int table[17], int block_base, std::array<int, 36>& quant,
                 std::array<int, 36>& scale) {
    for (int level = 3; level >= 1; --level) {
        const int block = block_base + (3 - level) * 4;
        quant[block] = 1;  // every measured lowpass declares quantization 1
        scale[block] = 0;  // and no lowpass carries a B_SCALE tag
        for (int band = 1; band < 4; ++band) {
            quant[block + band] = BandQuantization(table, level, band);
            scale[block + band] = kDeclaredScale[level][band];
        }
    }
}

}  // namespace

Rgb444QualityState CalculateRgb444QualityState(const Rgb444QualityInput& input) {
    Rgb444QualityState out;
    out.quality = input.quality;
    const int profile = ProfileIndex(input.quality);
    if (profile < 0) {
        out.error = "RGB444 p12 supports only the six regular encoding profiles";
        return out;
    }
    const int limiter = input.use_initial_limiter ? kInitialLimiter[profile]
                                                  : input.fs_rate_limiter;
    if (limiter < 0 || limiter > 20) {
        out.error = "fs_rate_limiter must be within the SDK range 0..20";
        return out;
    }
    out.fs_rate_limiter = limiter;

    const Tables tables = BuildTables(input.quality, limiter);
    FillChannel(tables.luma, 0, out.quant, out.scale);    // Green
    FillChannel(tables.chroma, 12, out.quant, out.scale); // Red
    FillChannel(tables.chroma, 24, out.quant, out.scale); // Blue
    out.lowpass_quantization = 1;
    out.ok = true;
    out.error = "";
    return out;
}

ProfileContract BandsFromQualityState(const Rgb444QualityState& state) {
    if (!state.ok) throw std::invalid_argument(state.error);
    ProfileContract contract;
    contract.quality = state.quality;
    int at = 0;
    for (int channel = 0; channel < 3; ++channel) {
        const Plane plane = channel == 0 ? Plane::Green
                            : channel == 1 ? Plane::Red
                                           : Plane::Blue;
        // The stream writes subband 0 (lowpass) first, then the three highpass
        // bands of each level from the coarsest to the finest.
        BandContract& low = contract.bands[at++];
        low.plane = plane;
        low.lowpass = true;
        low.level = 0;
        low.subband = 0;
        low.quantization = state.lowpass_quantization;
        low.scale = 0;

        for (int level = 3; level >= 1; --level) {
            const int block = channel * 12 + (3 - level) * 4;
            for (int band = 1; band < 4; ++band) {
                BandContract& entry = contract.bands[at++];
                entry.plane = plane;
                entry.lowpass = false;
                entry.level = level;
                entry.orientation = static_cast<Orientation>(band);
                entry.subband = (3 - level) * 3 + band;
                entry.quantization = state.quant[block + band];
                entry.scale = state.scale[block + band];
            }
        }
    }
    return contract;
}

ProfileContract MeasuredProfileContract(EncodingQuality quality) {
    Rgb444QualityInput in;
    in.quality = quality;
    const Rgb444QualityState state = CalculateRgb444QualityState(in);
    if (!state.ok) throw std::invalid_argument(state.error);
    return BandsFromQualityState(state);
}

}  // namespace rgb444
}  // namespace hypercine
