// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifndef HYPERCINE_WAVELET_CUDA_INTERNAL_H
#define HYPERCINE_WAVELET_CUDA_INTERNAL_H

#include <cuda_runtime.h>
#include <stdint.h>
#include "hypercine/wavelet.h"

// Reference kernels

HyperCineStatus ForwardVerticalQuant26_Reference(
    const int16_t* d_low, int low_stride,
    const int16_t* d_high, int high_stride,
    int width, int height,
    int16_t* d_ll, int ll_stride,
    int16_t* d_lh, int lh_stride,
    int16_t* d_hl, int hl_stride,
    int16_t* d_hh, int hh_stride,
    int quant_ll, int quant_lh, int quant_hl, int quant_hh,
    int midpoint,
    cudaStream_t stream
);

HyperCineStatus ForwardHorizontal26_Reference(
    const int16_t* d_src, int width, int height, int src_stride,
    int16_t* d_low, int low_stride,
    int16_t* d_high, int high_stride,
    cudaStream_t stream
);

HyperCineStatus ForwardVertical26_Reference(
    const int16_t* d_src, int width, int height, int src_stride,
    int16_t* d_low, int low_stride,
    int16_t* d_high, int high_stride,
    cudaStream_t stream
);

HyperCineStatus InverseVertical26_Reference(
    const int16_t* d_low, int low_stride,
    const int16_t* d_high, int high_stride,
    int width, int height,
    int16_t* d_dst, int dst_stride,
    cudaStream_t stream
);

HyperCineStatus InverseHorizontal26_Reference(
    const int16_t* d_low, int low_stride,
    const int16_t* d_high, int high_stride,
    int width, int height,
    int16_t* d_dst, int dst_stride,
    cudaStream_t stream
);

// Optimized kernels
HyperCineStatus ForwardHorizontal26_Optimized(
    const int16_t* d_src, int width, int height, int src_stride,
    int16_t* d_low, int low_stride,
    int16_t* d_high, int high_stride,
    cudaStream_t stream
);

HyperCineStatus ForwardVertical26_Optimized(
    const int16_t* d_src, int width, int height, int src_stride,
    int16_t* d_low, int low_stride,
    int16_t* d_high, int high_stride,
    cudaStream_t stream
);

HyperCineStatus InverseVertical26_Optimized(
    const int16_t* d_low, int low_stride,
    const int16_t* d_high, int high_stride,
    int width, int height,
    int16_t* d_dst, int dst_stride,
    cudaStream_t stream
);

HyperCineStatus InverseHorizontal26_Optimized(
    const int16_t* d_low, int low_stride,
    const int16_t* d_high, int high_stride,
    int width, int height,
    int16_t* d_dst, int dst_stride,
    cudaStream_t stream
);

#endif // HYPERCINE_WAVELET_CUDA_INTERNAL_H
