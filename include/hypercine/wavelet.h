// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.

#ifndef HYPERCINE_WAVELET_H
#define HYPERCINE_WAVELET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Status and error codes returned by HyperCine wavelet transforms.
 */
typedef enum {
    HYPERCINE_STATUS_SUCCESS = 0,
    HYPERCINE_STATUS_INVALID_ARGUMENT = -1,
    HYPERCINE_STATUS_OUT_OF_MEMORY = -2,
    HYPERCINE_STATUS_CUDA_ERROR = -3,
    HYPERCINE_STATUS_NOT_SUPPORTED = -4
} HyperCineStatus;

/**
 * @brief Execution backend selection for wavelet transforms.
 */
typedef enum {
    HYPERCINE_BACKEND_CPU = 0,
    HYPERCINE_BACKEND_CUDA_REFERENCE = 1,
    HYPERCINE_BACKEND_CUDA_OPTIMIZED = 2
} HyperCineTransformBackend;

/**
 * @brief Query whether CUDA runtime support is compiled and available at runtime.
 */
bool HyperCine_IsCUDAAvailable(void);

/**
 * @brief Device query information.
 */
typedef struct {
    char device_name[256];
    int major;
    int minor;
    int multiprocessor_count;
    size_t total_global_mem;
    int cuda_driver_version;
    int cuda_runtime_version;
} HyperCineCUDADeviceInfo;

/**
 * @brief Get CUDA device information for device 0.
 */
HyperCineStatus HyperCine_GetCUDADeviceInfo(HyperCineCUDADeviceInfo* info);

/* ========================================================================= */
/* CPU Reference Wavelet Transform Functions                                 */
/* ========================================================================= */

/**
 * @brief Production 1D Horizontal 2-6 Forward Wavelet Transform (CPU Reference).
 *
 * Decomposes each row of length @p width into lowpass and highpass rows of length @p width / 2.
 * Conforms bit-for-bit with upstream GoPro CineForm `FilterHorizontalRow16s`.
 *
 * @param src         Pointer to input pixel row(s) (16-bit signed).
 * @param width       Input width (must be even and >= 6).
 * @param height      Number of rows to transform.
 * @param src_stride  Input stride in elements (int16_t).
 * @param lowpass     Output lowpass buffer.
 * @param low_stride  Output lowpass stride in elements.
 * @param highpass    Output highpass buffer.
 * @param high_stride Output highpass stride in elements.
 */
HyperCineStatus HyperCine_ForwardHorizontal26_CPU(
    const int16_t* src, int width, int height, int src_stride,
    int16_t* lowpass, int low_stride,
    int16_t* highpass, int high_stride
);

/**
 * @brief Production 1D Vertical 2-6 Forward Wavelet Transform (CPU Reference).
 *
 * Decomposes each column of height @p height into lowpass and highpass columns of height @p height / 2.
 *
 * @param src         Pointer to input column(s) (16-bit signed).
 * @param width       Width of the buffer in elements.
 * @param height      Input height (must be even and >= 6).
 * @param src_stride  Input stride in elements.
 * @param lowpass     Output lowpass buffer.
 * @param low_stride  Output lowpass stride in elements.
 * @param highpass    Output highpass buffer.
 * @param high_stride Output highpass stride in elements.
 */
HyperCineStatus HyperCine_ForwardVertical26_CPU(
    const int16_t* src, int width, int height, int src_stride,
    int16_t* lowpass, int low_stride,
    int16_t* highpass, int high_stride
);

/**
 * @brief Production 2D 1-Level 2-6 Spatial Forward Wavelet Transform (CPU Reference).
 *
 * Applies horizontal transform followed by vertical transform to produce 4 subbands:
 * LL (lowlow), LH (lowhigh), HL (highlow), HH (highhigh).
 */
HyperCineStatus HyperCine_ForwardSpatial2D_CPU(
    const int16_t* src, int width, int height, int src_stride,
    int16_t* ll, int ll_stride,
    int16_t* lh, int lh_stride,
    int16_t* hl, int hl_stride,
    int16_t* hh, int hh_stride
);

/**
 * @brief Production 1D Vertical 2-6 Inverse Wavelet Transform (CPU Reference).
 *
 * Synthesizes two subbands of height @p height into an output of height 2 * @p height.
 * Conforms bit-for-bit with upstream `InvertVertical16s`.
 */
HyperCineStatus HyperCine_InverseVertical26_CPU(
    const int16_t* lowpass, int low_stride,
    const int16_t* highpass, int high_stride,
    int width, int height,
    int16_t* dst, int dst_stride
);

/**
 * @brief Production 1D Horizontal 2-6 Inverse Wavelet Transform (CPU Reference).
 *
 * Synthesizes two subbands of width @p width into an output of width 2 * @p width.
 * Conforms bit-for-bit with upstream `InvertHorizontalRow16s`.
 */
HyperCineStatus HyperCine_InverseHorizontal26_CPU(
    const int16_t* lowpass, int low_stride,
    const int16_t* highpass, int high_stride,
    int width, int height,
    int16_t* dst, int dst_stride
);

/**
 * @brief Production 2D 1-Level 2-6 Spatial Inverse Wavelet Transform (CPU Reference).
 *
 * Synthesizes 4 subbands (LL, LH, HL, HH) of dimensions (subband_width x subband_height)
 * into a reconstructed frame of dimensions (2*subband_width x 2*subband_height).
 */
HyperCineStatus HyperCine_InverseSpatial2D_CPU(
    const int16_t* ll, int ll_stride,
    const int16_t* lh, int lh_stride,
    const int16_t* hl, int hl_stride,
    const int16_t* hh, int hh_stride,
    int subband_width, int subband_height,
    int16_t* dst, int dst_stride
);

/* ========================================================================= */
/* CUDA Wavelet Transform Functions (Device Memory Pointers)                 */
/* ========================================================================= */

/**
 * @brief CUDA 1D Horizontal 2-6 Forward Wavelet Transform.
 * Operates on GPU device pointers asynchronously on the specified CUDA stream.
 */
HyperCineStatus HyperCine_ForwardHorizontal26_CUDA(
    HyperCineTransformBackend backend,
    const int16_t* d_src, int width, int height, int src_stride,
    int16_t* d_low, int low_stride,
    int16_t* d_high, int high_stride,
    void* cuda_stream
);

/**
 * @brief CUDA 1D Vertical 2-6 Forward Wavelet Transform.
 */
HyperCineStatus HyperCine_ForwardVertical26_CUDA(
    HyperCineTransformBackend backend,
    const int16_t* d_src, int width, int height, int src_stride,
    int16_t* d_low, int low_stride,
    int16_t* d_high, int high_stride,
    void* cuda_stream
);

/**
 * @brief CUDA 2D 1-Level 2-6 Spatial Forward Wavelet Transform.
 */
HyperCineStatus HyperCine_ForwardSpatial2D_CUDA(
    HyperCineTransformBackend backend,
    const int16_t* d_src, int width, int height, int src_stride,
    int16_t* d_ll, int ll_stride,
    int16_t* d_lh, int lh_stride,
    int16_t* d_hl, int hl_stride,
    int16_t* d_hh, int hh_stride,
    void* cuda_stream
);

/**
 * @brief CUDA 1D Vertical 2-6 Inverse Wavelet Transform.
 */
HyperCineStatus HyperCine_InverseVertical26_CUDA(
    HyperCineTransformBackend backend,
    const int16_t* d_low, int low_stride,
    const int16_t* d_high, int high_stride,
    int width, int height,
    int16_t* d_dst, int dst_stride,
    void* cuda_stream
);

/**
 * @brief CUDA 1D Horizontal 2-6 Inverse Wavelet Transform.
 */
HyperCineStatus HyperCine_InverseHorizontal26_CUDA(
    HyperCineTransformBackend backend,
    const int16_t* d_low, int low_stride,
    const int16_t* d_high, int high_stride,
    int width, int height,
    int16_t* d_dst, int dst_stride,
    void* cuda_stream
);

/**
 * @brief CUDA 2D 1-Level 2-6 Spatial Inverse Wavelet Transform.
 */
HyperCineStatus HyperCine_InverseSpatial2D_CUDA(
    HyperCineTransformBackend backend,
    const int16_t* d_ll, int ll_stride,
    const int16_t* d_lh, int lh_stride,
    const int16_t* d_hl, int hl_stride,
    const int16_t* d_hh, int hh_stride,
    int subband_width, int subband_height,
    int16_t* d_dst, int dst_stride,
    void* cuda_stream
);

#ifdef __cplusplus
}
#endif

#endif /* HYPERCINE_WAVELET_H */
