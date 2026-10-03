// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include <iostream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <iomanip>
#include "hypercine/decode_inverse_cuda.h"
#include "decode_phase1_support.h"

using namespace HyperCine;
using namespace HyperCine::DecodePhase1;

static inline int16_t sat16(int32_t val) {
    if (val < -32768) return -32768;
    if (val > 32767) return 32767;
    return static_cast<int16_t>(val);
}

static inline int16_t sat16_add(int16_t a, int16_t b) {
    return sat16(int32_t(a) + int32_t(b));
}

static inline int16_t sat16_sub(int16_t a, int16_t b) {
    return sat16(int32_t(a) - int32_t(b));
}

// Reference CPU implementation of spatial inverse
void CpuInvertSpatial(
    const int16_t* lowlow, int ll_pitch,
    const int16_t* lowhigh, int lh_pitch,
    const int16_t* highlow, int hl_pitch,
    const int16_t* highhigh, int hh_pitch,
    int16_t* out, int out_pitch,
    int width, int height, int descale)
{
    int ll_stride = ll_pitch / sizeof(int16_t);
    int lh_stride = lh_pitch / sizeof(int16_t);
    int hl_stride = hl_pitch / sizeof(int16_t);
    int hh_stride = hh_pitch / sizeof(int16_t);
    int out_stride = out_pitch / sizeof(int16_t);

    std::vector<int16_t> even_low(width * height);
    std::vector<int16_t> odd_low(width * height);
    std::vector<int16_t> even_high(width * height);
    std::vector<int16_t> odd_high(width * height);

    // 1. Vertical inverse
    for (int r = 0; r < height; ++r) {
        for (int c = 0; c < width; ++c) {
            int16_t el, ol, eh, oh;
            if (r == 0) {
                // Top border
                int32_t even_l = (((11 * lowlow[0 * ll_stride + c] - 4 * lowlow[1 * ll_stride + c] + lowlow[2 * ll_stride + c] + 4) >> 3) + highlow[0 * hl_stride + c]) >> 1;
                int32_t odd_l  = (((5  * lowlow[0 * ll_stride + c] + 4 * lowlow[1 * ll_stride + c] - lowlow[2 * ll_stride + c] + 4) >> 3) - highlow[0 * hl_stride + c]) >> 1;
                el = sat16(even_l);
                ol = sat16(odd_l);

                int32_t even_h = (((11 * lowhigh[0 * lh_stride + c] - 4 * lowhigh[1 * lh_stride + c] + lowhigh[2 * lh_stride + c] + 4) >> 3) + highhigh[0 * hh_stride + c]) >> 1;
                int32_t odd_h  = (((5  * lowhigh[0 * lh_stride + c] + 4 * lowhigh[1 * lh_stride + c] - lowhigh[2 * lh_stride + c] + 4) >> 3) - highhigh[0 * hh_stride + c]) >> 1;
                eh = sat16(even_h);
                oh = sat16(odd_h);
            } else if (r == height - 1) {
                // Bottom border
                int32_t even_l = (((5  * lowlow[r * ll_stride + c] + 4 * lowlow[(r - 1) * ll_stride + c] - lowlow[(r - 2) * ll_stride + c] + 4) >> 3) + highlow[r * hl_stride + c]) >> 1;
                int32_t odd_l  = (((11 * lowlow[r * ll_stride + c] - 4 * lowlow[(r - 1) * ll_stride + c] + lowlow[(r - 2) * ll_stride + c] + 4) >> 3) - highlow[r * hl_stride + c]) >> 1;
                el = sat16(even_l);
                ol = sat16(odd_l);

                int32_t even_h = (((5  * lowhigh[r * lh_stride + c] + 4 * lowhigh[(r - 1) * lh_stride + c] - lowhigh[(r - 2) * lh_stride + c] + 4) >> 3) + highhigh[r * hh_stride + c]) >> 1;
                int32_t odd_h  = (((11 * lowhigh[r * lh_stride + c] - 4 * lowhigh[(r - 1) * lh_stride + c] + lowhigh[(r - 2) * lh_stride + c] + 4) >> 3) - highhigh[r * hh_stride + c]) >> 1;
                eh = sat16(even_h);
                oh = sat16(odd_h);
            } else {
                // Middle rows
                if (descale == 0) {
                    int16_t l0 = lowlow[(r - 1) * ll_stride + c];
                    int16_t l1 = lowlow[r * ll_stride + c];
                    int16_t l2 = lowlow[(r + 1) * ll_stride + c];
                    int16_t h_val = highlow[r * hl_stride + c];

                    int16_t diff_even = sat16_add(sat16_sub(l0, l2), 4) >> 3;
                    el = (sat16_add(sat16_add(l1, diff_even), h_val)) >> 1;

                    int16_t diff_odd = sat16_add(sat16_add(sat16_sub(0, l0), l2), 4) >> 3;
                    ol = (sat16_sub(sat16_add(l1, diff_odd), h_val)) >> 1;

                    int16_t lh0 = lowhigh[(r - 1) * lh_stride + c];
                    int16_t lh1 = lowhigh[r * lh_stride + c];
                    int16_t lh2 = lowhigh[(r + 1) * lh_stride + c];
                    int16_t hh_val = highhigh[r * hh_stride + c];

                    int16_t diff_h_even = sat16_add(sat16_sub(lh0, lh2), 4) >> 3;
                    eh = (sat16_add(sat16_add(lh1, diff_h_even), hh_val)) >> 1;

                    int16_t diff_h_odd = sat16_add(sat16_add(sat16_sub(0, lh0), lh2), 4) >> 3;
                    oh = (sat16_sub(sat16_add(lh1, diff_h_odd), hh_val)) >> 1;
                } else {
                    int32_t l0 = lowlow[(r - 1) * ll_stride + c];
                    int32_t l1 = lowlow[r * ll_stride + c];
                    int32_t l2 = lowlow[(r + 1) * ll_stride + c];
                    int32_t h_val = highlow[r * hl_stride + c];

                    int32_t even_l = (((l0 - l2 + 4) >> 3) + l1 + h_val) >> 1;
                    int32_t odd_l  = (((-l0 + l2 + 4) >> 3) + l1 - h_val) >> 1;
                    el = sat16(even_l);
                    ol = sat16(odd_l);

                    int32_t lh0 = lowhigh[(r - 1) * lh_stride + c];
                    int32_t lh1 = lowhigh[r * lh_stride + c];
                    int32_t lh2 = lowhigh[(r + 1) * lh_stride + c];
                    int32_t hh_val = highhigh[r * hh_stride + c];

                    int32_t even_h = (((lh0 - lh2 + 4) >> 3) + lh1 + hh_val) >> 1;
                    int32_t odd_h  = (((-lh0 + lh2 + 4) >> 3) + lh1 - hh_val) >> 1;
                    eh = sat16(even_h);
                    oh = sat16(odd_h);
                }
            }
            even_low[r * width + c] = el;
            odd_low[r * width + c] = ol;
            even_high[r * width + c] = eh;
            odd_high[r * width + c] = oh;
        }
    }

    // 2. Horizontal inverse
    auto horiz_row = [&](const int16_t* L, const int16_t* H, int16_t* row_out) {
        // Left border
        int32_t left_even = ((11 * L[0] - 4 * L[1] + L[2] + 4) >> 3) + H[0];
        int32_t left_odd  = ((5  * L[0] + 4 * L[1] - L[2] + 4) >> 3) - H[0];
        if (descale == 0) {
            row_out[0] = sat16(left_even >> 1);
            row_out[1] = sat16(left_odd >> 1);
        } else {
            row_out[0] = sat16(left_even * 2);
            row_out[1] = sat16(left_odd * 2);
        }

        // Interior
        for (int c = 1; c < width - 1; ++c) {
            if (descale == 0) {
                int16_t diff_e = sat16_add(sat16_sub(L[c - 1], L[c + 1]), 4) >> 3;
                int16_t ev = sat16_add(sat16_add(L[c], diff_e), H[c]) >> 1;

                int16_t diff_o = sat16_add(sat16_add(sat16_sub(0, L[c - 1]), L[c + 1]), 4) >> 3;
                int16_t od = sat16_sub(sat16_add(L[c], diff_o), H[c]) >> 1;

                row_out[2 * c]     = sat16(ev);
                row_out[2 * c + 1] = sat16(od);
            } else {
                int16_t diff_e = sat16_add(sat16_sub(L[c - 1], L[c + 1]), 4) >> 3;
                int16_t ev = sat16_add(sat16_add(L[c], diff_e), H[c]);

                int16_t diff_o = sat16_add(sat16_add(sat16_sub(0, L[c - 1]), L[c + 1]), 4) >> 3;
                int16_t od = sat16_sub(sat16_add(L[c], diff_o), H[c]);

                row_out[2 * c]     = sat16(int32_t(ev) * 2);
                row_out[2 * c + 1] = sat16(int32_t(od) * 2);
            }
        }

        // Right border
        int c = width - 1;
        int32_t right_even = ((5  * L[c] + 4 * L[c - 1] - L[c - 2] + 4) >> 3) + H[c];
        int32_t right_odd  = ((11 * L[c] - 4 * L[c - 1] + L[c - 2] + 4) >> 3) - H[c];
        if (descale == 0) {
            row_out[2 * c]     = sat16(right_even >> 1);
            row_out[2 * c + 1] = sat16(right_odd >> 1);
        } else {
            row_out[2 * c]     = sat16(right_even * 2);
            row_out[2 * c + 1] = sat16(right_odd * 2);
        }
    };

    for (int r = 0; r < height; ++r) {
        horiz_row(&even_low[r * width], &even_high[r * width], &out[(2 * r) * out_stride]);
        horiz_row(&odd_low[r * width], &odd_high[r * width], &out[(2 * r + 1) * out_stride]);
    }
}

// Synthetic pattern generator for extreme value and border tests
enum TestPatternType {
    PATTERN_ZEROS,
    PATTERN_MAX_POS,
    PATTERN_MIN_NEG,
    PATTERN_NEAR_SAT_POS,
    PATTERN_NEAR_SAT_NEG,
    PATTERN_ALTERNATING_EXTREMES,
    PATTERN_IMPULSE_CENTER,
    PATTERN_IMPULSE_CORNERS,
    PATTERN_IMPULSE_BORDERS,
    PATTERN_FLAT_VAL,
    PATTERN_HIGHPASS_IMPULSE,
    PATTERN_LL_IMPULSE
};

void GenerateSyntheticBands(
    TestPatternType type,
    int width, int height,
    std::vector<int16_t>& ll,
    std::vector<int16_t>& lh,
    std::vector<int16_t>& hl,
    std::vector<int16_t>& hh)
{
    size_t count = size_t(width) * height;
    ll.assign(count, 0);
    lh.assign(count, 0);
    hl.assign(count, 0);
    hh.assign(count, 0);

    switch (type) {
    case PATTERN_ZEROS:
        break;
    case PATTERN_MAX_POS:
        ll.assign(count, 32767);
        lh.assign(count, 32767);
        hl.assign(count, 32767);
        hh.assign(count, 32767);
        break;
    case PATTERN_MIN_NEG:
        ll.assign(count, -32768);
        lh.assign(count, -32768);
        hl.assign(count, -32768);
        hh.assign(count, -32768);
        break;
    case PATTERN_NEAR_SAT_POS:
        ll.assign(count, 32760);
        lh.assign(count, 32760);
        hl.assign(count, 32760);
        hh.assign(count, 32760);
        break;
    case PATTERN_NEAR_SAT_NEG:
        ll.assign(count, -32760);
        lh.assign(count, -32760);
        hl.assign(count, -32760);
        hh.assign(count, -32760);
        break;
    case PATTERN_ALTERNATING_EXTREMES:
        for (size_t i = 0; i < count; ++i) {
            int16_t v = (i % 2 == 0) ? 32767 : -32768;
            ll[i] = v;
            lh[i] = -v;
            hl[i] = v;
            hh[i] = -v;
        }
        break;
    case PATTERN_IMPULSE_CENTER: {
        int cy = height / 2;
        int cx = width / 2;
        ll[cy * width + cx] = 32000;
        lh[cy * width + cx] = -32000;
        hl[cy * width + cx] = 32000;
        hh[cy * width + cx] = -32000;
        break;
    }
    case PATTERN_IMPULSE_CORNERS: {
        // (0,0), (0, w-1), (h-1, 0), (h-1, w-1)
        int corners[4][2] = {{0, 0}, {0, width - 1}, {height - 1, 0}, {height - 1, width - 1}};
        for (auto& pt : corners) {
            size_t idx = pt[0] * width + pt[1];
            ll[idx] = 32767;
            lh[idx] = -32768;
            hl[idx] = 32767;
            hh[idx] = -32768;
        }
        break;
    }
    case PATTERN_IMPULSE_BORDERS: {
        for (int c = 0; c < width; ++c) {
            ll[0 * width + c] = 30000;
            ll[(height - 1) * width + c] = -30000;
            lh[0 * width + c] = -30000;
            lh[(height - 1) * width + c] = 30000;
        }
        for (int r = 0; r < height; ++r) {
            hl[r * width + 0] = 30000;
            hl[r * width + (width - 1)] = -30000;
            hh[r * width + 0] = -30000;
            hh[r * width + (width - 1)] = 30000;
        }
        break;
    }
    case PATTERN_FLAT_VAL:
        ll.assign(count, 1024);
        break;
    case PATTERN_HIGHPASS_IMPULSE:
        for (size_t i = 0; i < count; i += 7) {
            lh[i] = 16000;
            hl[i] = -16000;
            hh[i] = 24000;
        }
        break;
    case PATTERN_LL_IMPULSE:
        for (size_t i = 0; i < count; i += 5) {
            ll[i] = 20000;
        }
        break;
    }
}

const char* PatternTypeName(TestPatternType t) {
    switch (t) {
    case PATTERN_ZEROS: return "Zeros";
    case PATTERN_MAX_POS: return "MaxPos (INT16_MAX)";
    case PATTERN_MIN_NEG: return "MinNeg (INT16_MIN)";
    case PATTERN_NEAR_SAT_POS: return "NearSatPos";
    case PATTERN_NEAR_SAT_NEG: return "NearSatNeg";
    case PATTERN_ALTERNATING_EXTREMES: return "AlternatingExtremes";
    case PATTERN_IMPULSE_CENTER: return "ImpulseCenter";
    case PATTERN_IMPULSE_CORNERS: return "ImpulseCorners";
    case PATTERN_IMPULSE_BORDERS: return "ImpulseBorders";
    case PATTERN_FLAT_VAL: return "Flat";
    case PATTERN_HIGHPASS_IMPULSE: return "HighpassImpulse";
    case PATTERN_LL_IMPULSE: return "LLImpulse";
    }
    return "Unknown";
}

int RunSyntheticWaveletTest(
    int frame_w, int frame_h,
    TestPatternType pattern_type,
    CudaInverseWaveletPipeline& pipeline)
{
    // Generate synthetic bands for all 3 levels for Y channel (channel 0)
    int ch_w = frame_w;
    int ch_h = frame_h;

    int w_l2 = ch_w / 8, h_l2 = ch_h / 8;
    int w_l1 = ch_w / 4, h_l1 = ch_h / 4;
    int w_l0 = ch_w / 2, h_l0 = ch_h / 2;

    std::vector<int16_t> l2_ll, l2_lh, l2_hl, l2_hh;
    std::vector<int16_t> l1_dummy_ll, l1_lh, l1_hl, l1_hh;
    std::vector<int16_t> l0_dummy_ll, l0_lh, l0_hl, l0_hh;

    GenerateSyntheticBands(pattern_type, w_l2, h_l2, l2_ll, l2_lh, l2_hl, l2_hh);
    GenerateSyntheticBands(pattern_type, w_l1, h_l1, l1_dummy_ll, l1_lh, l1_hl, l1_hh);
    GenerateSyntheticBands(pattern_type, w_l0, h_l0, l0_dummy_ll, l0_lh, l0_hl, l0_hh);

    // Compute CPU reference
    std::vector<int16_t> ref_ll1(w_l1 * h_l1, 0);
    std::vector<int16_t> ref_ll0(w_l0 * h_l0, 0);
    std::vector<int16_t> ref_plane(ch_w * ch_h, 0);

    // Level 2 -> LL1 (descale = 0)
    CpuInvertSpatial(
        l2_ll.data(), w_l2 * sizeof(int16_t),
        l2_lh.data(), w_l2 * sizeof(int16_t),
        l2_hl.data(), w_l2 * sizeof(int16_t),
        l2_hh.data(), w_l2 * sizeof(int16_t),
        ref_ll1.data(), w_l1 * sizeof(int16_t),
        w_l2, h_l2, 0
    );

    // Level 1 -> LL0 (descale = 2)
    CpuInvertSpatial(
        ref_ll1.data(), w_l1 * sizeof(int16_t),
        l1_lh.data(), w_l1 * sizeof(int16_t),
        l1_hl.data(), w_l1 * sizeof(int16_t),
        l1_hh.data(), w_l1 * sizeof(int16_t),
        ref_ll0.data(), w_l0 * sizeof(int16_t),
        w_l1, h_l1, 2
    );

    // Level 0 -> Reconstructed Plane (descale = 0)
    CpuInvertSpatial(
        ref_ll0.data(), w_l0 * sizeof(int16_t),
        l0_lh.data(), w_l0 * sizeof(int16_t),
        l0_hl.data(), w_l0 * sizeof(int16_t),
        l0_hh.data(), w_l0 * sizeof(int16_t),
        ref_plane.data(), ch_w * sizeof(int16_t),
        w_l0, h_l0, 0
    );

    // Prepare descriptors for GPU
    std::vector<DecodeBandDescriptor> bands;

    auto add_band = [&](int ch, int level, int band, int w, int h, const int16_t* ptr, int descale) {
        DecodeBandDescriptor desc{};
        desc.channel = ch;
        desc.level = level;
        desc.band = band;
        desc.width = w;
        desc.height = h;
        desc.pitch = w * sizeof(int16_t);
        desc.prescale = (descale == 2) ? 2 : 0;
        desc.descale = descale;
        desc.precision = 10;
        desc.host_data = ptr;
        bands.push_back(desc);
    };

    // For all 3 channels to satisfy pipeline
    for (int ch = 0; ch < 3; ++ch) {
        int w2 = (ch == 0) ? w_l2 : (w_l2 / 2);
        int h2 = h_l2;
        int w1 = (ch == 0) ? w_l1 : (w_l1 / 2);
        int h1 = h_l1;
        int w0 = (ch == 0) ? w_l0 : (w_l0 / 2);
        int h0 = h_l0;

        if (ch == 0) {
            add_band(0, 2, 0, w2, h2, l2_ll.data(), 0);
            add_band(0, 2, 1, w2, h2, l2_lh.data(), 0);
            add_band(0, 2, 2, w2, h2, l2_hl.data(), 0);
            add_band(0, 2, 3, w2, h2, l2_hh.data(), 0);

            add_band(0, 1, 1, w1, h1, l1_lh.data(), 2);
            add_band(0, 1, 2, w1, h1, l1_hl.data(), 2);
            add_band(0, 1, 3, w1, h1, l1_hh.data(), 2);

            add_band(0, 0, 1, w0, h0, l0_lh.data(), 0);
            add_band(0, 0, 2, w0, h0, l0_hl.data(), 0);
            add_band(0, 0, 3, w0, h0, l0_hh.data(), 0);
        } else {
            // For U/V channels, allocate zeros
            static std::vector<int16_t> zero_buf(ch_w * ch_h, 0);
            add_band(ch, 2, 0, w2, h2, zero_buf.data(), 0);
            add_band(ch, 2, 1, w2, h2, zero_buf.data(), 0);
            add_band(ch, 2, 2, w2, h2, zero_buf.data(), 0);
            add_band(ch, 2, 3, w2, h2, zero_buf.data(), 0);

            add_band(ch, 1, 1, w1, h1, zero_buf.data(), 2);
            add_band(ch, 1, 2, w1, h1, zero_buf.data(), 2);
            add_band(ch, 1, 3, w1, h1, zero_buf.data(), 2);

            add_band(ch, 0, 1, w0, h0, zero_buf.data(), 0);
            add_band(ch, 0, 2, w0, h0, zero_buf.data(), 0);
            add_band(ch, 0, 3, w0, h0, zero_buf.data(), 0);
        }
    }

    if (!pipeline.UploadCoefficients(bands)) {
        std::cerr << "UploadCoefficients failed for " << PatternTypeName(pattern_type) << std::endl;
        return 1;
    }

    if (!pipeline.Reconstruct()) {
        std::cerr << "Reconstruct failed for " << PatternTypeName(pattern_type) << std::endl;
        return 1;
    }

    // Verify LL1 intermediate
    std::vector<int16_t> gpu_ll1(w_l1 * h_l1, 0);
    if (!pipeline.DownloadIntermediateLL(1, 0, gpu_ll1.data(), w_l1 * sizeof(int16_t))) {
        std::cerr << "DownloadIntermediateLL(1) failed" << std::endl;
        return 1;
    }
    for (size_t i = 0; i < ref_ll1.size(); ++i) {
        if (ref_ll1[i] != gpu_ll1[i]) {
            std::cerr << "LL1 mismatch in pattern " << PatternTypeName(pattern_type)
                      << " at index " << i << ": CPU=" << ref_ll1[i] << " GPU=" << gpu_ll1[i] << std::endl;
            return 1;
        }
    }

    // Verify LL0 intermediate
    std::vector<int16_t> gpu_ll0(w_l0 * h_l0, 0);
    if (!pipeline.DownloadIntermediateLL(0, 0, gpu_ll0.data(), w_l0 * sizeof(int16_t))) {
        std::cerr << "DownloadIntermediateLL(0) failed" << std::endl;
        return 1;
    }
    for (size_t i = 0; i < ref_ll0.size(); ++i) {
        if (ref_ll0[i] != gpu_ll0[i]) {
            std::cerr << "LL0 mismatch in pattern " << PatternTypeName(pattern_type)
                      << " at index " << i << ": CPU=" << ref_ll0[i] << " GPU=" << gpu_ll0[i] << std::endl;
            return 1;
        }
    }

    // Verify final reconstructed plane
    std::vector<int16_t> gpu_plane_y(ch_w * ch_h, 0);
    std::vector<int16_t> dummy_v((ch_w / 2) * ch_h, 0);
    std::vector<int16_t> dummy_u((ch_w / 2) * ch_h, 0);

    if (!pipeline.DownloadPlanes(
        gpu_plane_y.data(), ch_w * sizeof(int16_t),
        dummy_v.data(), (ch_w / 2) * sizeof(int16_t),
        dummy_u.data(), (ch_w / 2) * sizeof(int16_t)))
    {
        std::cerr << "DownloadPlanes failed" << std::endl;
        return 1;
    }

    for (size_t i = 0; i < ref_plane.size(); ++i) {
        if (ref_plane[i] != gpu_plane_y[i]) {
            std::cerr << "Final plane mismatch in pattern " << PatternTypeName(pattern_type)
                      << " at index " << i << ": CPU=" << ref_plane[i] << " GPU=" << gpu_plane_y[i] << std::endl;
            return 1;
        }
    }

    return 0;
}

int main() {
    try {
        std::cout << "=== Running HyperCine Inverse Spatial Wavelet CUDA Unit Tests ===" << std::endl;

        // Test geometry: 1920x1080 (standard), and small minimal legal frame: 128x64
        struct GeometryTest {
            int w, h;
            const char* desc;
        } geoms[] = {
            {1920, 1080, "1080p Standard"},
            {256, 128, "Minimal Legal Geometry (256x128)"},
            {3840, 2160, "4K UHD Geometry"}
        };

        for (const auto& g : geoms) {
            std::cout << "\nTesting Geometry: " << g.desc << " (" << g.w << "x" << g.h << ")" << std::endl;

            CudaInverseWaveletPipeline pipeline;
            if (!pipeline.Initialize(g.w, g.h, 10)) {
                std::cerr << "Failed to initialize CUDA pipeline for " << g.w << "x" << g.h << std::endl;
                return 1;
            }

            TestPatternType patterns[] = {
                PATTERN_ZEROS,
                PATTERN_MAX_POS,
                PATTERN_MIN_NEG,
                PATTERN_NEAR_SAT_POS,
                PATTERN_NEAR_SAT_NEG,
                PATTERN_ALTERNATING_EXTREMES,
                PATTERN_IMPULSE_CENTER,
                PATTERN_IMPULSE_CORNERS,
                PATTERN_IMPULSE_BORDERS,
                PATTERN_FLAT_VAL,
                PATTERN_HIGHPASS_IMPULSE,
                PATTERN_LL_IMPULSE
            };

            for (auto p : patterns) {
                int res = RunSyntheticWaveletTest(g.w, g.h, p, pipeline);
                if (res != 0) {
                    std::cerr << "FAIL on " << PatternTypeName(p) << " in " << g.desc << std::endl;
                    return 1;
                }
                std::cout << "  Pattern: " << std::left << std::setw(25) << PatternTypeName(p) << " [PASS - Exact Match (0 diff)]" << std::endl;
            }
        }

        std::cout << "\nAll 36 synthetic extreme-value and border tests PASSED with 0 differences!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
}
