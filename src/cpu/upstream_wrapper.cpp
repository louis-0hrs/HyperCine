// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/wavelet.h"
#include <vector>

extern "C" {
#include "spatial.h"
#include "wavelet.h"
}

HyperCineStatus HyperCine_ForwardSpatial2D_Upstream(
    const int16_t* src, int width, int height, int src_stride,
    int16_t* ll, int ll_stride,
    int16_t* lh, int lh_stride,
    int16_t* hl, int hl_stride,
    int16_t* hh, int hh_stride,
    int quant_ll, int quant_lh, int quant_hl, int quant_hh
) {
    if (!src || !ll || !lh || !hl || !hh) return HYPERCINE_STATUS_INVALID_ARGUMENT;

    const int half_w = width / 2;
    const int half_h = height / 2;
    
    // Allocate buffer for upstream optimized transform
    size_t buf_size = ForwardSpatialBufferSize(width);
    std::vector<int16_t> buffer(buf_size / sizeof(int16_t));
    
    ROI roi = { width, height };
    int quant[4] = {quant_ll, quant_lh, quant_hl, quant_hh};

    FilterSpatialQuant16s(
        const_cast<int16_t*>(src), src_stride * sizeof(int16_t),
        ll, ll_stride * sizeof(int16_t),
        lh, lh_stride * sizeof(int16_t),
        hl, hl_stride * sizeof(int16_t),
        hh, hh_stride * sizeof(int16_t),
        buffer.data(), buf_size,
        roi, quant
    );

    return HYPERCINE_STATUS_SUCCESS;
}
