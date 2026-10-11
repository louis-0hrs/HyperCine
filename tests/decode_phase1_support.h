// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include "hypercine/decode_inverse_cuda.h"
#include "decode_phase0_support.h"

extern "C" {
#include "spatial.h"
#include "wavelet.h"
}

namespace HyperCine {
namespace DecodePhase1 {

inline std::vector<DecodeBandDescriptor> ExtractBandsFromNative(DECODER* d) {
    std::vector<DecodeBandDescriptor> bands;
    int precision = d->codec.precision;

    for (int ch = 0; ch < 3; ++ch) {
        auto* t = d->transform[ch];

        // Level 2 (bands 0, 1, 2, 3)
        auto* w2 = t->wavelet[2];
        for (int b = 0; b < 4; ++b) {
            DecodeBandDescriptor desc{};
            desc.channel = ch;
            desc.level = 2;
            desc.band = b;
            desc.width = w2->width;
            desc.height = w2->height;
            desc.pitch = w2->pitch;
            desc.prescale = 0;
            desc.descale = 0;
            desc.precision = precision;
            desc.host_data = reinterpret_cast<const int16_t*>(w2->band[b]);
            bands.push_back(desc);
        }

        // Level 1 (highpass bands 1, 2, 3; band 0 is intermediate LL)
        auto* w1 = t->wavelet[1];
        for (int b = 1; b < 4; ++b) {
            DecodeBandDescriptor desc{};
            desc.channel = ch;
            desc.level = 1;
            desc.band = b;
            desc.width = w1->width;
            desc.height = w1->height;
            desc.pitch = w1->pitch;
            desc.prescale = 2;
            desc.descale = 2;
            desc.precision = precision;
            desc.host_data = reinterpret_cast<const int16_t*>(w1->band[b]);
            bands.push_back(desc);
        }

        // Level 0 (highpass bands 1, 2, 3; band 0 is intermediate LL)
        auto* w0 = t->wavelet[0];
        for (int b = 1; b < 4; ++b) {
            DecodeBandDescriptor desc{};
            desc.channel = ch;
            desc.level = 0;
            desc.band = b;
            desc.width = w0->width;
            desc.height = w0->height;
            desc.pitch = w0->pitch;
            desc.prescale = 0;
            desc.descale = 0;
            desc.precision = precision;
            desc.host_data = reinterpret_cast<const int16_t*>(w0->band[b]);
            bands.push_back(desc);
        }
    }
    return bands;
}

struct CpuIntermediateReference {
    // LL1 intermediate (output of Level 2): [channel][samples]
    std::vector<int16_t> ll1[3];
    int ll1_width[3];
    int ll1_height[3];

    // LL0 intermediate (output of Level 1): [channel][samples]
    std::vector<int16_t> ll0[3];
    int ll0_width[3];
    int ll0_height[3];

    // Final reconstructed planar channel (output of Level 0): [channel][samples]
    std::vector<int16_t> plane[3];
    int plane_width[3];
    int plane_height[3];
};

inline CpuIntermediateReference ComputeCpuReferenceIntermediates(DECODER* d) {
    CpuIntermediateReference ref;

    for (int ch = 0; ch < 3; ++ch) {
        auto* t = d->transform[ch];

        // 1. Level 2 -> Level 1 LL
        auto* w2 = t->wavelet[2];
        auto* w1 = t->wavelet[1];
        ref.ll1_width[ch] = w1->width;
        ref.ll1_height[ch] = w1->height;
        ref.ll1[ch].assign(w1->width * w1->height, 0);

        std::vector<PIXEL> scratch1(w1->width * 32, 0);
        ROI roi2 = { w2->width, w2->height };
        InvertSpatialQuant16s(
            w2->band[0], w2->pitch,
            w2->band[1], w2->pitch,
            w2->band[2], w2->pitch,
            w2->band[3], w2->pitch,
            ref.ll1[ch].data(), w1->width * sizeof(PIXEL),
            roi2, scratch1.data(), scratch1.size() * sizeof(PIXEL),
            w2->quantization
        );

        // 2. Level 1 -> Level 0 LL
        auto* w0 = t->wavelet[0];
        ref.ll0_width[ch] = w0->width;
        ref.ll0_height[ch] = w0->height;
        ref.ll0[ch].assign(w0->width * w0->height, 0);

        std::vector<PIXEL> scratch0(w0->width * 32, 0);
        ROI roi1 = { w1->width, w1->height };
        InvertSpatialQuantDescale16s(
            ref.ll1[ch].data(), w1->width * sizeof(PIXEL),
            w1->band[1], w1->pitch,
            w1->band[2], w1->pitch,
            w1->band[3], w1->pitch,
            ref.ll0[ch].data(), w0->width * sizeof(PIXEL),
            roi1, scratch0.data(), scratch0.size() * sizeof(PIXEL),
            2, w1->quantization
        );

        // 3. Level 0 -> Final Reconstructed Plane
        int ch_w = 2 * w0->width;
        int ch_h = 2 * w0->height;
        ref.plane_width[ch] = ch_w;
        ref.plane_height[ch] = ch_h;
        ref.plane[ch].assign(ch_w * ch_h, 0);

        std::vector<PIXEL> scratch_plane(ch_w * 32, 0);
        ROI roi0 = { w0->width, w0->height };
        InvertSpatialQuant16s(
            ref.ll0[ch].data(), w0->width * sizeof(PIXEL),
            w0->band[1], w0->pitch,
            w0->band[2], w0->pitch,
            w0->band[3], w0->pitch,
            ref.plane[ch].data(), ch_w * sizeof(PIXEL),
            roi0, scratch_plane.data(), scratch_plane.size() * sizeof(PIXEL),
            w0->quantization
        );
    }

    return ref;
}

inline void ConvertPlanarToYU64(
    const int16_t* plane_y, int y_pitch,
    const int16_t* plane_v, int v_pitch,
    const int16_t* plane_u, int u_pitch,
    int width, int height, int precision,
    uint8_t* out_yu64, int out_pitch)
{
    int upshift = 16 - precision;
    // Upstream saturates to the active precision, then shifts. Clamping after
    // the shift maps 1024 to 65535 instead of the upstream value 65472.
    const int limit = precision >= 16 ? 65535 : (1 << precision) - 1;
    auto expand = [&](int value) {
        if (value < 0) value = 0;
        else if (value > limit) value = limit;
        return value << upshift;
    };
    int y_stride = y_pitch / sizeof(int16_t);
    int v_stride = v_pitch / sizeof(int16_t);
    int u_stride = u_pitch / sizeof(int16_t);

    for (int y = 0; y < height; ++y) {
        const int16_t* row_y = &plane_y[y * y_stride];
        const int16_t* row_v = &plane_v[y * v_stride];
        const int16_t* row_u = &plane_u[y * u_stride];
        uint16_t* out_row = reinterpret_cast<uint16_t*>(out_yu64 + y * out_pitch);

        for (int x = 0; x < width; x += 2) {
            int y0 = expand(row_y[x]);
            int y1 = expand(row_y[x + 1]);
            int v = expand(row_v[x / 2]);
            int u = expand(row_u[x / 2]);

            // YU64 native little-endian words: Y0, V, Y1, U
            out_row[x * 2 + 0] = static_cast<uint16_t>(y0);
            out_row[x * 2 + 1] = static_cast<uint16_t>(v);
            out_row[x * 2 + 2] = static_cast<uint16_t>(y1);
            out_row[x * 2 + 3] = static_cast<uint16_t>(u);
        }
    }
}

} // namespace DecodePhase1
} // namespace HyperCine
