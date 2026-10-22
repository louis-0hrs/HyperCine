// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include "cpu_isa.h"
#include "hypercine/wavelet.h"

namespace hypercine { namespace detail {
// Experimental explicit kernel seam. Auto remains scalar; this does not
// register a validated production codec backend or change public dispatch.
bool WaveletSimdAvailable(CpuIsa isa) noexcept;
HyperCineStatus ForwardHorizontal26(CpuIsa isa, const int16_t* src,
    int width, int height, int src_stride, int16_t* low, int low_stride,
    int16_t* high, int high_stride);
HyperCineStatus ForwardVertical26(CpuIsa isa, const int16_t* src,
    int width, int height, int src_stride, int16_t* low, int low_stride,
    int16_t* high, int high_stride);
}} // namespace hypercine::detail
