// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
// The constants and integer/float operation order follow the regular
// progressive 10-bit quantizer in upstream/Codec/quantize.{c,h}; upstream is
// reference-only and is not included or linked by this translation unit.
#include "hypercine/quality_state.h"

#include <algorithm>
#include <climits>
#include <limits>

namespace hypercine {
const char* EncodingQualityName(EncodingQuality q) noexcept {
    switch (q) {
    case EncodingQuality::Low: return "low";
    case EncodingQuality::Medium: return "medium";
    case EncodingQuality::High: return "high";
    case EncodingQuality::FilmScan1: return "filmscan1";
    case EncodingQuality::FilmScan2: return "filmscan2";
    case EncodingQuality::FilmScan3: return "filmscan3";
    }
    return "invalid";
}
const char* EncodingQualitySdkName(EncodingQuality q) noexcept {
    switch (q) {
    case EncodingQuality::Low: return "CFHD_ENCODING_QUALITY_LOW";
    case EncodingQuality::Medium: return "CFHD_ENCODING_QUALITY_MEDIUM";
    case EncodingQuality::High: return "CFHD_ENCODING_QUALITY_HIGH";
    case EncodingQuality::FilmScan1: return "CFHD_ENCODING_QUALITY_FILMSCAN1";
    case EncodingQuality::FilmScan2: return "CFHD_ENCODING_QUALITY_FILMSCAN2";
    case EncodingQuality::FilmScan3: return "CFHD_ENCODING_QUALITY_FILMSCAN3";
    }
    return "invalid";
}
bool IsValidEncodingQuality(EncodingQuality q) noexcept {
    return q >= EncodingQuality::Low && q <= EncodingQuality::FilmScan3;
}

namespace {
// Per-channel (luma/chroma), per-wavelet-level high-pass tables. These are
// the documented values in GoPro quantize.h after its 10-bit transform-scale
// arithmetic, with low-pass entries supplied separately as 1.
struct Tables { int luma[3][3]; int chroma[3][3]; };
constexpr Tables kLow = {{{128,128,192},{16,16,12},{64,64,48}},
                         {{128,128,192},{16,16,12},{64,64,48}}};
constexpr Tables kMedium = {{{64,64,96},{12,12,8},{48,48,32}},
                            {{64,64,128},{12,12,8},{48,48,32}}};
constexpr Tables kHigh = {{{32,32,48},{8,8,6},{32,32,24}},
                          {{32,32,64},{12,12,8},{48,48,32}}};
constexpr Tables kFilmScan1 = {{{24,24,36},{6,6,3},{24,24,12}},
                               {{24,24,48},{6,6,3},{24,24,12}}};

Tables MidpointTable(const Tables& quality, const Tables& lower) noexcept {
    Tables result{};
    for (int level = 0; level < 3; ++level) for (int band = 0; band < 3; ++band) {
        result.luma[level][band] = quality.luma[level][band] + (lower.luma[level][band] - quality.luma[level][band]) / 2;
        result.chroma[level][band] = quality.chroma[level][band] + (lower.chroma[level][band] - quality.chroma[level][band]) / 2;
    }
    return result;
}

int ClampLimiter(int value) noexcept { return std::max(0, std::min(value, 20)); }

int NextLimiter(const QualityStateInput& in) noexcept {
    int limiter = in.previous_fs_rate_limiter;
    if (in.previous_completed_sample_bytes == 0) return ClampLimiter(limiter);
    // Upstream converts its int byte count to float, then computes the source
    // byte count using integer arithmetic, divides in float, and applies the
    // 4:2:2 adjustment as a separate float operation.
    const std::int32_t source_bytes = static_cast<std::int32_t>(
        (static_cast<std::int64_t>(in.coding_width) * in.coding_height * 3 * in.encoded_precision) / 8);
    const float gop_size = static_cast<float>(static_cast<std::int32_t>(in.previous_completed_sample_bytes));
    float compression = static_cast<float>(source_bytes) / gop_size;
    if (!in.full_resolution_chroma) compression = static_cast<float>(compression / 1.5f);
    if (in.quality == EncodingQuality::FilmScan2) {
        if (compression > 5.5f) { --limiter; if (compression > 6.5f) --limiter; if (compression > 7.5f) limiter -= 2; }
        else if (compression < 4.0f) {
            ++limiter; if (compression < 3.5f) ++limiter; if (compression < 3.0f) ++limiter;
            if (compression < 2.5f) ++limiter; if (compression < 2.0f) ++limiter;
            if (compression < 1.5f) limiter += 2;
        }
    } else {
        if (compression > 4.5f) { --limiter; if (compression > 5.5f) --limiter; if (compression > 6.5f) limiter -= 2; }
        else if (compression < 3.0f) {
            ++limiter; if (compression < 2.5f) ++limiter; if (compression < 2.0f) ++limiter;
            if (compression < 1.5f) limiter += 2;
        }
    }
    return ClampLimiter(limiter);
}
}

QualityState CalculateQualityState(const QualityStateInput& in) noexcept {
    QualityState out;
    out.quality = in.quality; out.frame_index = in.frame_index;
    out.coding_width = in.coding_width; out.coding_height = in.coding_height;
    out.encoded_precision = in.encoded_precision;
    out.frame_rate_numerator = in.frame_rate_numerator; out.frame_rate_denominator = in.frame_rate_denominator;
    out.gop_length = in.gop_length; out.prescale = in.prescale;
    out.previous_completed_sample_bytes = in.previous_completed_sample_bytes;
    if (!IsValidEncodingQuality(in.quality)) { out.error = "quality profile is not one of the six regular profiles"; return out; }
    if (in.coding_width <= 0 || in.coding_height <= 0 || (in.coding_width & 1) || in.encoded_precision != 10 ||
        in.frame_rate_numerator <= 0 || in.frame_rate_denominator <= 0 || in.gop_length != 1 || in.full_resolution_chroma) {
        out.error = "P1 supports progressive 10-bit YUV 4:2:2, positive frame rate and GOP 1"; return out;
    }
    const std::int64_t source_product = static_cast<std::int64_t>(in.coding_width) * in.coding_height * 3 * in.encoded_precision;
    if (source_product / 8 > INT_MAX || in.previous_completed_sample_bytes > INT_MAX) {
        out.error = "source byte arithmetic or prior sample exceeds upstream signed 32-bit range"; return out;
    }

    if (in.quality == EncodingQuality::FilmScan2 || in.quality == EncodingQuality::FilmScan3) {
        out.fs_rate_limiter = in.frame_index == 0
            ? (in.quality == EncodingQuality::FilmScan2 ? 8 : 4)
            : NextLimiter(in);
    } else {
        out.fs_rate_limiter = 0;
    }

    bool use_overrate_table = false;
    if (static_cast<int>(in.quality) <= 3 && in.coding_width <= 1920 && in.coding_height <= 1080 &&
        in.previous_completed_sample_bytes != 0) {
        const std::int64_t prior_bits64 = static_cast<std::int64_t>(in.previous_completed_sample_bytes) * 8;
        if (prior_bits64 > INT_MAX) { out.error = "prior GOP bit count exceeds upstream signed 32-bit rate arithmetic"; return out; }
        const float bit_count = static_cast<float>(static_cast<std::int32_t>(prior_bits64));
        const float frame_rate = static_cast<float>(in.frame_rate_numerator) / static_cast<float>(in.frame_rate_denominator);
        const int current_bitrate = static_cast<int>(bit_count * frame_rate / static_cast<float>(in.gop_length));
        const int upper_limit = 130000000 + (static_cast<int>(in.quality) - 2) * 20000000;
        const int over = std::max(0, std::min(in.previous_overbitrate, 16));
        use_overrate_table = current_bitrate > upper_limit ||
            (over > 0 && current_bitrate >= upper_limit * 8 / 10);
        out.overbitrate = use_overrate_table ? std::max(1, over) : 0;
    }
    static constexpr int kScale[12] = {4,2,2,1, 16,8,8,4, 64,32,32,16};
    for (int c = 0; c < 3; ++c) for (int level = 0; level < 3; ++level) {
        const int base = c * 12;
        for (int band = 0; band < 4; ++band) {
            const int at = base + level * 4 + band;
            out.scale[at] = kScale[level * 4 + band];
            out.quant[at] = band == 0 ? 1 : 0;
        }
    }

    const Tables* tables = nullptr;
    switch (in.quality) {
    case EncodingQuality::Low: tables = &kLow; break;
    case EncodingQuality::Medium: tables = &kMedium; break;
    case EncodingQuality::High: tables = &kHigh; break;
    case EncodingQuality::FilmScan1: tables = &kFilmScan1; break;
    case EncodingQuality::FilmScan2:
    case EncodingQuality::FilmScan3: break;
    }
    const Tables* lower_tables = nullptr;
    switch (in.quality) {
    case EncodingQuality::Low: lower_tables = &kLow; break;
    case EncodingQuality::Medium: lower_tables = &kLow; break;
    case EncodingQuality::High: lower_tables = &kMedium; break;
    case EncodingQuality::FilmScan1:
    case EncodingQuality::FilmScan2:
    case EncodingQuality::FilmScan3: break;
    }
    Tables max_tables{};
    if (tables && lower_tables) max_tables = MidpointTable(*tables, *lower_tables);
    const int limiter_scale = (16 + std::min(out.fs_rate_limiter, 16) * 2) / 2;
    for (int c = 0; c < 3; ++c) {
        for (int level = 0; level < 3; ++level) for (int band = 1; band < 4; ++band) {
            const int at = c * 12 + level * 4 + band;
            if (tables) {
                const Tables* selected = use_overrate_table ? &max_tables : tables;
                out.quant[at] = selected->luma[level][band - 1];
                if (c != 0) out.quant[at] = selected->chroma[level][band - 1];
            } else if (level == 0) {
                const int base = band == 3 ? (c ? 16 : 12) : 8;
                out.quant[at] = std::max(2, base * limiter_scale / 16);
            } else {
                out.quant[at] = (band == 3 ? 2 : 2) * out.scale[at] / 4;
            }
        }
    }
    out.ok = true; out.error = "";
    return out;
}
}  // namespace hypercine
