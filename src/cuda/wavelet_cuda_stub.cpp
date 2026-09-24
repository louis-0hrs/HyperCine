// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#include "hypercine/wavelet.h"

bool HyperCine_IsCUDAAvailable(void) {
    return false;
}

HyperCineStatus HyperCine_GetCUDADeviceInfo(HyperCineCUDADeviceInfo* info) {
    (void)info;
    return HYPERCINE_STATUS_NOT_SUPPORTED;
}

HyperCineStatus HyperCine_ForwardHorizontal26_CUDA(
    HyperCineTransformBackend backend,
    const int16_t* d_src, int width, int height, int src_stride,
    int16_t* d_low, int low_stride,
    int16_t* d_high, int high_stride,
    void* cuda_stream
) {
    (void)backend; (void)d_src; (void)width; (void)height; (void)src_stride;
    (void)d_low; (void)low_stride; (void)d_high; (void)high_stride; (void)cuda_stream;
    return HYPERCINE_STATUS_NOT_SUPPORTED;
}

HyperCineStatus HyperCine_ForwardVertical26_CUDA(
    HyperCineTransformBackend backend,
    const int16_t* d_src, int width, int height, int src_stride,
    int16_t* d_low, int low_stride,
    int16_t* d_high, int high_stride,
    void* cuda_stream
) {
    (void)backend; (void)d_src; (void)width; (void)height; (void)src_stride;
    (void)d_low; (void)low_stride; (void)d_high; (void)high_stride; (void)cuda_stream;
    return HYPERCINE_STATUS_NOT_SUPPORTED;
}

HyperCineStatus HyperCine_ForwardSpatial2D_CUDA(
    HyperCineTransformBackend backend,
    const int16_t* d_src, int width, int height, int src_stride,
    int16_t* d_ll, int ll_stride,
    int16_t* d_lh, int lh_stride,
    int16_t* d_hl, int hl_stride,
    int16_t* d_hh, int hh_stride,
    void* cuda_stream
) {
    (void)backend; (void)d_src; (void)width; (void)height; (void)src_stride;
    (void)d_ll; (void)ll_stride; (void)d_lh; (void)lh_stride;
    (void)d_hl; (void)hl_stride; (void)d_hh; (void)hh_stride; (void)cuda_stream;
    return HYPERCINE_STATUS_NOT_SUPPORTED;
}

HyperCineStatus HyperCine_InverseVertical26_CUDA(
    HyperCineTransformBackend backend,
    const int16_t* d_low, int low_stride,
    const int16_t* d_high, int high_stride,
    int width, int height,
    int16_t* d_dst, int dst_stride,
    void* cuda_stream
) {
    (void)backend; (void)d_low; (void)low_stride; (void)d_high; (void)high_stride;
    (void)width; (void)height; (void)d_dst; (void)dst_stride; (void)cuda_stream;
    return HYPERCINE_STATUS_NOT_SUPPORTED;
}

HyperCineStatus HyperCine_InverseHorizontal26_CUDA(
    HyperCineTransformBackend backend,
    const int16_t* d_low, int low_stride,
    const int16_t* d_high, int high_stride,
    int width, int height,
    int16_t* d_dst, int dst_stride,
    void* cuda_stream
) {
    (void)backend; (void)d_low; (void)low_stride; (void)d_high; (void)high_stride;
    (void)width; (void)height; (void)d_dst; (void)dst_stride; (void)cuda_stream;
    return HYPERCINE_STATUS_NOT_SUPPORTED;
}

HyperCineStatus HyperCine_InverseSpatial2D_CUDA(
    HyperCineTransformBackend backend,
    const int16_t* d_ll, int ll_stride,
    const int16_t* d_lh, int lh_stride,
    const int16_t* d_hl, int hl_stride,
    const int16_t* d_hh, int hh_stride,
    int subband_width, int subband_height,
    int16_t* d_dst, int dst_stride,
    void* cuda_stream
) {
    (void)backend; (void)d_ll; (void)ll_stride; (void)d_lh; (void)lh_stride;
    (void)d_hl; (void)hl_stride; (void)d_hh; (void)hh_stride;
    (void)subband_width; (void)subband_height; (void)d_dst; (void)dst_stride; (void)cuda_stream;
    return HYPERCINE_STATUS_NOT_SUPPORTED;
}
